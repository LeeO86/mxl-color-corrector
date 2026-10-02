#include "util/json.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace cc
{
namespace
{
struct Parser
{
    std::string_view text;
    std::size_t i = 0;

    [[noreturn]] void fail(char const* what) const
    {
        throw JsonError(what);
    }

    void skip()
    {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t'))
        {
            ++i;
        }
    }

    char peek()
    {
        skip();
        if (i >= text.size())
        {
            fail("unexpected end of json");
        }
        return text[i];
    }

    char get()
    {
        char const c = peek();
        ++i;
        return c;
    }

    Json parseValue()
    {
        char const c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Json::str(parseString());
        if (c == 't' || c == 'f') return Json::boolean(parseBool());
        if (c == 'n')
        {
            parseNull();
            return Json::nul();
        }
        return Json::number(parseNumber());
    }

    Json parseObject()
    {
        if (get() != '{') fail("expected object");
        Json obj = Json::object();
        skip();
        if (peek() == '}')
        {
            ++i;
            return obj;
        }
        while (true)
        {
            if (peek() != '"') fail("expected key");
            auto key = parseString();
            if (get() != ':') fail("expected colon");
            obj.o.emplace_back(std::move(key), parseValue());
            char const sep = get();
            if (sep == '}') return obj;
            if (sep != ',') fail("expected comma");
        }
    }

    Json parseArray()
    {
        if (get() != '[') fail("expected array");
        Json arr = Json::array();
        skip();
        if (peek() == ']')
        {
            ++i;
            return arr;
        }
        while (true)
        {
            arr.push(parseValue());
            char const sep = get();
            if (sep == ']') return arr;
            if (sep != ',') fail("expected comma");
        }
    }

    std::string parseString()
    {
        if (get() != '"') fail("expected string");
        std::string out;
        while (i < text.size())
        {
            char c = text[i++];
            if (c == '"') return out;
            if (c == '\\')
            {
                if (i >= text.size()) fail("bad escape");
                char e = text[i++];
                switch (e)
                {
                    case '"':
                    case '\\':
                    case '/':
                        out.push_back(e);
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u':
                    {
                        if (i + 4 > text.size()) fail("bad unicode escape");
                        int code = 0;
                        for (int k = 0; k < 4; ++k)
                        {
                            char h = text[i++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code += h - '0';
                            else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
                            else fail("bad unicode escape");
                        }
                        if (code < 0x80)
                        {
                            out.push_back(static_cast<char>(code));
                        }
                        else if (code < 0x800)
                        {
                            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                        }
                        else
                        {
                            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                        }
                        break;
                    }
                    default:
                        fail("bad escape");
                }
            }
            else
            {
                out.push_back(c);
            }
        }
        fail("unterminated string");
    }

    bool parseBool()
    {
        if (text.substr(i, 4) == "true")
        {
            i += 4;
            return true;
        }
        if (text.substr(i, 5) == "false")
        {
            i += 5;
            return false;
        }
        fail("expected boolean");
    }

    void parseNull()
    {
        if (text.substr(i, 4) != "null") fail("expected null");
        i += 4;
    }

    double parseNumber()
    {
        std::size_t start = i;
        if (text[i] == '-') ++i;
        if (i >= text.size() || text[i] < '0' || text[i] > '9') fail("expected number");
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (i < text.size() && text[i] == '.')
        {
            ++i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        }
        if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
        {
            ++i;
            if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        }
        try
        {
            return std::stod(std::string(text.substr(start, i - start)));
        }
        catch (...)
        {
            fail("bad number");
        }
    }
};

void dumpString(std::ostream& os, std::string const& s)
{
    os << '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':
                os << "\\\"";
                break;
            case '\\':
                os << "\\\\";
                break;
            case '\b':
                os << "\\b";
                break;
            case '\f':
                os << "\\f";
                break;
            case '\n':
                os << "\\n";
                break;
            case '\r':
                os << "\\r";
                break;
            case '\t':
                os << "\\t";
                break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                }
                else
                {
                    os << static_cast<char>(c);
                }
        }
    }
    os << '"';
}

void dumpInto(std::ostream& os, Json const& j)
{
    switch (j.type)
    {
        case Json::Type::Null:
            os << "null";
            break;
        case Json::Type::Bool:
            os << (j.b ? "true" : "false");
            break;
        case Json::Type::Number:
        {
            if (std::isfinite(j.n) && std::floor(j.n) == j.n && std::fabs(j.n) < 1e15)
            {
                os << static_cast<long long>(j.n);
            }
            else
            {
                os.setf(std::ios::fmtflags(0), std::ios::floatfield);
                os.precision(15);
                os << j.n;
            }
            break;
        }
        case Json::Type::String:
            dumpString(os, j.s);
            break;
        case Json::Type::Array:
            os << '[';
            for (std::size_t i = 0; i < j.a.size(); ++i)
            {
                if (i) os << ',';
                dumpInto(os, j.a[i]);
            }
            os << ']';
            break;
        case Json::Type::Object:
            os << '{';
            for (std::size_t i = 0; i < j.o.size(); ++i)
            {
                if (i) os << ',';
                dumpString(os, j.o[i].first);
                os << ':';
                dumpInto(os, j.o[i].second);
            }
            os << '}';
            break;
    }
}
} // namespace

Json Json::nul()
{
    return {};
}
Json Json::boolean(bool value)
{
    Json j;
    j.type = Type::Bool;
    j.b = value;
    return j;
}
Json Json::number(double value)
{
    Json j;
    j.type = Type::Number;
    j.n = value;
    return j;
}
Json Json::str(std::string value)
{
    Json j;
    j.type = Type::String;
    j.s = std::move(value);
    return j;
}
Json Json::array()
{
    Json j;
    j.type = Type::Array;
    return j;
}
Json Json::object()
{
    Json j;
    j.type = Type::Object;
    return j;
}

Json const* Json::find(std::string const& key) const
{
    if (type != Type::Object) return nullptr;
    for (auto const& kv : o)
    {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Json& Json::operator[](std::string const& key)
{
    if (type != Type::Object)
    {
        type = Type::Object;
        o.clear();
    }
    for (auto& kv : o)
    {
        if (kv.first == key) return kv.second;
    }
    o.emplace_back(key, Json{});
    return o.back().second;
}

void Json::push(Json value)
{
    if (type != Type::Array)
    {
        type = Type::Array;
        a.clear();
    }
    a.push_back(std::move(value));
}

double Json::num(double fallback) const
{
    if (type == Type::Number) return n;
    if (type == Type::String)
    {
        try
        {
            return std::stod(s);
        }
        catch (...)
        {
            return fallback;
        }
    }
    if (type == Type::Bool) return b ? 1 : 0;
    return fallback;
}

std::string Json::text() const
{
    if (type == Type::String) return s;
    if (type == Type::Number)
    {
        std::ostringstream os;
        if (std::floor(n) == n && std::fabs(n) < 1e15) os << static_cast<long long>(n);
        else
        {
            os.precision(15);
            os << n;
        }
        return os.str();
    }
    if (type == Type::Bool) return b ? "true" : "false";
    return {};
}

std::string Json::dump() const
{
    std::ostringstream os;
    dumpInto(os, *this);
    return os.str();
}

Json parseJson(std::string_view text)
{
    Parser p{text, 0};
    Json v = p.parseValue();
    p.skip();
    if (p.i != text.size())
    {
        throw JsonError("trailing data");
    }
    return v;
}

} // namespace cc
