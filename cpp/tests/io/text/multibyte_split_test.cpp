/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION.
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

#include <cudf_test/base_fixture.hpp>
#include <cudf_test/column_utilities.hpp>
#include <cudf_test/column_wrapper.hpp>
#include <cudf_test/cudf_gtest.hpp>
#include <cudf_test/table_utilities.hpp>
#include <cudf_test/type_lists.hpp>

#include <cudf/concatenate.hpp>
#include <cudf/io/text/byte_range_info.hpp>
#include <cudf/io/text/data_chunk_source_factories.hpp>
#include <cudf/io/text/multibyte_split.hpp>
#include <cudf/strings/strings_column_view.hpp>

using namespace cudf;
using namespace test;

// 😀 | F0 9F 98 80 | 11110000 10011111 10011000 10000000
// 😎 | F0 9F 98 8E | 11110000 10011111 10011000 10001110

struct MultibyteSplitTest : public BaseFixture {
};

TEST_F(MultibyteSplitTest, Simple)
{
  auto delimiter  = std::string(":");
  auto host_input = std::string("abc:def");

  auto expected = strings_column_wrapper{"abc:", "def"};

  auto source = cudf::io::text::make_source(host_input);
  auto out    = cudf::io::text::multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out);
}

TEST_F(MultibyteSplitTest, NondeterministicMatching)
{
  auto delimiter  = std::string("abac");
  auto host_input = std::string("ababacabacab");

  auto expected = strings_column_wrapper{"ababac", "abac", "ab"};

  auto source = cudf::io::text::make_source(host_input);
  auto out    = cudf::io::text::multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out);
}

TEST_F(MultibyteSplitTest, DelimiterAtEnd)
{
  auto delimiter  = std::string(":");
  auto host_input = std::string("abcdefg:");

  auto expected = strings_column_wrapper{"abcdefg:", ""};

  auto source = cudf::io::text::make_source(host_input);
  auto out    = cudf::io::text::multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out);
}

TEST_F(MultibyteSplitTest, DelimiterAtEndByteRange)
{
  auto delimiter  = std::string(":");
  auto host_input = std::string("abcdefg:");

  auto expected = strings_column_wrapper{"abcdefg:", ""};

  auto source = cudf::io::text::make_source(host_input);
  auto out    = cudf::io::text::multibyte_split(
    *source,
    delimiter,
    cudf::io::text::byte_range_info{0, static_cast<int64_t>(host_input.size())});

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out);
}

TEST_F(MultibyteSplitTest, LargeInputSparse)
{
  auto host_input    = std::string(1024 * 1024 * 32, '.');
  auto host_expected = std::vector<std::string>();

  host_input[host_input.size() / 2] = '|';

  host_expected.emplace_back(host_input.substr(0, host_input.size() / 2 + 1));
  host_expected.emplace_back(host_input.substr(host_input.size() / 2 + 1));

  auto expected = strings_column_wrapper{host_expected.begin(), host_expected.end()};

  auto delimiter = std::string("|");
  auto source    = cudf::io::text::make_source(host_input);
  auto out       = cudf::io::text::multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out);
}

TEST_F(MultibyteSplitTest, LargeInput)
{
  auto host_input    = std::string();
  auto host_expected = std::vector<std::string>();

  for (auto i = 0; i < (2 * 32 * 128 * 1024); i++) {
    host_input += "...:|";
    host_expected.emplace_back(std::string("...:|"));
  }

  host_expected.emplace_back(std::string(""));

  auto expected = strings_column_wrapper{host_expected.begin(), host_expected.end()};

  auto delimiter = std::string("...:|");
  auto source    = cudf::io::text::make_source(host_input);
  auto out       = cudf::io::text::multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out);
}

TEST_F(MultibyteSplitTest, OverlappingMatchErasure)
{
  auto delimiter = "::";

  auto host_input = std::string(
    ":::::"
    ":::::");
  auto expected = strings_column_wrapper{":::::", ":::::"};

  auto source = cudf::io::text::make_source(host_input);
  auto out    = cudf::io::text::multibyte_split(*source, delimiter);

  // CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out); // this use case it not yet supported.
}

