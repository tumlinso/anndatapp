#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace anndatapp::detail {

namespace tree_json_impl {

enum class NumberKind : std::uint8_t {
    finite,
    nan,
    positive_infinity,
    negative_infinity
};

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr bool is_value_start_boundary(char c) noexcept {
    return is_space(c) || c == ',' || c == ':' || c == '[' || c == '{';
}

constexpr bool is_value_end_boundary(char c) noexcept {
    return is_space(c) || c == ',' || c == ']' || c == '}';
}

inline bool has_start_boundary(std::string_view text, std::size_t pos) noexcept {
    return pos == 0 || is_value_start_boundary(text[pos - 1]);
}

inline bool has_end_boundary(std::string_view text, std::size_t pos) noexcept {
    return pos == text.size() || is_value_end_boundary(text[pos]);
}

inline bool scan_number(std::string_view text, std::size_t begin,
                 std::size_t& end) noexcept {
    std::size_t pos = begin;
    if (pos < text.size() && text[pos] == '-') ++pos;
    if (pos == text.size()) return false;

    if (text[pos] == '0') {
        ++pos;
    } else if (text[pos] >= '1' && text[pos] <= '9') {
        do { ++pos; } while (pos < text.size() && is_digit(text[pos]));
    } else {
        return false;
    }

    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        if (pos == text.size() || !is_digit(text[pos])) return false;
        do { ++pos; } while (pos < text.size() && is_digit(text[pos]));
    }

    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
        if (pos == text.size() || !is_digit(text[pos])) return false;
        do { ++pos; } while (pos < text.size() && is_digit(text[pos]));
    }

    end = pos;
    return true;
}

inline void check_integer_range(std::string_view token) {
    const char* first = token.data();
    const char* last = first + token.size();
    if (token.front() == '-') {
        std::int64_t value = 0;
        const auto result = std::from_chars(first, last, value);
        if (result.ec == std::errc::result_out_of_range)
            throw std::out_of_range("TreeData JSON integer is below int64_t range");
        if (result.ec != std::errc{} || result.ptr != last)
            throw std::logic_error("TreeData JSON scanner produced an invalid integer token");
    } else {
        std::uint64_t value = 0;
        const auto result = std::from_chars(first, last, value);
        if (result.ec == std::errc::result_out_of_range)
            throw std::out_of_range("TreeData JSON integer exceeds uint64_t range");
        if (result.ec != std::errc{} || result.ptr != last)
            throw std::logic_error("TreeData JSON scanner produced an invalid integer token");
    }
}

} // namespace tree_json_impl

inline nlohmann::ordered_json parse_tree_json(std::string_view text) {
    using tree_json_impl::NumberKind;
    using Json = nlohmann::ordered_json;

    std::string normalized;
    normalized.reserve(text.size());
    std::vector<NumberKind> numbers;
    bool in_string = false;
    bool escaped = false;

    for (std::size_t pos = 0; pos < text.size();) {
        const char c = text[pos];
        if (in_string) {
            normalized.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            ++pos;
            continue;
        }

        if (c == '"') {
            in_string = true;
            normalized.push_back(c);
            ++pos;
            continue;
        }

        if (tree_json_impl::has_start_boundary(text, pos)) {
            struct SpecialToken { std::string_view spelling; NumberKind kind; };
            constexpr SpecialToken special_tokens[] = {
                {"-Infinity", NumberKind::negative_infinity},
                {"Infinity", NumberKind::positive_infinity},
                {"NaN", NumberKind::nan},
            };
            bool replaced = false;
            for (const auto& token : special_tokens) {
                if (text.substr(pos, token.spelling.size()) == token.spelling &&
                    tree_json_impl::has_end_boundary(text, pos + token.spelling.size())) {
                    normalized.push_back('0');
                    numbers.push_back(token.kind);
                    pos += token.spelling.size();
                    replaced = true;
                    break;
                }
            }
            if (replaced) continue;

            if (c == '-' || tree_json_impl::is_digit(c)) {
                std::size_t end = pos;
                if (tree_json_impl::scan_number(text, pos, end) &&
                    tree_json_impl::has_end_boundary(text, end)) {
                    const auto token = text.substr(pos, end - pos);
                    if (token.find_first_of(".eE") == std::string_view::npos)
                        tree_json_impl::check_integer_range(token);
                    normalized.append(token);
                    numbers.push_back(NumberKind::finite);
                    pos = end;
                    continue;
                }
            }
        }

        normalized.push_back(c);
        ++pos;
    }

    Json result = Json::parse(normalized);
    std::size_t number_index = 0;
    auto restore_specials = [&](auto&& self, Json& value) -> void {
        if (value.is_number()) {
            if (number_index < numbers.size()) {
                switch (numbers[number_index]) {
                case NumberKind::nan:
                    value = std::numeric_limits<double>::quiet_NaN();
                    break;
                case NumberKind::positive_infinity:
                    value = std::numeric_limits<double>::infinity();
                    break;
                case NumberKind::negative_infinity:
                    value = -std::numeric_limits<double>::infinity();
                    break;
                case NumberKind::finite:
                    break;
                }
            }
            ++number_index;
        } else if (value.is_array()) {
            for (auto& child : value) self(self, child);
        } else if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it)
                self(self, it.value());
        }
    };
    restore_specials(restore_specials, result);
    if (number_index != numbers.size()) {
        throw Json::parse_error::create(
            101, 0,
            "could not restore non-finite numbers without losing JSON numeric values",
            nullptr);
    }
    return result;
}

inline std::string dump_tree_json(const nlohmann::ordered_json& value) {
    using Json = nlohmann::ordered_json;
    std::string output;
    auto append = [&](auto&& self, const Json& current) -> void {
        if (current.is_object()) {
            output.push_back('{');
            bool first = true;
            for (auto it = current.begin(); it != current.end(); ++it) {
                if (!first) output.push_back(',');
                first = false;
                output += Json(it.key()).dump();
                output.push_back(':');
                self(self, it.value());
            }
            output.push_back('}');
        } else if (current.is_array()) {
            output.push_back('[');
            bool first = true;
            for (const auto& child : current) {
                if (!first) output.push_back(',');
                first = false;
                self(self, child);
            }
            output.push_back(']');
        } else if (current.is_number_float()) {
            const double number = current.get<double>();
            if (std::isnan(number)) output += "NaN";
            else if (std::isinf(number))
                output += std::signbit(number) ? "-Infinity" : "Infinity";
            else output += current.dump();
        } else {
            output += current.dump();
        }
    };
    append(append, value);
    return output;
}

} // namespace anndatapp::detail
