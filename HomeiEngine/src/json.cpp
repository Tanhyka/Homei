#include "json.h"
#include <cctype>
#include <cstdio>

namespace homei {

namespace {

struct Parser {
    std::string_view s;
    size_t pos = 0;
    std::string& err;

    void skip_ws() {
        while (pos < s.size()) {
            char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > 64) {
            err = "JSON nesting too deep";
            return false;
        }
        skip_ws();
        if (pos >= s.size()) {
            err = "Unexpected end of JSON";
            return false;
        }
        char c = s[pos];
        if (c == '{') return parse_object(out, depth + 1);
        if (c == '[') return parse_array(out, depth + 1);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parse_string(out.str_val);
        }
        if (c == 't') {
            if (s.substr(pos, 4) == "true") {
                pos += 4;
                out = JsonValue::make_bool(true);
                return true;
            }
            err = "Invalid token";
            return false;
        }
        if (c == 'f') {
            if (s.substr(pos, 5) == "false") {
                pos += 5;
                out = JsonValue::make_bool(false);
                return true;
            }
            err = "Invalid token";
            return false;
        }
        if (c == 'n') {
            if (s.substr(pos, 4) == "null") {
                pos += 4;
                out = JsonValue::make_null();
                return true;
            }
            err = "Invalid token";
            return false;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(out);
        }
        err = "Unexpected character in JSON";
        return false;
    }

