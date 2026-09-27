#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace seed {
// A parse error with the 1-based line and column where it was found.
class JsonError final : public std::runtime_error {
public:
    JsonError(const std::string& message, std::size_t line, std::size_t column)
        : std::runtime_error(message + " at line " + std::to_string(line) + ", column " +
                             std::to_string(column)),
          line(line),
          column(column) {}
    std::size_t line, column;
};

// A JSON value for project files. Objects keep their members in insertion order so files written
// by the editor diff cleanly; duplicate keys are rejected when parsing.
class Json final {
public:
    enum class Type { null, boolean, number, string, array, object };
    using Members = std::vector<std::pair<std::string, Json>>;
    using Items = std::vector<Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(double value);
    Json(int value) : Json(static_cast<double>(value)) {}
    Json(std::int64_t value);
    Json(std::string value) : value_(std::move(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    Json(const char* value) : value_(std::string(value)) {}
    static Json array() { return Json(Items{}); }
    static Json object() { return Json(Members{}); }

    Type type() const { return static_cast<Type>(value_.index()); }
    bool is(Type type) const { return this->type() == type; }

    // Typed reads throw std::runtime_error naming the expected type when the value differs.
    bool as_bool() const;
    double as_number() const;
    // The number as an integer; throws unless it is integral and within [minimum, maximum].
    std::int64_t as_int(std::int64_t minimum = -(std::int64_t(1) << 53),
                        std::int64_t maximum = std::int64_t(1) << 53) const;
    const std::string& as_string() const;
    const Items& items() const;
    Items& items();
    const Members& members() const;

    // Object access. find returns nullptr for a missing key; at throws "Missing field: key".
    const Json* find(std::string_view key) const;
    const Json& at(std::string_view key) const;
    // Sets a member, replacing an existing one in place or appending a new one.
    Json& set(std::string_view key, Json value);
    void push(Json value) { items().push_back(std::move(value)); }
    std::size_t size() const;

    bool operator==(const Json&) const = default;

private:
    explicit Json(Items items) : value_(std::move(items)) {}
    explicit Json(Members members) : value_(std::move(members)) {}
    Members& members();
    std::variant<std::nullptr_t, bool, double, std::string, Items, Members> value_;
};

// A float as the shortest decimal that reads back to the same float, so 0.9991F is written as
// 0.9991 rather than as its exact double expansion.
Json json_float(float value);

// Parses strict RFC 8259 JSON: no comments, no trailing commas, at most 128 levels of nesting.
Json parse_json(std::string_view text);
// Writes JSON with two-space indentation and a trailing newline. Numbers use the shortest text
// that reads back to the same double; non-finite numbers are rejected.
std::string to_json(const Json& value);
} // namespace seed