TEST_F(MultibyteSplitTest, HandpickedInput)
{
  auto delimiters = "::|";
  auto host_input = std::string(
    "aaa::|"
    "bbb::|"
    "ccc::|"
    "ddd::|"
    "eee::|"
    "fff::|"
    "ggg::|"
    "hhh::|"
    "___::|"
    "here::|"
    "is::|"
    "another::|"
    "simple::|"
    "text::|"
    "seperated::|"
    "by::|"
    "emojis::|"
    "which::|"
    "are::|"
    "multiple::|"
    "bytes::|"
    "and::|"
    "used::|"
    "as::|"
    "delimiters.::|"
    "::|"
    "::|"
    "::|");

  auto expected = strings_column_wrapper{
    "aaa::|",         "bbb::|",      "ccc::|",       "ddd::|",  "eee::|",    "fff::|",
    "ggg::|",         "hhh::|",      "___::|",       "here::|", "is::|",     "another::|",
    "simple::|",      "text::|",     "seperated::|", "by::|",   "emojis::|", "which::|",
    "are::|",         "multiple::|", "bytes::|",     "and::|",  "used::|",   "as::|",
    "delimiters.::|", "::|",         "::|",          "::|",     ""};

  auto source = cudf::io::text::make_source(host_input);
  auto out    = cudf::io::text::multibyte_split(*source, delimiters);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out, debug_output_level::ALL_ERRORS);
}

TEST_F(MultibyteSplitTest, LargeInputMultipleRange)
{
  auto host_input    = std::string();
  auto host_expected = std::vector<std::string>();

  for (auto i = 0; i < 1000; i++) {
    host_input += "...:|";
  }

  auto delimiter = std::string("...:|");
  auto source    = cudf::io::text::make_source(host_input);

  auto byte_ranges = cudf::io::text::create_byte_range_infos_consecutive(host_input.size(), 3);
  auto out0        = cudf::io::text::multibyte_split(*source, delimiter, byte_ranges[0]);
  auto out1        = cudf::io::text::multibyte_split(*source, delimiter, byte_ranges[1]);
  auto out2        = cudf::io::text::multibyte_split(*source, delimiter, byte_ranges[2]);

  auto out_views = std::vector<cudf::column_view>({out0->view(), out1->view(), out2->view()});
  auto out       = cudf::concatenate(out_views);

  auto expected = cudf::io::text::multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected->view(), *out, debug_output_level::ALL_ERRORS);
}

TEST_F(MultibyteSplitTest, LargeInputSparseMultipleRange)
{
  auto host_input    = std::string();
  auto host_expected = std::vector<std::string>();

  for (auto i = 0; i < 1000; i++) {
    host_input += ".....";
  }

  auto delimiter                        = std::string("...:|");
  host_input[host_input.size() / 2]     = ':';
  host_input[host_input.size() / 2 + 1] = '|';
  auto source                           = cudf::io::text::make_source(host_input);

  auto byte_ranges = cudf::io::text::create_byte_range_infos_consecutive(host_input.size(), 3);
  auto out0        = cudf::io::text::multibyte_split(*source, delimiter, byte_ranges[0]);
  auto out1        = cudf::io::text::multibyte_split(*source, delimiter, byte_ranges[1]);
  auto out2        = cudf::io::text::multibyte_split(*source, delimiter, byte_ranges[2]);

  auto out_views = std::vector<cudf::column_view>({out0->view(), out1->view(), out2->view()});
  auto out       = cudf::concatenate(out_views);

  auto expected = cudf::io::text::multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected->view(), *out, debug_output_level::ALL_ERRORS);
}

