/*
 * Copyright (c) 2019-2022, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cudf/column/column.hpp>
#include <cudf/column/column_device_view.cuh>
#include <cudf/column/column_factories.hpp>
#include <cudf/detail/copy.hpp>
#include <cudf/detail/get_value.cuh>
#include <cudf/detail/iterator.cuh>
#include <cudf/detail/null_mask.hpp>
#include <cudf/detail/nvtx/ranges.hpp>
#include <cudf/dictionary/detail/search.hpp>
#include <cudf/dictionary/detail/update_keys.hpp>
#include <cudf/dictionary/dictionary_column_view.hpp>
#include <cudf/dictionary/dictionary_factories.hpp>
#include <cudf/replace.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/scalar/scalar_device_view.cuh>
#include <cudf/strings/detail/utilities.cuh>
#include <cudf/strings/detail/utilities.hpp>
#include <cudf/strings/strings_column_view.hpp>
#include <cudf/types.hpp>
#include <cudf/utilities/default_stream.hpp>

#include <rmm/cuda_stream_view.hpp>
#include <rmm/exec_policy.hpp>

#include <thrust/for_each.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/transform_iterator.h>
#include <thrust/iterator/zip_iterator.h>
#include <thrust/transform.h>
#include <thrust/tuple.h>

namespace cudf {
namespace detail {
namespace {
template <typename Transformer>
std::pair<std::unique_ptr<column>, std::unique_ptr<column>> form_offsets_and_char_column(
  cudf::column_device_view input,
  size_type,
  Transformer offsets_transformer,
  rmm::cuda_stream_view stream,
  rmm::mr::device_memory_resource* mr)
{
  std::unique_ptr<column> offsets_column{};
  auto strings_count = input.size();

  if (input.nullable()) {
    auto input_begin =
      cudf::detail::make_null_replacement_iterator<string_view>(input, string_view{});
    auto offsets_transformer_itr =
      thrust::make_transform_iterator(input_begin, offsets_transformer);
    offsets_column = cudf::strings::detail::make_offsets_child_column(
      offsets_transformer_itr, offsets_transformer_itr + strings_count, stream, mr);
  } else {
    auto offsets_transformer_itr =
      thrust::make_transform_iterator(input.begin<string_view>(), offsets_transformer);
    offsets_column = cudf::strings::detail::make_offsets_child_column(
      offsets_transformer_itr, offsets_transformer_itr + strings_count, stream, mr);
  }

  // build chars column
  auto const bytes =
    cudf::detail::get_value<int32_t>(offsets_column->view(), strings_count, stream);
  auto chars_column = cudf::strings::detail::create_chars_child_column(bytes, stream, mr);

  return std::pair(std::move(offsets_column), std::move(chars_column));
}

template <typename OptionalScalarIterator, typename ReplaceScalarIterator>
std::unique_ptr<cudf::column> clamp_string_column(strings_column_view const& input,
                                                  OptionalScalarIterator lo_itr,
                                                  ReplaceScalarIterator lo_replace_itr,
                                                  OptionalScalarIterator hi_itr,
                                                  ReplaceScalarIterator hi_replace_itr,
                                                  rmm::cuda_stream_view stream,
                                                  rmm::mr::device_memory_resource* mr)
{
  auto input_device_column = column_device_view::create(input.parent(), stream);
  auto d_input             = *input_device_column;
  size_type null_count     = input.null_count();

  // build offset column
  auto offsets_transformer = [lo_itr, hi_itr, lo_replace_itr, hi_replace_itr] __device__(
                               string_view element, bool is_valid = true) {
    const auto d_lo         = (*lo_itr).value_or(element);
    const auto d_hi         = (*hi_itr).value_or(element);
    const auto d_lo_replace = *(*lo_replace_itr);
    const auto d_hi_replace = *(*hi_replace_itr);
    size_type bytes         = 0;

    if (is_valid) {
      if (element < d_lo) {
        bytes = d_lo_replace.size_bytes();
      } else if (d_hi < element) {
        bytes = d_hi_replace.size_bytes();
      } else {
        bytes = element.size_bytes();
      }
    }
    return bytes;
  };

  auto [offsets_column, chars_column] =
    form_offsets_and_char_column(d_input, null_count, offsets_transformer, stream, mr);

  auto d_offsets = offsets_column->view().template data<size_type>();
  auto d_chars   = chars_column->mutable_view().template data<char>();
  // fill in chars
  auto copy_transformer =
    [d_input, lo_itr, hi_itr, lo_replace_itr, hi_replace_itr, d_offsets, d_chars] __device__(
      size_type idx) {
      if (d_input.is_null(idx)) { return; }
      auto input_element      = d_input.element<string_view>(idx);
      const auto d_lo         = (*lo_itr).value_or(input_element);
      const auto d_hi         = (*hi_itr).value_or(input_element);
      const auto d_lo_replace = *(*lo_replace_itr);
      const auto d_hi_replace = *(*hi_replace_itr);

      if (input_element < d_lo) {
        memcpy(d_chars + d_offsets[idx], d_lo_replace.data(), d_lo_replace.size_bytes());
      } else if (d_hi < input_element) {
        memcpy(d_chars + d_offsets[idx], d_hi_replace.data(), d_hi_replace.size_bytes());
      } else {
        memcpy(d_chars + d_offsets[idx], input_element.data(), input_element.size_bytes());
      }
    };

  thrust::for_each_n(rmm::exec_policy(stream),
                     thrust::make_counting_iterator<size_type>(0),
                     input.size(),
                     copy_transformer);

  return make_strings_column(input.size(),
                             std::move(offsets_column),
                             std::move(chars_column),
                             input.null_count(),
                             std::move(cudf::detail::copy_bitmask(input.parent(), stream, mr)));
}

template <typename T, typename OptionalScalarIterator, typename ReplaceScalarIterator>
std::enable_if_t<cudf::is_fixed_width<T>(), std::unique_ptr<cudf::column>> clamper(
  column_view const& input,
  OptionalScalarIterator lo_itr,
  ReplaceScalarIterator lo_replace_itr,
  OptionalScalarIterator hi_itr,
  ReplaceScalarIterator hi_replace_itr,
  rmm::cuda_stream_view stream,
  rmm::mr::device_memory_resource* mr)
{
  auto output =
    detail::allocate_like(input, input.size(), mask_allocation_policy::NEVER, stream, mr);
  // mask will not change
  if (input.nullable()) { output->set_null_mask(copy_bitmask(input), input.null_count()); }

  auto output_device_view =
    cudf::mutable_column_device_view::create(output->mutable_view(), stream);
  auto input_device_view = cudf::column_device_view::create(input, stream);
  auto scalar_zip_itr =
    thrust::make_zip_iterator(thrust::make_tuple(lo_itr, lo_replace_itr, hi_itr, hi_replace_itr));

  auto trans = [] __device__(auto element_optional, auto scalar_tuple) {
    if (element_optional.has_value()) {
      auto lo_optional = thrust::get<0>(scalar_tuple);
      auto hi_optional = thrust::get<2>(scalar_tuple);
      if (lo_optional.has_value() and (*element_optional < *lo_optional)) {
        return *(thrust::get<1>(scalar_tuple));
      } else if (hi_optional.has_value() and (*element_optional > *hi_optional)) {
        return *(thrust::get<3>(scalar_tuple));
      }
    }

    return *element_optional;
  };

  auto input_pair_iterator =
    make_optional_iterator<T>(*input_device_view, nullate::DYNAMIC{input.has_nulls()});
  thrust::transform(rmm::exec_policy(stream),
                    input_pair_iterator,
                    input_pair_iterator + input.size(),
                    scalar_zip_itr,
                    output_device_view->begin<T>(),
                    trans);

  return output;
}

template <typename T, typename OptionalScalarIterator, typename ReplaceScalarIterator>
std::enable_if_t<std::is_same_v<T, string_view>, std::unique_ptr<cudf::column>> clamper(
  column_view const& input,
  OptionalScalarIterator lo_itr,
  ReplaceScalarIterator lo_replace_itr,
  OptionalScalarIterator hi_itr,
  ReplaceScalarIterator hi_replace_itr,
  rmm::cuda_stream_view stream,
  rmm::mr::device_memory_resource* mr)
{
  return clamp_string_column(input, lo_itr, lo_replace_itr, hi_itr, hi_replace_itr, stream, mr);
}

}  // namespace

template <typename T, typename OptionalScalarIterator, typename ReplaceScalarIterator>
std::unique_ptr<column> clamp(
  column_view const& input,
  OptionalScalarIterator lo_itr,
  ReplaceScalarIterator lo_replace_itr,
  OptionalScalarIterator hi_itr,
  ReplaceScalarIterator hi_replace_itr,
  rmm::cuda_stream_view stream,
  rmm::mr::device_memory_resource* mr = rmm::mr::get_current_device_resource())
{
  return clamper<T>(input, lo_itr, lo_replace_itr, hi_itr, hi_replace_itr, stream, mr);
}

struct dispatch_clamp {
  template <typename T>
  std::unique_ptr<column> operator()(
    column_view const& input,
    scalar const& lo,
    scalar const& lo_replace,
    scalar const& hi,
    scalar const& hi_replace,
    rmm::cuda_stream_view stream,
    rmm::mr::device_memory_resource* mr = rmm::mr::get_current_device_resource())
  {
    CUDF_EXPECTS(lo.type() == input.type(), "mismatching types of scalar and input");

    auto lo_itr         = make_optional_iterator<T>(lo, nullate::YES{});
    auto hi_itr         = make_optional_iterator<T>(hi, nullate::YES{});
    auto lo_replace_itr = make_optional_iterator<T>(lo_replace, nullate::NO{});
    auto hi_replace_itr = make_optional_iterator<T>(hi_replace, nullate::NO{});

    return clamp<T>(input, lo_itr, lo_replace_itr, hi_itr, hi_replace_itr, stream, mr);
  }
};

template <>
std::unique_ptr<column> dispatch_clamp::operator()<cudf::list_view>(
  column_view const& input,
  scalar const& lo,
  scalar const& lo_replace,
  scalar const& hi,
  scalar const& hi_replace,
  rmm::cuda_stream_view stream,
  rmm::mr::device_memory_resource* mr)
{
  CUDF_FAIL("clamp for list_view not supported");
}

template <>
std::unique_ptr<column> dispatch_clamp::operator()<struct_view>(column_view const& input,
                                                                scalar const& lo,
                                                                scalar const& lo_replace,
                                                                scalar const& hi,
                                                                scalar const& hi_replace,
                                                                rmm::cuda_stream_view stream,
                                                                rmm::mr::device_memory_resource* mr)
{
  CUDF_FAIL("clamp for struct_view not supported");
}

template <>
std::unique_ptr<column> dispatch_clamp::operator()<cudf::dictionary32>(
  column_view const& input,
  scalar const& lo,
  scalar const& lo_replace,
  scalar const& hi,
  scalar const& hi_replace,
  rmm::cuda_stream_view stream,
  rmm::mr::device_memory_resource* mr)
{
  // add lo_replace and hi_replace to keys
  auto matched_column = [&] {
    auto matched_view              = dictionary_column_view(input);
    std::unique_ptr<column> result = nullptr;
    auto add_scalar_key            = [&](scalar const& key, scalar const& key_replace) {
      if (key.is_valid(stream)) {
        result = dictionary::detail::add_keys(
          matched_view, make_column_from_scalar(key_replace, 1, stream)->view(), stream, mr);
        matched_view = dictionary_column_view(result->view());
      }
    };
    add_scalar_key(lo, lo_replace);
    add_scalar_key(hi, hi_replace);
    return result;
  }();
  auto matched_view = dictionary_column_view(matched_column->view());

  // get the indexes for lo_replace and for hi_replace
  auto lo_replace_index = dictionary::detail::get_index(matched_view, lo_replace, stream);
  auto hi_replace_index = dictionary::detail::get_index(matched_view, hi_replace, stream);

  // get the closest indexes for lo and for hi
  auto lo_index = dictionary::detail::get_insert_index(matched_view, lo, stream);
  auto hi_index = dictionary::detail::get_insert_index(matched_view, hi, stream);

  // call clamp with the scalar indexes and the matched indices
  auto matched_indices = matched_view.get_indices_annotated();
  auto new_indices     = cudf::type_dispatcher<dispatch_storage_type>(matched_indices.type(),
                                                                  dispatch_clamp{},
                                                                  matched_indices,
                                                                  *lo_index,
                                                                  *lo_replace_index,
                                                                  *hi_index,
                                                                  *hi_replace_index,
                                                                  stream,
                                                                  mr);

  auto const indices_type = new_indices->type();
  auto const output_size  = new_indices->size();
  auto const null_count   = new_indices->null_count();
  auto contents           = new_indices->release();
  auto indices_column     = std::make_unique<column>(indices_type,
                                                 static_cast<size_type>(output_size),
                                                 std::move(*(contents.data.release())),
                                                 rmm::device_buffer{},
                                                 0);

  // take the keys from the matched column allocated using mr
  std::unique_ptr<column> keys_column(std::move(matched_column->release().children.back()));

  // create column with keys_column and indices_column
  return make_dictionary_column(std::move(keys_column),
                                std::move(indices_column),
                                std::move(*(contents.null_mask.release())),
                                null_count);
}

/**
 * @copydoc cudf::clamp(column_view const& input,
                                      scalar const& lo,
                                      scalar const& lo_replace,
                                      scalar const& hi,
                                      scalar const& hi_replace,
                                      rmm::mr::device_memory_resource* mr);
 *
 * @param[in] stream CUDA stream used for device memory operations and kernel launches.
 */
