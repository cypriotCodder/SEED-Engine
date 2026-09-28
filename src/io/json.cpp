#include "io/json.hpp"
#include <charconv>
#include <cmath>

namespace seed {
namespace {
constexpr std::int64_t exact_limit = std::int64_t(1) << 53;

[[noreturn]] void wrong_type(const char* expected) {
    throw std::runtime_error(std::string("Expected a JSON ") + expected);
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Json document() {
        skip_space();
        auto value = parse(0);
        skip_space();
        if (at_ < text_.size()) fail("Unexpected text after the JSON value");
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < at_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else
                ++column;
        }
        throw JsonError(message, line, column);
    }
    void skip_space() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r'))
            ++at_;
    }
    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }

    Json parse(int depth) {
        if (depth >= 128) fail("JSON nested too deeply");
        switch (peek()) {
        case '{':
            return object(depth);
        case '[':
            return array(depth);
        case '"':
            return Json(string());
        case 't':
            if (literal("true")) return Json(true);
            break;
        case 'f':
            if (literal("false")) return Json(false);
            break;
        case 'n':
            if (literal("null")) return Json();
            break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return number();
        }
        if (at_ >= text_.size()) fail("Unexpected end of JSON");
        fail("Unexpected character");
    }

    Json object(int depth) {
        ++at_;
        auto result = Json::object();
        skip_space();
        if (peek() == '}') {
            ++at_;
            return result;
        }
        while (true) {
            skip_space();
            if (peek() != '"') fail("Expected a quoted member name");
            const auto start = at_;
            auto key = string();
            if (result.find(key)) {
                at_ = start;
                fail("Duplicate member \"" + key + "\"");
            }
            skip_space();
            if (peek() != ':') fail("Expected ':' after a member name");
            ++at_;
            skip_space();
            result.set(key, parse(depth + 1));
            skip_space();
            if (peek() == ',') {
                ++at_;
                continue;
            }
            if (peek() == '}') {
                ++at_;
                return result;
            }
            fail("Expected ',' or '}' in an object");
        }
    }

    Json array(int depth) {
        ++at_;
        auto result = Json::array();
        skip_space();
        if (peek() == ']') {
            ++at_;
            return result;
        }
        while (true) {
            skip_space();
            result.push(parse(depth + 1));
            skip_space();
            if (peek() == ',') {
                ++at_;
                continue;
            }
            if (peek() == ']') {
                ++at_;
                return result;
            }
            fail("Expected ',' or ']' in an array");
        }
    }

    unsigned hex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = peek();
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail("Invalid \\u escape");
            ++at_;
        }
        return value;
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80)
            out += static_cast<char>(code);
        else if (code < 0x800) {
            out += static_cast<char>(0xc0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xe0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        }
    }

    std::string string() {
        ++at_; // Opening quote.
        std::string out;
        while (true) {
            if (at_ >= text_.size()) fail("Unterminated string");
            const char c = text_[at_];
            if (c == '"') {
                ++at_;
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) fail("Control character in a string");
            if (c != '\\') {
                out += c;
                ++at_;
                continue;
            }
            ++at_;
            const char escape = peek();
            ++at_;
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out += escape;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                unsigned code = hex4();
                if (code >= 0xd800 && code < 0xdc00) {
                    if (!literal("\\u")) fail("Unpaired UTF-16 surrogate");
                    const unsigned low = hex4();
                    if (low < 0xdc00 || low >= 0xe000) fail("Unpaired UTF-16 surrogate");
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code < 0xe000)
                    fail("Unpaired UTF-16 surrogate");
                append_utf8(out, code);
                break;
            }
            default:
                --at_;
                fail("Invalid escape in a string");
            }
        }
    }

    Json number() {
        const auto start = at_;
        if (peek() == '-') ++at_;
        if (peek() == '0')
            ++at_;
        else if (peek() >= '1' && peek() <= '9')
            while (peek() >= '0' && peek() <= '9')
                ++at_;
        else
            fail("Invalid number");
        if (peek() == '.') {
            ++at_;
            if (!(peek() >= '0' && peek() <= '9')) fail("Invalid number");
            while (peek() >= '0' && peek() <= '9')
                ++at_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++at_;
            if (peek() == '+' || peek() == '-') ++at_;
            if (!(peek() >= '0' && peek() <= '9')) fail("Invalid number");
            while (peek() >= '0' && peek() <= '9')
                ++at_;
        }
        double value{};
        const auto result = std::from_chars(text_.data() + start, text_.data() + at_, value);
        if (result.ec != std::errc{} || !std::isfinite(value)) {
            at_ = start;
            fail("Number out of range");
        }
        return Json(value);
    }

    std::string_view text_;
    std::size_t at_{};
};