TEST_F(MultibyteSplitTest, SmallInputAllPossibleRanges)
{
  using namespace cudf::io::text;

  auto host_input = std::string();
  for (auto i = 0; i < 5; i++) {
    host_input += "::";
  }

  auto delimiter = std::string("::");
  auto source    = make_source(host_input);

  // for all possible ways to split the input, check that each field is only output once
  int size = static_cast<int>(host_input.size());
  for (int split1 = 1; split1 < size; split1++) {
    SCOPED_TRACE(split1);
    for (int split2 = split1 + 1; split2 < size; split2++) {
      SCOPED_TRACE(split2);
      auto out1 = multibyte_split(*source, delimiter, byte_range_info{0, split1});
      auto out2 = multibyte_split(*source, delimiter, byte_range_info{split1, split2 - split1});
      auto out3 = multibyte_split(*source, delimiter, byte_range_info{split2, size - split2});

      auto out_views = std::vector<cudf::column_view>({out1->view(), out2->view(), out3->view()});
      auto out       = cudf::concatenate(out_views);

      auto expected = multibyte_split(*source, delimiter);

      CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected->view(), *out, debug_output_level::ALL_ERRORS);
    }
  }
}

TEST_F(MultibyteSplitTest, SmallInputAllPossibleRangesSingleByte)
{
  using namespace cudf::io::text;

  auto host_input = std::string();
  for (auto i = 0; i < 5; i++) {
    host_input += std::to_string(i) + ":";
  }

  auto delimiter = std::string(":");
  auto source    = make_source(host_input);

  // for all possible ways to split the input, check that each field is only output once
  int size = static_cast<int>(host_input.size());
  for (int split1 = 1; split1 < size; split1++) {
    SCOPED_TRACE(split1);
    for (int split2 = split1 + 1; split2 < size; split2++) {
      SCOPED_TRACE(split2);
      auto out1 = multibyte_split(*source, delimiter, byte_range_info{0, split1});
      auto out2 = multibyte_split(*source, delimiter, byte_range_info{split1, split2 - split1});
      auto out3 = multibyte_split(*source, delimiter, byte_range_info{split2, size - split2});

      auto out_views = std::vector<cudf::column_view>({out1->view(), out2->view(), out3->view()});
      auto out       = cudf::concatenate(out_views);

      auto expected = multibyte_split(*source, delimiter);

      CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected->view(), *out, debug_output_level::ALL_ERRORS);
    }
  }
}

TEST_F(MultibyteSplitTest, EmptyInput)
{
  using namespace cudf::io::text;
  auto host_input = std::string();
  auto delimiter  = std::string("::");
  auto source     = make_source(host_input);
  auto expected   = strings_column_wrapper{};

  auto out = multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out, debug_output_level::ALL_ERRORS);
}

TEST_F(MultibyteSplitTest, EmptyInputSingleByte)
{
  using namespace cudf::io::text;
  auto host_input = std::string();
  auto delimiter  = std::string(":");
  auto source     = make_source(host_input);
  auto expected   = strings_column_wrapper{};

  auto out = multibyte_split(*source, delimiter);

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out, debug_output_level::ALL_ERRORS);
}

TEST_F(MultibyteSplitTest, EmptyRange)
{
  using namespace cudf::io::text;
  auto host_input = std::string("ab::cd");
  auto delimiter  = std::string("::");
  auto source     = make_source(host_input);
  auto expected   = strings_column_wrapper{};

  auto out = multibyte_split(*source, delimiter, byte_range_info{4, 0});

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out, debug_output_level::ALL_ERRORS);
}

TEST_F(MultibyteSplitTest, EmptyRangeSingleByte)
{
  using namespace cudf::io::text;
  auto host_input = std::string("ab:cd");
  auto delimiter  = std::string(":");
  auto source     = make_source(host_input);
  auto expected   = strings_column_wrapper{};

  auto out = multibyte_split(*source, delimiter, byte_range_info{3, 0});

  CUDF_TEST_EXPECT_COLUMNS_EQUAL(expected, *out, debug_output_level::ALL_ERRORS);
}

CUDF_TEST_PROGRAM_MAIN()
