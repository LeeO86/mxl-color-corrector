#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cc
{

class Json
{
public:
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    static Json nul();
    static Json boolean(bool value);
    static Json number(double value);
    static Json str(std::string value);
    static Json array();
    static Json object();

    [[nodiscard]] bool isNull() const { return type == Type::Null; }
    [[nodiscard]] bool isBool() const { return type == Type::Bool; }
    [[nodiscard]] bool isNumber() const { return type == Type::Number; }
    [[nodiscard]] bool isString() const { return type == Type::String; }
    [[nodiscard]] bool isArray() const { return type == Type::Array; }
    [[nodiscard]] bool isObject() const { return type == Type::Object; }

    [[nodiscard]] Json const* find(std::string const& key) const;
    Json& operator[](std::string const& key);
    void push(Json value);

    [[nodiscard]] double num(double fallback = 0) const;
    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::string dump() const;
};

class JsonError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] Json parseJson(std::string_view text);

} // namespace cc
