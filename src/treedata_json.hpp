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

// Spans refer to the original input and exclude surrounding whitespace. Duplicate
// names retain their first position and their last value, as in ordered_json.
struct TreeJsonMember {
    std::string name;
    std::size_t begin;
    std::size_t end;
};

namespace tree_json_impl {

class MemberScanner {
    std::string_view text_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail() const {
        throw nlohmann::ordered_json::parse_error::create(
            101, pos_ + 1, "invalid TreeData JSON", nullptr);
    }
    void space() { while (pos_ < text_.size() && is_space(text_[pos_])) ++pos_; }
    bool take(char c) {
        space();
        if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; }
        return false;
    }
    unsigned hex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ == text_.size()) fail();
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value += c - '0';
            else if (c >= 'a' && c <= 'f') value += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value += c - 'A' + 10;
            else fail();
        }
        return value;
    }
    void string() {
        if (pos_ == text_.size() || text_[pos_++] != '"') fail();
        while (pos_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return;
            if (c < 0x20) fail();
            if (c == '\\') {
                if (pos_ == text_.size()) fail();
                const char e = text_[pos_++];
                if (e == 'u') {
                    const unsigned u = hex4();
                    if (u >= 0xd800 && u <= 0xdbff) {
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' ||
                            text_[pos_ + 1] != 'u') fail();
                        pos_ += 2;
                        const unsigned low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail();
                    } else if (u >= 0xdc00 && u <= 0xdfff) fail();
                } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) fail();
            } else if (c >= 0x80) {
                unsigned n = 0, code = 0, min = 0;
                if (c >= 0xc2 && c <= 0xdf) { n = 1; code = c & 0x1f; min = 0x80; }
                else if (c >= 0xe0 && c <= 0xef) { n = 2; code = c & 0x0f; min = 0x800; }
                else if (c >= 0xf0 && c <= 0xf4) { n = 3; code = c & 0x07; min = 0x10000; }
                else fail();
                for (unsigned i = 0; i < n; ++i) {
                    if (pos_ == text_.size()) fail();
                    c = static_cast<unsigned char>(text_[pos_++]);
                    if ((c & 0xc0) != 0x80) fail();
                    code = (code << 6) | (c & 0x3f);
                }
                if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) fail();
            }
        }
        fail();
    }
    void scalar() {
        if (pos_ == text_.size()) fail();
        if (text_[pos_] == '"') { string(); return; }
        for (auto word : {std::string_view("true"), std::string_view("false"),
                          std::string_view("null"), std::string_view("NaN"),
                          std::string_view("Infinity"), std::string_view("-Infinity")}) {
            if (text_.substr(pos_, word.size()) == word &&
                has_end_boundary(text_, pos_ + word.size())) {
                pos_ += word.size(); return;
            }
        }
        const std::size_t begin = pos_;
        std::size_t end = pos_;
        if (!scan_number(text_, pos_, end) || !has_end_boundary(text_, end)) fail();
        auto token = text_.substr(begin, end - begin);
        if (token.find_first_of(".eE") == std::string_view::npos) check_integer_range(token);
        else (void)nlohmann::ordered_json::parse(token); // Match JSON floating-point range checks.
        pos_ = end;
    }
    void value() {
        enum State { array_first, array_after, object_first, object_after };
        std::vector<State> stack;
        bool need_value = true;
        for (;;) {
            if (!need_value && stack.empty()) return;
            space();
            if (need_value) {
                if (pos_ == text_.size()) fail();
                if (text_[pos_] == '[') { ++pos_; stack.push_back(array_first); }
                else if (text_[pos_] == '{') { ++pos_; stack.push_back(object_first); }
                else scalar();
                need_value = false;
            }
            if (stack.empty()) return;
            const State state = stack.back();
            if (state == array_first) {
                if (take(']')) { stack.pop_back(); continue; }
                stack.back() = array_after;
                need_value = true;
            } else if (state == array_after) {
                if (take(']')) { stack.pop_back(); continue; }
                if (!take(',')) fail();
                need_value = true;
            } else {
                if (state == object_first && take('}')) { stack.pop_back(); continue; }
                if (state == object_after) {
                    if (take('}')) { stack.pop_back(); continue; }
                    if (!take(',')) fail();
                }
                space(); string();
                if (!take(':')) fail();
                stack.back() = object_after;
                need_value = true;
            }
        }
    }
public:
    explicit MemberScanner(std::string_view text) : text_(text) {}
    std::vector<TreeJsonMember> scan();
};

} // namespace tree_json_impl

inline std::vector<TreeJsonMember> tree_json_impl::MemberScanner::scan() {
    std::vector<TreeJsonMember> members;
    if (text_.substr(0, 3) == "\xef\xbb\xbf") pos_ = 3;
    if (!take('{')) fail();
    if (!take('}')) {
        for (;;) {
            space();
            const std::size_t key_begin = pos_;
            string();
            std::string key = nlohmann::ordered_json::parse(
                text_.substr(key_begin, pos_ - key_begin)).get<std::string>();
            if (!take(':')) fail();
            space();
            const std::size_t begin = pos_;
            value();
            const std::size_t end = pos_;
            bool found = false;
            for (auto& member : members) {
                if (member.name == key) {
                    member.begin = begin; member.end = end; found = true; break;
                }
            }
            if (!found) members.push_back({std::move(key), begin, end});
            if (take('}')) break;
            if (!take(',')) fail();
        }
    }
    space();
    if (pos_ != text_.size()) fail();
    return members;
}

inline std::vector<TreeJsonMember> scan_tree_json_members(std::string_view text) {
    return tree_json_impl::MemberScanner(text).scan();
}

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