    static void append_utf8(std::string& str, uint32_t cp) {
        if (cp <= 0x7F) {
            str.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            str.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            str.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            str.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            str.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            str.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            str.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(uint32_t& out_hex) {
        if (pos + 4 > s.size()) return false;
        out_hex = 0;
        for (int i = 0; i < 4; ++i) {
            char ch = s[pos++];
            out_hex <<= 4;
            if (ch >= '0' && ch <= '9') out_hex |= (ch - '0');
            else if (ch >= 'a' && ch <= 'f') out_hex |= (ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') out_hex |= (ch - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parse_string(std::string& out) {
        out.clear();
        if (pos >= s.size() || s[pos] != '"') {
            err = "Expected '\"'";
            return false;
        }
        ++pos;
        while (pos < s.size()) {
            unsigned char ch = static_cast<unsigned char>(s[pos++]);
            if (ch == '"') {
                return true;
            }
            if (ch < 0x20) {
                err = "Unescaped control char in string";
                return false;
            }
            if (ch == '\\') {
                if (pos >= s.size()) {
                    err = "Unterminated escape in string";
                    return false;
                }
                char esc = s[pos++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!parse_hex4(cp)) {
                            err = "Invalid \\u escape";
                            return false;
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos + 2 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                                pos += 2;
                                uint32_t low = 0;
                                if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                                    err = "Invalid surrogate pair";
                                    return false;
                                }
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                            } else {
                                err = "Unpaired high surrogate";
                                return false;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            err = "Unpaired low surrogate";
                            return false;
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        err = "Invalid escape sequence";
                        return false;
                }
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
        err = "Unterminated string";
        return false;
    }

    bool parse_number(JsonValue& out) {
        bool neg = false;
        if (s[pos] == '-') {
            neg = true;
            ++pos;
            if (pos >= s.size()) {
                err = "Invalid number";
                return false;
            }
        }
        if (s[pos] < '0' || s[pos] > '9') {
            err = "Invalid number";
            return false;
        }
        if (s[pos] == '0' && pos + 1 < s.size() && s[pos + 1] >= '0' && s[pos + 1] <= '9') {
            err = "Leading zero in number";
            return false;
        }
        uint64_t val = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            uint64_t d = static_cast<uint64_t>(s[pos] - '0');
            if (val > (9223372036854775807ULL - d) / 10) {
                err = "Integer overflow in JSON";
                return false;
            }
            val = val * 10 + d;
            ++pos;
        }
        if (pos < s.size() && (s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E')) {
            err = "Floating point numbers not supported in CLP JSON";
            return false;
        }
        out.type = JsonValue::Type::Int;
        out.int_val = neg ? -static_cast<int64_t>(val) : static_cast<int64_t>(val);
        return true;
    }

    bool parse_array(JsonValue& out, int depth) {
        out = JsonValue::make_array();
        ++pos; // skip '['
        skip_ws();
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!parse_value(elem, depth)) return false;
            out.arr_val.push_back(std::move(elem));
            skip_ws();
            if (pos >= s.size()) {
                err = "Unterminated array";
                return false;
            }
            if (s[pos] == ']') {
                ++pos;
                return true;
            }
            if (s[pos] != ',') {
                err = "Expected ',' or ']' in array";
                return false;
            }
            ++pos;
        }
    }

    bool parse_object(JsonValue& out, int depth) {
        out = JsonValue::make_object();
        ++pos; // skip '{'
        skip_ws();
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos >= s.size() || s[pos] != '"') {
                err = "Expected string key in object";
                return false;
            }
            std::string key;
            if (!parse_string(key)) return false;
            if (out.obj_val.find(key) != out.obj_val.end()) {
                err = "Duplicate key in JSON object: " + key;
                return false;
            }
            skip_ws();
            if (pos >= s.size() || s[pos] != ':') {
                err = "Expected ':' after object key";
                return false;
            }
            ++pos;
            JsonValue val;
            if (!parse_value(val, depth)) return false;
            out.obj_val.emplace(std::move(key), std::move(val));
            skip_ws();
            if (pos >= s.size()) {
                err = "Unterminated object";
                return false;
            }
            if (s[pos] == '}') {
                ++pos;
                return true;
            }
            if (s[pos] != ',') {
                err = "Expected ',' or '}' in object";
                return false;
            }
            ++pos;
        }
    }
};

void stringify_into(const JsonValue& v, std::string& out) {
    switch (v.type) {
        case JsonValue::Type::Null:
            out.append("null");
            break;
        case JsonValue::Type::Bool:
            out.append(v.bool_val ? "true" : "false");
            break;
        case JsonValue::Type::Int:
            out.append(std::to_string(v.int_val));
            break;
        case JsonValue::Type::String: {
            out.push_back('"');
            for (unsigned char c : v.str_val) {
                switch (c) {
                    case '"': out.append("\\\""); break;
                    case '\\': out.append("\\\\"); break;
                    case '\b': out.append("\\b"); break;
                    case '\f': out.append("\\f"); break;
                    case '\n': out.append("\\n"); break;
                    case '\r': out.append("\\r"); break;
                    case '\t': out.append("\\t"); break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out.append(buf);
                        } else {
                            out.push_back(static_cast<char>(c));
                        }
                        break;
                }
            }
            out.push_back('"');
            break;
        }
        case JsonValue::Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < v.arr_val.size(); ++i) {
                if (i > 0) out.push_back(',');
                stringify_into(v.arr_val[i], out);
            }
            out.push_back(']');
            break;
        }
        case JsonValue::Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, val] : v.obj_val) {
                if (!first) out.push_back(',');
                first = false;
                JsonValue key_val = JsonValue::make_string(k);
                stringify_into(key_val, out);
                out.push_back(':');
                stringify_into(val, out);
            }
            out.push_back('}');
            break;
        }
    }
}

} // namespace

bool json_parse(std::string_view input, JsonValue& out, std::string& err) {
    if (input.size() > 1048576) {
        err = "JSON exceeds 1 MiB limit";
        return false;
    }
    Parser p{input, 0, err};
    if (!p.parse_value(out, 0)) return false;
    p.skip_ws();
    if (p.pos != input.size()) {
        err = "Trailing characters after JSON";
        return false;
    }
    return true;
}

std::string json_stringify(const JsonValue& v) {
    std::string out;
    out.reserve(256);
    stringify_into(v, out);
    return out;
}

} // namespace homei
