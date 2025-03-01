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

#pragma once

#include <cudf/utilities/default_stream.hpp>
#include <cudf/utilities/error.hpp>
#include <cudf/utilities/span.hpp>

#include <rmm/cuda_stream_view.hpp>
#include <rmm/device_buffer.hpp>

#include <thrust/host_vector.h>
#include <thrust/system/cuda/experimental/pinned_allocator.h>

/**
 * @brief A helper class that wraps fixed-length device memory for the GPU, and
 * a mirror host pinned memory for the CPU.
 *
 * This abstraction allocates a specified fixed chunk of device memory that can
 * initialized upfront, or gradually initialized as required.
 * The host-side memory can be used to manipulate data on the CPU before and
 * after operating on the same data on the GPU.
 */
template <typename T>
class hostdevice_vector {
 public:
  using value_type = T;

  hostdevice_vector() : hostdevice_vector(0, cudf::default_stream_value) {}

  explicit hostdevice_vector(size_t size, rmm::cuda_stream_view stream)
    : hostdevice_vector(size, size, stream)
  {
  }

  explicit hostdevice_vector(size_t initial_size, size_t max_size, rmm::cuda_stream_view stream)
    : d_data(0, stream)
  {
    CUDF_EXPECTS(initial_size <= max_size, "initial_size cannot be larger than max_size");
    h_data.reserve(max_size);
    h_data.resize(initial_size);
    d_data.resize(max_size, stream);
  }

  void push_back(const T& data)
  {
    CUDF_EXPECTS(size() < capacity(),
                 "Cannot insert data into hostdevice_vector because capacity has been exceeded.");
    h_data.push_back(data);
  }

  [[nodiscard]] size_t capacity() const noexcept { return h_data.capacity(); }
  [[nodiscard]] size_t size() const noexcept { return h_data.size(); }
  [[nodiscard]] size_t memory_size() const noexcept { return sizeof(T) * size(); }

  [[nodiscard]] T& operator[](size_t i) { return h_data[i]; }
  [[nodiscard]] T const& operator[](size_t i) const { return h_data[i]; }

  [[nodiscard]] T* host_ptr(size_t offset = 0) { return h_data.data() + offset; }
  [[nodiscard]] T const* host_ptr(size_t offset = 0) const { return h_data.data() + offset; }

  [[nodiscard]] T* begin() { return host_ptr(); }
  [[nodiscard]] T const* begin() const { return host_ptr(); }

  [[nodiscard]] T* end() { return host_ptr(size()); }
  [[nodiscard]] T const* end() const { return host_ptr(size()); }

  [[nodiscard]] T* device_ptr(size_t offset = 0) { return d_data.data() + offset; }
  [[nodiscard]] T const* device_ptr(size_t offset = 0) const { return d_data.data() + offset; }

  [[nodiscard]] T* d_begin() { return device_ptr(); }
  [[nodiscard]] T const* d_begin() const { return device_ptr(); }

  [[nodiscard]] T* d_end() { return device_ptr(size()); }
  [[nodiscard]] T const* d_end() const { return device_ptr(size()); }

  /**
   * @brief Returns the specified element from device memory
   *
   * @note This function incurs a device to host memcpy and should be used sparingly.
   * @note This function synchronizes `stream`.
   *
   * @throws rmm::out_of_range exception if `element_index >= size()`
   *
   * @param element_index Index of the desired element
   * @param stream The stream on which to perform the copy
   * @return The value of the specified element
   */
  [[nodiscard]] T element(std::size_t element_index, rmm::cuda_stream_view stream) const
  {
    return d_data.element(element_index, stream);
  }

  operator cudf::host_span<T>() { return {host_ptr(), size()}; }
  operator cudf::host_span<T const>() const { return {host_ptr(), size()}; }

  operator cudf::device_span<T>() { return {device_ptr(), size()}; }
  operator cudf::device_span<T const>() const { return {device_ptr(), size()}; }

  void host_to_device(rmm::cuda_stream_view stream, bool synchronize = false)
  {
    CUDF_CUDA_TRY(cudaMemcpyAsync(
      device_ptr(), host_ptr(), memory_size(), cudaMemcpyHostToDevice, stream.value()));
    if (synchronize) { stream.synchronize(); }
  }

  void device_to_host(rmm::cuda_stream_view stream, bool synchronize = false)
  {
    CUDF_CUDA_TRY(cudaMemcpyAsync(
      host_ptr(), device_ptr(), memory_size(), cudaMemcpyDeviceToHost, stream.value()));
    if (synchronize) { stream.synchronize(); }
  }

 private:
  thrust::host_vector<T, thrust::system::cuda::experimental::pinned_allocator<T>> h_data;
  rmm::device_uvector<T> d_data;
};

namespace cudf {
namespace detail {

/**
 * @brief Wrapper around hostdevice_vector to enable two-dimensional indexing.
 *
 * Does not incur additional allocations.
 */
template <typename T>
class hostdevice_2dvector {
 public:
  hostdevice_2dvector(size_t rows, size_t columns, rmm::cuda_stream_view stream)
    : _size{rows, columns}, _data{rows * columns, stream}
  {
  }

  operator device_2dspan<T>() { return {_data.device_ptr(), _size}; }
  operator device_2dspan<T const>() const { return {_data.device_ptr(), _size}; }

  device_2dspan<T> device_view() { return static_cast<device_2dspan<T>>(*this); }
  device_2dspan<T> device_view() const { return static_cast<device_2dspan<T const>>(*this); }

  operator host_2dspan<T>() { return {_data.host_ptr(), _size}; }
  operator host_2dspan<T const>() const { return {_data.host_ptr(), _size}; }

  host_2dspan<T> host_view() { return static_cast<host_2dspan<T>>(*this); }
  host_2dspan<T> host_view() const { return static_cast<host_2dspan<T const>>(*this); }

  host_span<T> operator[](size_t row)
  {
    return {_data.host_ptr() + host_2dspan<T>::flatten_index(row, 0, _size), _size.second};
  }

  host_span<T const> operator[](size_t row) const
  {
    return {_data.host_ptr() + host_2dspan<T>::flatten_index(row, 0, _size), _size.second};
  }

  auto size() const noexcept { return _size; }
  auto count() const noexcept { return _size.first * _size.second; }
  auto is_empty() const noexcept { return count() == 0; }

  T* base_host_ptr(size_t offset = 0) { return _data.host_ptr(offset); }
  T* base_device_ptr(size_t offset = 0) { return _data.device_ptr(offset); }

  T const* base_host_ptr(size_t offset = 0) const { return _data.host_ptr(offset); }

  T const* base_device_ptr(size_t offset = 0) const { return _data.device_ptr(offset); }

  size_t memory_size() const noexcept { return _data.memory_size(); }

  void host_to_device(rmm::cuda_stream_view stream, bool synchronize = false)
  {
    _data.host_to_device(stream, synchronize);
  }

  void device_to_host(rmm::cuda_stream_view stream, bool synchronize = false)
  {
    _data.device_to_host(stream, synchronize);
  }

 private:
  hostdevice_vector<T> _data;
  typename host_2dspan<T>::size_type _size;
};

}  // namespace detail
}  // namespace cudf
