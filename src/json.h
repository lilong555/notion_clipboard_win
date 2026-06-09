#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ncw
{
enum class JsonType
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

class JsonValue
{
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    static JsonValue MakeNull()
    {
        return JsonValue();
    }

    static JsonValue MakeBool(bool value)
    {
        JsonValue json;
        json.type_ = JsonType::Bool;
        json.bool_value_ = value;
        return json;
    }

    static JsonValue MakeNumber(double value)
    {
        JsonValue json;
        json.type_ = JsonType::Number;
        json.number_value_ = value;
        return json;
    }

    static JsonValue MakeString(std::string value)
    {
        JsonValue json;
        json.type_ = JsonType::String;
        json.string_value_ = std::move(value);
        return json;
    }

    static JsonValue MakeArray(Array value)
    {
        JsonValue json;
        json.type_ = JsonType::Array;
        json.array_value_ = std::move(value);
        return json;
    }

    static JsonValue MakeObject(Object value)
    {
        JsonValue json;
        json.type_ = JsonType::Object;
        json.object_value_ = std::move(value);
        return json;
    }

    bool is_null() const
    {
        return type_ == JsonType::Null;
    }

    bool is_bool() const
    {
        return type_ == JsonType::Bool;
    }

    bool is_number() const
    {
        return type_ == JsonType::Number;
    }

    bool is_string() const
    {
        return type_ == JsonType::String;
    }

    bool is_array() const
    {
        return type_ == JsonType::Array;
    }

    bool is_object() const
    {
        return type_ == JsonType::Object;
    }

    bool as_bool() const
    {
        if (!is_bool())
        {
            throw std::runtime_error("JSON 类型不是 bool");
        }
        return bool_value_;
    }

    double as_number() const
    {
        if (!is_number())
        {
            throw std::runtime_error("JSON 类型不是 number");
        }
        return number_value_;
    }

    const std::string &as_string() const
    {
        if (!is_string())
        {
            throw std::runtime_error("JSON 类型不是 string");
        }
        return string_value_;
    }

    const Array &as_array() const
    {
        if (!is_array())
        {
            throw std::runtime_error("JSON 类型不是 array");
        }
        return array_value_;
    }

    const Object &as_object() const
    {
        if (!is_object())
        {
            throw std::runtime_error("JSON 类型不是 object");
        }
        return object_value_;
    }

    const JsonValue *find(const std::string &key) const
    {
        if (!is_object())
        {
            return nullptr;
        }
        const auto it = object_value_.find(key);
        if (it == object_value_.end())
        {
            return nullptr;
        }
        return &it->second;
    }

private:
    JsonType type_ = JsonType::Null;
    bool bool_value_ = false;
    double number_value_ = 0.0;
    std::string string_value_;
    Array array_value_;
    Object object_value_;
};

JsonValue ParseJson(const std::string &text);
}
