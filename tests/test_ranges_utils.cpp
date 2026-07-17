#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "nexusfix/util/ranges_utils.hpp"

using namespace nfx::util;

// ============================================================================
// trim Tests
// ============================================================================

TEST_CASE("trim whitespace", "[utils][ranges][regression]") {
    REQUIRE(trim("  hello  ") == "hello");
    REQUIRE(trim("\t\nhello\r\n") == "hello");
    REQUIRE(trim("hello") == "hello");
    REQUIRE(trim("   ") == "");
    REQUIRE(trim("") == "");
    REQUIRE(trim("  a  b  ") == "a  b");
}

// ============================================================================
// split_string Tests
// ============================================================================

TEST_CASE("split_string by delimiter", "[utils][ranges][regression]") {
    SECTION("comma-separated") {
        auto parts = split_string("a,b,c", ',');
        std::vector<std::string_view> result;
        for (auto part : parts) {
            result.push_back(part);
        }
        REQUIRE(result.size() == 3);
        REQUIRE(result[0] == "a");
        REQUIRE(result[1] == "b");
        REQUIRE(result[2] == "c");
    }

    SECTION("single element") {
        auto parts = split_string("hello", ',');
        std::vector<std::string_view> result;
        for (auto part : parts) {
            result.push_back(part);
        }
        REQUIRE(result.size() == 1);
        REQUIRE(result[0] == "hello");
    }

    SECTION("empty string") {
        auto parts = split_string("", ',');
        std::vector<std::string_view> result;
        for (auto part : parts) {
            result.push_back(part);
        }
        REQUIRE(result.empty());
    }
}

// ============================================================================
// indices Tests
// ============================================================================

TEST_CASE("indices generates sequence", "[utils][ranges][regression]") {
    std::vector<size_t> result;
    for (auto i : indices(5)) {
        result.push_back(i);
    }
    REQUIRE(result.size() == 5);
    REQUIRE(result[0] == 0);
    REQUIRE(result[4] == 4);
}

TEST_CASE("indices with zero", "[utils][ranges][regression]") {
    std::vector<size_t> result;
    for (auto i : indices(0)) {
        result.push_back(i);
    }
    REQUIRE(result.empty());
}

// ============================================================================
// Range Algorithm Tests
// ============================================================================

TEST_CASE("contains", "[utils][ranges][regression]") {
    std::vector<int> v = {1, 2, 3, 4, 5};
    REQUIRE(contains(v, 3));
    REQUIRE_FALSE(contains(v, 6));
}

TEST_CASE("any_of / all_of / none_of", "[utils][ranges][regression]") {
    std::vector<int> v = {2, 4, 6, 8};
    auto is_even = [](int x) { return x % 2 == 0; };
    auto is_odd = [](int x) { return x % 2 != 0; };
    auto is_positive = [](int x) { return x > 0; };

    REQUIRE(all_of(v, is_even));
    REQUIRE_FALSE(any_of(v, is_odd));
    REQUIRE(none_of(v, is_odd));
    REQUIRE(all_of(v, is_positive));
}

TEST_CASE("count_if", "[utils][ranges][regression]") {
    std::vector<int> v = {1, 2, 3, 4, 5, 6};
    auto is_even = [](int x) { return x % 2 == 0; };
    REQUIRE(count_if(v, is_even) == 3);
}

// ============================================================================
// take_n / skip_n Tests
// ============================================================================

TEST_CASE("take_n", "[utils][ranges][regression]") {
    std::vector<int> v = {1, 2, 3, 4, 5};
    std::vector<int> result;
    for (auto x : take_n(v, 3)) {
        result.push_back(x);
    }
    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == 1);
    REQUIRE(result[2] == 3);
}

TEST_CASE("skip_n", "[utils][ranges][regression]") {
    std::vector<int> v = {1, 2, 3, 4, 5};
    std::vector<int> result;
    for (auto x : skip_n(v, 2)) {
        result.push_back(x);
    }
    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == 3);
    REQUIRE(result[2] == 5);
}

// ============================================================================
// FixFieldView Tests
// ============================================================================

TEST_CASE("FixFieldView iterates FIX fields", "[utils][ranges][fix_field][regression]") {
    std::string data = "35=D\x01""55=AAPL\x01""38=100\x01";
    auto view = fix_fields(data);

    std::vector<std::pair<int, std::string_view>> fields;
    for (auto [tag, value] : view) {
        if (tag == 0) break;
        fields.emplace_back(tag, value);
    }

    REQUIRE(fields.size() == 3);
    REQUIRE(fields[0].first == 35);
    REQUIRE(fields[0].second == "D");
    REQUIRE(fields[1].first == 55);
    REQUIRE(fields[1].second == "AAPL");
    REQUIRE(fields[2].first == 38);
    REQUIRE(fields[2].second == "100");
}

TEST_CASE("FixFieldView empty input", "[utils][ranges][fix_field][regression]") {
    auto view = fix_fields("");
    int count = 0;
    for (auto [tag, value] : view) {
        if (tag == 0) break;
        ++count;
    }
    REQUIRE(count == 0);
}

// ============================================================================
// Span Utilities Tests
// ============================================================================

TEST_CASE("as_span / as_const_span", "[utils][ranges][regression]") {
    std::vector<int> v = {1, 2, 3};
    auto s = as_span(v);
    REQUIRE(s.size() == 3);
    REQUIRE(s[0] == 1);

    const auto& cv = v;
    auto cs = as_const_span(cv);
    REQUIRE(cs.size() == 3);
}