std::unique_ptr<column> clamp(
  column_view const& input,
  scalar const& lo,
  scalar const& lo_replace,
  scalar const& hi,
  scalar const& hi_replace,
  rmm::cuda_stream_view stream,
  rmm::mr::device_memory_resource* mr = rmm::mr::get_current_device_resource())
{
  CUDF_EXPECTS(lo.type() == hi.type(), "mismatching types of limit scalars");
  CUDF_EXPECTS(lo_replace.type() == hi_replace.type(), "mismatching types of replace scalars");
  CUDF_EXPECTS(lo.type() == lo_replace.type(), "mismatching types of limit and replace scalars");

  if ((not lo.is_valid(stream) and not hi.is_valid(stream)) or (input.is_empty())) {
    // There will be no change
    return std::make_unique<column>(input, stream, mr);
  }

  if (lo.is_valid(stream)) {
    CUDF_EXPECTS(lo_replace.is_valid(stream), "lo_replace can't be null if lo is not null");
  }
  if (hi.is_valid(stream)) {
    CUDF_EXPECTS(hi_replace.is_valid(stream), "hi_replace can't be null if hi is not null");
  }

  return cudf::type_dispatcher<dispatch_storage_type>(
    input.type(), dispatch_clamp{}, input, lo, lo_replace, hi, hi_replace, stream, mr);
}

}  // namespace detail

// clamp input at lo and hi with lo_replace and hi_replace
std::unique_ptr<column> clamp(column_view const& input,
                              scalar const& lo,
                              scalar const& lo_replace,
                              scalar const& hi,
                              scalar const& hi_replace,
                              rmm::mr::device_memory_resource* mr)
{
  CUDF_FUNC_RANGE();
  return detail::clamp(input, lo, lo_replace, hi, hi_replace, cudf::default_stream_value, mr);
}

// clamp input at lo and hi
std::unique_ptr<column> clamp(column_view const& input,
                              scalar const& lo,
                              scalar const& hi,
                              rmm::mr::device_memory_resource* mr)
{
  CUDF_FUNC_RANGE();
  return detail::clamp(input, lo, lo, hi, hi, cudf::default_stream_value, mr);
}
}  // namespace cudf
