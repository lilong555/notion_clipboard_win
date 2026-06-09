#include "json.h"

#include <cctype>
#include <cstdint>

namespace ncw
{
namespace
{
class JsonParser
{
public:
    explicit JsonParser(std::string input) : input_(std::move(input)) {}

    JsonValue Parse()
    {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (pos_ != input_.size())
        {
            throw std::runtime_error("JSON 末尾存在多余内容");
        }
        return value;
    }

private:
    JsonValue ParseValue()
    {
        if (pos_ >= input_.size())
        {
            throw std::runtime_error("JSON 内容不完整");
        }

        const char ch = input_[pos_];
        if (ch == 'n')
        {
            ConsumeLiteral("null");
            return JsonValue::MakeNull();
        }
        if (ch == 't')
        {
            ConsumeLiteral("true");
            return JsonValue::MakeBool(true);
        }
        if (ch == 'f')
        {
            ConsumeLiteral("false");
            return JsonValue::MakeBool(false);
        }
        if (ch == '"')
        {
            return JsonValue::MakeString(ParseString());
        }
        if (ch == '[')
        {
            return ParseArray();
        }
        if (ch == '{')
        {
            return ParseObject();
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)))
        {
            return ParseNumber();
        }
        throw std::runtime_error("JSON value 起始字符不合法");
    }

    JsonValue ParseArray()
    {
        Expect('[');
        SkipWhitespace();
        JsonValue::Array array;
        if (Match(']'))
        {
            return JsonValue::MakeArray(std::move(array));
        }

        while (true)
        {
            SkipWhitespace();
            array.push_back(ParseValue());
            SkipWhitespace();
            if (Match(']'))
            {
                break;
            }
            Expect(',');
        }

        return JsonValue::MakeArray(std::move(array));
    }

    JsonValue ParseObject()
    {
        Expect('{');
        SkipWhitespace();
        JsonValue::Object object;
        if (Match('}'))
        {
            return JsonValue::MakeObject(std::move(object));
        }

        while (true)
        {
            SkipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != '"')
            {
                throw std::runtime_error("JSON object key 必须是 string");
            }
            std::string key = ParseString();
            SkipWhitespace();
            Expect(':');
            SkipWhitespace();
            object.emplace(std::move(key), ParseValue());
            SkipWhitespace();
            if (Match('}'))
            {
                break;
            }
            Expect(',');
        }

        return JsonValue::MakeObject(std::move(object));
    }

    JsonValue ParseNumber()
    {
        const std::size_t start = pos_;
        Match('-');
        if (Match('0'))
        {
            // 单个 0 已经消耗，继续处理小数或指数。
        }
        else
        {
            ConsumeDigits();
        }

        if (Match('.'))
        {
            ConsumeDigits();
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-'))
            {
                ++pos_;
            }
            ConsumeDigits();
        }

        return JsonValue::MakeNumber(std::stod(input_.substr(start, pos_ - start)));
    }

    std::string ParseString()
    {
        Expect('"');
        std::string output;
        while (pos_ < input_.size())
        {
            const char ch = input_[pos_++];
            if (ch == '"')
            {
                return output;
            }
            if (ch != '\\')
            {
                output.push_back(ch);
                continue;
            }
            if (pos_ >= input_.size())
            {
                throw std::runtime_error("JSON string escape 不完整");
            }
            const char escaped = input_[pos_++];
            switch (escaped)
            {
            case '"':
                output.push_back('"');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case '/':
                output.push_back('/');
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u':
                ParseUnicodeEscape(&output);
                break;
            default:
                throw std::runtime_error("JSON string escape 不支持");
            }
        }
        throw std::runtime_error("JSON string 未闭合");
    }

    void ParseUnicodeEscape(std::string *output)
    {
        if (pos_ + 4 > input_.size())
        {
            throw std::runtime_error("JSON unicode escape 不完整");
        }
        std::uint32_t codepoint = ParseHex4(pos_);
        pos_ += 4;

        if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
        {
            if (pos_ + 6 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u')
            {
                throw std::runtime_error("JSON unicode 高代理项缺少低代理项");
            }
            pos_ += 2;
            const std::uint32_t low = ParseHex4(pos_);
            pos_ += 4;
            if (low < 0xDC00 || low > 0xDFFF)
            {
                throw std::runtime_error("JSON unicode 低代理项不合法");
            }
            codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
        }

        AppendUtf8(output, codepoint);
    }

    std::uint32_t ParseHex4(std::size_t pos) const
    {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const char ch = input_[pos + i];
            value <<= 4;
            if (ch >= '0' && ch <= '9')
            {
                value += static_cast<std::uint32_t>(ch - '0');
            }
            else if (ch >= 'a' && ch <= 'f')
            {
                value += static_cast<std::uint32_t>(ch - 'a' + 10);
            }
            else if (ch >= 'A' && ch <= 'F')
            {
                value += static_cast<std::uint32_t>(ch - 'A' + 10);
            }
            else
            {
                throw std::runtime_error("JSON unicode escape 包含非法十六进制字符");
            }
        }
        return value;
    }

    static void AppendUtf8(std::string *output, std::uint32_t codepoint)
    {
        if (codepoint <= 0x7F)
        {
            output->push_back(static_cast<char>(codepoint));
        }
        else if (codepoint <= 0x7FF)
        {
            output->push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint <= 0xFFFF)
        {
            output->push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else
        {
            output->push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    void ConsumeLiteral(const char *literal)
    {
        const std::string text(literal);
        if (input_.compare(pos_, text.size(), text) != 0)
        {
            throw std::runtime_error("JSON literal 不匹配");
        }
        pos_ += text.size();
    }

    void ConsumeDigits()
    {
        const std::size_t start = pos_;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_])))
        {
            ++pos_;
        }
        if (start == pos_)
        {
            throw std::runtime_error("JSON number 缺少数字");
        }
    }

    void SkipWhitespace()
    {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_])))
        {
            ++pos_;
        }
    }

    void Expect(char ch)
    {
        if (pos_ >= input_.size() || input_[pos_] != ch)
        {
            throw std::runtime_error("JSON 结构不符合预期");
        }
        ++pos_;
    }

    bool Match(char ch)
    {
        if (pos_ < input_.size() && input_[pos_] == ch)
        {
            ++pos_;
            return true;
        }
        return false;
    }

    std::string input_;
    std::size_t pos_ = 0;
};
}

JsonValue ParseJson(const std::string &text)
{
    return JsonParser(text).Parse();
}
}
