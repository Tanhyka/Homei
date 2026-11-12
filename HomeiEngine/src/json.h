#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace homei {

struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Int,
        String,
        Array,
        Object
    };

    Type type = Type::Null;
    bool bool_val = false;
    int64_t int_val = 0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::map<std::string, JsonValue> obj_val;

    static JsonValue make_null() { return JsonValue{}; }
    static JsonValue make_bool(bool b) { JsonValue v; v.type = Type::Bool; v.bool_val = b; return v; }
    static JsonValue make_int(int64_t n) { JsonValue v; v.type = Type::Int; v.int_val = n; return v; }
    static JsonValue make_string(std::string s) { JsonValue v; v.type = Type::String; v.str_val = std::move(s); return v; }
    static JsonValue make_array() { JsonValue v; v.type = Type::Array; return v; }
    static JsonValue make_object() { JsonValue v; v.type = Type::Object; return v; }

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_int() const { return type == Type::Int; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    bool has_key(const std::string& k) const {
        return type == Type::Object && obj_val.find(k) != obj_val.end();
    }

    const JsonValue* get(const std::string& k) const {
        if (type != Type::Object) return nullptr;
        auto it = obj_val.find(k);
        return (it != obj_val.end()) ? &it->second : nullptr;
    }
};

// Parse JSON string into out. Returns false and sets err on failure.
bool json_parse(std::string_view input, JsonValue& out, std::string& err);

// Serialize JsonValue to compact string (sort_keys=true, separators=(",", ":"))
std::string json_stringify(const JsonValue& v);

} // namespace homei
