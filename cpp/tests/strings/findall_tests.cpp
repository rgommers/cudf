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

#include <cudf/strings/findall.hpp>
#include <cudf/strings/strings_column_view.hpp>
#include <cudf_test/base_fixture.hpp>
#include <cudf_test/column_utilities.hpp>
#include <cudf_test/column_wrapper.hpp>
#include <cudf_test/table_utilities.hpp>
#include <tests/strings/utilities.h>

#include <thrust/iterator/transform_iterator.h>

#include <vector>

struct StringsFindallTests : public cudf::test::BaseFixture {
};

TEST_F(StringsFindallTests, FindallTest)
{
  bool valids[] = {1, 1, 1, 1, 1, 0, 1, 1};
  cudf::test::strings_column_wrapper input(
    {"3-A", "4-May 5-Day 6-Hay", "12-Dec-2021-Jan", "Feb-March", "4 ABC", "", "", "25-9000-Hal"},
    valids);

  auto results = cudf::strings::findall(cudf::strings_column_view(input), "(\\d+)-(\\w+)");

  using LCW = cudf::test::lists_column_wrapper<cudf::string_view>;
  LCW expected({LCW{"3-A"},
                LCW{"4-May", "5-Day", "6-Hay"},
                LCW{"12-Dec", "2021-Jan"},
                LCW{},
                LCW{},
                LCW{},
                LCW{},
                LCW{"25-9000"}},
               valids);
  CUDF_TEST_EXPECT_COLUMNS_EQUIVALENT(results->view(), expected);
}

TEST_F(StringsFindallTests, Multiline)
{
  cudf::test::strings_column_wrapper input({"abc\nfff\nabc", "fff\nabc\nlll", "abc", "", "abc\n"});
  auto view = cudf::strings_column_view(input);

  auto results = cudf::strings::findall(view, "(^abc$)", cudf::strings::regex_flags::MULTILINE);
  using LCW    = cudf::test::lists_column_wrapper<cudf::string_view>;
  LCW expected({LCW{"abc", "abc"}, LCW{"abc"}, LCW{"abc"}, LCW{}, LCW{"abc"}});
  CUDF_TEST_EXPECT_COLUMNS_EQUIVALENT(results->view(), expected);
}

TEST_F(StringsFindallTests, DotAll)
{
  cudf::test::strings_column_wrapper input({"abc\nfa\nef", "fff\nabbc\nfff", "abcdef", ""});
  auto view = cudf::strings_column_view(input);

  auto results = cudf::strings::findall(view, "(b.*f)", cudf::strings::regex_flags::DOTALL);
  using LCW    = cudf::test::lists_column_wrapper<cudf::string_view>;
  LCW expected({LCW{"bc\nfa\nef"}, LCW{"bbc\nfff"}, LCW{"bcdef"}, LCW{}});
  CUDF_TEST_EXPECT_COLUMNS_EQUIVALENT(results->view(), expected);
}

TEST_F(StringsFindallTests, MediumRegex)
{
  // This results in 15 regex instructions and falls in the 'medium' range.
  std::string medium_regex = "(\\w+) (\\w+) (\\d+)";

  cudf::test::strings_column_wrapper input({"first words 1234 and just numbers 9876", "neither"});
  auto strings_view = cudf::strings_column_view(input);
  auto results      = cudf::strings::findall(strings_view, medium_regex);

  using LCW = cudf::test::lists_column_wrapper<cudf::string_view>;
  LCW expected({LCW{"first words 1234", "just numbers 9876"}, LCW{}});
  CUDF_TEST_EXPECT_COLUMNS_EQUIVALENT(results->view(), expected);
}

TEST_F(StringsFindallTests, LargeRegex)
{
  // This results in 115 regex instructions and falls in the 'large' range.
  std::string large_regex =
    "hello @abc @def world The quick brown @fox jumps over the lazy @dog hello "
    "http://www.world.com I'm here @home zzzz";

  cudf::test::strings_column_wrapper input(
    {"hello @abc @def world The quick brown @fox jumps over the lazy @dog hello "
     "http://www.world.com I'm here @home zzzz",
     "12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012"
     "34"
     "5678901234567890",
     "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmn"
     "op"
     "qrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"});

  auto strings_view = cudf::strings_column_view(input);
  auto results      = cudf::strings::findall(strings_view, large_regex);

  using LCW = cudf::test::lists_column_wrapper<cudf::string_view>;
  LCW expected({LCW{large_regex.c_str()}, LCW{}, LCW{}});
  CUDF_TEST_EXPECT_COLUMNS_EQUIVALENT(results->view(), expected);
}