// ============================================================================
// WS3: FixFieldView tag-parse edge branches (TICKET_497_3)
// ============================================================================
// parse_next() has several early-exit guards: empty data, missing '=',
// non-digit characters in the tag, integer overflow on the tag, and a field
// with no trailing SOH (end-of-buffer). advance() also has a "no SOH found"
// branch that empties data_.

TEST_CASE("FixFieldView no equals sign returns tag 0", "[utils][ranges][fix_field][regression]") {
    // Field without '=' - parse_next must return {0, {}}
    std::string data = "NOTAG\x01";
    std::vector<std::pair<int, std::string_view>> fields;
    for (auto [tag, value] : fix_fields(data)) {
        if (tag == 0) break;
        fields.emplace_back(tag, value);
    }
    REQUIRE(fields.empty());
}

TEST_CASE("FixFieldView non-digit in tag returns 0", "[utils][ranges][fix_field][regression]") {
    std::string data = "3X=D\x01";
    std::vector<std::pair<int, std::string_view>> fields;
    for (auto [tag, value] : fix_fields(data)) {
        if (tag == 0) break;
        fields.emplace_back(tag, value);
    }
    REQUIRE(fields.empty());
}

TEST_CASE("FixFieldView tag integer overflow clamps to 0", "[utils][ranges][fix_field][regression]") {
    // Build a tag number larger than INT_MAX to trigger overflow guard
    std::string data = "99999999999999999999=V\x01";
    std::vector<std::pair<int, std::string_view>> fields;
    for (auto [tag, value] : fix_fields(data)) {
        if (tag == 0) break;
        fields.emplace_back(tag, value);
    }
    REQUIRE(fields.empty());
}

TEST_CASE("FixFieldView field with no trailing SOH uses data end", "[utils][ranges][fix_field][regression]") {
    // No SOH at end - soh_pos falls back to data_.size()
    std::string data = "35=D";
    std::vector<std::pair<int, std::string_view>> fields;
    for (auto [tag, value] : fix_fields(data)) {
        if (tag == 0) break;
        fields.emplace_back(tag, value);
    }
    REQUIRE(fields.size() == 1);
    REQUIRE(fields[0].first == 35);
    REQUIRE(fields[0].second == "D");
}

TEST_CASE("FixFieldView advance with no SOH empties data", "[utils][ranges][fix_field][regression]") {
    // First field has SOH so it is parsed; second field has no SOH but that
    // means advance() empties data_ after the first ++ and the iterator ends.
    std::string data = "8=FIX.4.4\x01""35=D";
    std::vector<std::pair<int, std::string_view>> fields;
    for (auto [tag, value] : fix_fields(data)) {
        if (tag == 0) break;
        fields.emplace_back(tag, value);
    }
    REQUIRE(fields.size() >= 1);
    REQUIRE(fields[0].first == 8);
}

TEST_CASE("FixFieldView zero-digit tag (empty tag) returns 0", "[utils][ranges][fix_field][regression]") {
    // '=' at position 0 - no digits before it, tag stays 0
    std::string data = "=VALUE\x01";
    std::vector<std::pair<int, std::string_view>> fields;
    for (auto [tag, value] : fix_fields(data)) {
        if (tag == 0) break;
        fields.emplace_back(tag, value);
    }
    REQUIRE(fields.empty());
}

// ============================================================================
// WS3: trim all-whitespace branches (TICKET_497_3)
// ============================================================================

TEST_CASE("trim all whitespace types", "[utils][ranges][regression]") {
    REQUIRE(trim("\t") == "");
    REQUIRE(trim("\r\n") == "");
    REQUIRE(trim("  \t  ") == "");
    REQUIRE(trim("a") == "a");
    REQUIRE(trim(" a") == "a");
    REQUIRE(trim("a ") == "a");
}

// ============================================================================
// WS3: enumerate, to_vector, format utilities (TICKET_497_3)
// ============================================================================

TEST_CASE("enumerate produces index-value pairs", "[utils][ranges][regression]") {
    std::vector<int> v = {10, 20, 30};
    int sum_idx = 0;
    for (auto [i, val] : enumerate(v)) {
        sum_idx += static_cast<int>(i);
        (void)val;
    }
    REQUIRE(sum_idx == 3);  // 0+1+2
}

TEST_CASE("to_vector converts range", "[utils][ranges][regression]") {
    std::vector<int> src = {1, 2, 3, 4};
    auto evens = src | std::views::filter([](int x){ return x % 2 == 0; });
    auto result = to_vector(evens);
    REQUIRE(result.size() == 2);
    REQUIRE(result[0] == 2);
    REQUIRE(result[1] == 4);
}

TEST_CASE("find_if returns correct iterator", "[utils][ranges][regression]") {
    std::vector<int> v = {1, 3, 5, 6, 7};
    auto it = find_if(v, [](int x){ return x % 2 == 0; });
    REQUIRE(it != v.end());
    REQUIRE(*it == 6);
}

TEST_CASE("find_if returns end when not found", "[utils][ranges][regression]") {
    std::vector<int> v = {1, 3, 5};
    auto it = find_if(v, [](int x){ return x % 2 == 0; });
    REQUIRE(it == v.end());
}
