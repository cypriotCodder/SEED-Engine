#include "io/json.hpp"
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Exception = std::runtime_error, class F>
void rejects(F&& f, const char* message) {
    try {
        f();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void parsing() {
    const auto value = seed::parse_json(R"( {"name": "Isle", "version": 1, "ratio": -2.5e-1,
        "tags": ["a", true, null], "nested": {"empty": {}, "list": []}} )");
    check(value.at("name").as_string() == "Isle", "String member");
    check(value.at("version").as_int() == 1, "Integer member");
    check(value.at("ratio").as_number() == -0.25, "Exponent number");
    check(value.at("tags").size() == 3 && value.at("tags").items()[1].as_bool() &&
              value.at("tags").items()[2].is(seed::Json::Type::null),
          "Mixed array");
    check(value.at("nested").at("empty").size() == 0, "Empty object");
    check(!value.find("missing"), "Missing member is absent");
    rejects([&] { (void)value.at("missing"); }, "Missing member accepted by at()");
    rejects([&] { (void)value.at("name").as_number(); }, "String read as a number");
    rejects([&] { (void)value.at("ratio").as_int(); }, "Fraction read as an integer");
    rejects([&] { (void)seed::Json(300).as_int(0, 255); }, "Out-of-range integer accepted");

    const auto escaped = seed::parse_json(R"("tab\t quote\" slash\/ \u00e9 \ud83c\udf32")");
    check(escaped.as_string() == "tab\t quote\" slash/ \xc3\xa9 \xf0\x9f\x8c\xb2",
          "Escapes and surrogate pairs");
}

void malformed() {
    const char* bad[] = {"",
                         "{",
                         "[1,]",
                         "{\"a\":1,}",
                         "{'a':1}",
                         "01",
                         "1.",
                         ".5",
                         "-",
                         "1e",
                         "tru",
                         "\"open",
                         "\"\\x\"",
                         "\"\\ud800\"",
                         "[1] 2",
                         "// c\n{}",
                         "{\"a\":1,\"a\":2}",
                         "\"a\nb\"",
                         "1e999",
                         "NaN"};
    for (const char* text : bad)
        rejects<seed::JsonError>([&] { seed::parse_json(text); }, text);
    try {
        seed::parse_json("{\n  \"a\": 1,\n  \"b\" 2\n}");
        throw std::runtime_error("Missing colon accepted");
    } catch (const seed::JsonError& error) {
        check(error.line == 3 && error.column == 7, "Error position is line 3, column 7");
    }
    std::string deep(129, '['), closing(129, ']');
    rejects<seed::JsonError>([&] { seed::parse_json(deep + closing); }, "Excessive nesting accepted");
    std::string ok(128, '['), ok_closing(128, ']');
    seed::parse_json(ok + ok_closing);
}

void writing() {
    auto project = seed::Json::object();
    project.set("name", "My \"Game\"");
    project.set("version", 2);
    project.set("scale", 0.1);
    auto list = seed::Json::array();
    list.push(true);
    list.push(nullptr);
    project.set("list", list);
    project.set("empty", seed::Json::object());
    project.set("version", 3); // Replaces in place, keeping order.
    const auto text = seed::to_json(project);
    check(text == "{\n  \"name\": \"My \\\"Game\\\"\",\n  \"version\": 3,\n  \"scale\": 0.1,\n"
                  "  \"list\": [\n    true,\n    null\n  ],\n  \"empty\": {}\n}\n",
          "Pretty output with stable member order");
    check(seed::parse_json(text) == project, "Written JSON reads back equal");

    for (double number : {0.1, 1e-300, 123456789.125, -0.0, 3.0})
        check(seed::parse_json(seed::to_json(seed::Json(number))).as_number() == number, "Number round trip");
    check(seed::to_json(seed::Json(std::string("\x01"))) == "\"\\u0001\"\n", "Control characters escaped");
    rejects<std::invalid_argument>([] { (void)seed::to_json(seed::Json(1.0 / 0.0)); }, "Infinity written");
    rejects<std::out_of_range>([] { (void)seed::Json(std::int64_t(1) << 60); }, "Inexact integer stored");
}
} // namespace

int main() {
    try {
        parsing();
        malformed();
        writing();
        std::cout << "JSON parsing and writing checks passed.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