void write_string(std::string& out, const std::string& value) {
    out += '"';
    for (const char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                out += "\\u00";
                out += digits[(c >> 4) & 0xf];
                out += digits[c & 0xf];
            } else
                out += c;
        }
    }
    out += '"';
}

void write(std::string& out, const Json& value, int indent, bool compact) {
    const auto newline = [&](int level) {
        if (compact) return;
        out += '\n';
        out.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    switch (value.type()) {
    case Json::Type::null:
        out += "null";
        break;
    case Json::Type::boolean:
        out += value.as_bool() ? "true" : "false";
        break;
    case Json::Type::number: {
        const double number = value.as_number();
        if (!std::isfinite(number)) throw std::invalid_argument("JSON cannot store a non-finite number");
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
        out.append(buffer, result.ptr);
        break;
    }
    case Json::Type::string:
        write_string(out, value.as_string());
        break;
    case Json::Type::array: {
        const auto& items = value.items();
        if (items.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += ',';
            newline(indent + 1);
            write(out, items[i], indent + 1, compact);
        }
        newline(indent);
        out += ']';
        break;
    }
    case Json::Type::object: {
        const auto& members = value.members();
        if (members.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (i) out += ',';
            newline(indent + 1);
            write_string(out, members[i].first);
            out += compact ? ":" : ": ";
            write(out, members[i].second, indent + 1, compact);
        }
        newline(indent);
        out += '}';
        break;
    }
    }
}
} // namespace

Json::Json(double value) : value_(value) {}

Json::Json(std::int64_t value) : value_(static_cast<double>(value)) {
    if (value < -exact_limit || value > exact_limit)
        throw std::out_of_range("Integer too large to store exactly in JSON");
}

bool Json::as_bool() const {
    if (const auto* value = std::get_if<bool>(&value_)) return *value;
    wrong_type("boolean");
}

double Json::as_number() const {
    if (const auto* value = std::get_if<double>(&value_)) return *value;
    wrong_type("number");
}

std::int64_t Json::as_int(std::int64_t minimum, std::int64_t maximum) const {
    const double value = as_number();
    if (value != std::floor(value) || value < static_cast<double>(minimum) ||
        value > static_cast<double>(maximum))
        throw std::runtime_error("Expected a whole number from " + std::to_string(minimum) + " to " +
                                 std::to_string(maximum));
    return static_cast<std::int64_t>(value);
}

const std::string& Json::as_string() const {
    if (const auto* value = std::get_if<std::string>(&value_)) return *value;
    wrong_type("string");
}

const Json::Items& Json::items() const {
    if (const auto* value = std::get_if<Items>(&value_)) return *value;
    wrong_type("array");
}

Json::Items& Json::items() {
    if (auto* value = std::get_if<Items>(&value_)) return *value;
    wrong_type("array");
}

const Json::Members& Json::members() const {
    if (const auto* value = std::get_if<Members>(&value_)) return *value;
    wrong_type("object");
}

Json::Members& Json::members() {
    if (auto* value = std::get_if<Members>(&value_)) return *value;
    wrong_type("object");
}

const Json* Json::find(std::string_view key) const {
    for (const auto& member : members())
        if (member.first == key) return &member.second;
    return nullptr;
}

const Json& Json::at(std::string_view key) const {
    if (const auto* value = find(key)) return *value;
    throw std::runtime_error("Missing field: " + std::string(key));
}

Json& Json::set(std::string_view key, Json value) {
    auto& list = members();
    for (auto& member : list)
        if (member.first == key) return member.second = std::move(value);
    list.emplace_back(std::string(key), std::move(value));
    return list.back().second;
}

std::size_t Json::size() const {
    if (is(Type::array)) return items().size();
    if (is(Type::object)) return members().size();
    wrong_type("array or object");
}

Json json_float(float value) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), value).ptr;
    double shortest{};
    std::from_chars(text, end, shortest);
    return Json(shortest);
}

Json parse_json(std::string_view text) {
    return Parser(text).document();
}

std::string to_json(const Json& value, bool compact) {
    std::string out;
    write(out, value, 0, compact);
    if (!compact) out += '\n';
    return out;
}
} // namespace seed
