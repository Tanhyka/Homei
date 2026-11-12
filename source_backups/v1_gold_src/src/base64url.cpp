#include "base64url.h"
#include <cstdint>

namespace homei {

namespace {

constexpr char B64URL_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

inline int b64url_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

} // namespace

std::string base64url_encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    const auto* data = reinterpret_cast<const unsigned char*>(input.data());
    size_t len = input.size();
    while (i + 3 <= len) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(B64URL_TABLE[(v >> 18) & 0x3F]);
        out.push_back(B64URL_TABLE[(v >> 12) & 0x3F]);
        out.push_back(B64URL_TABLE[(v >> 6) & 0x3F]);
        out.push_back(B64URL_TABLE[v & 0x3F]);
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(B64URL_TABLE[(v >> 18) & 0x3F]);
        out.push_back(B64URL_TABLE[(v >> 12) & 0x3F]);
    } else if (rem == 2) {
        uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(B64URL_TABLE[(v >> 18) & 0x3F]);
        out.push_back(B64URL_TABLE[(v >> 12) & 0x3F]);
        out.push_back(B64URL_TABLE[(v >> 6) & 0x3F]);
    }
    return out;
}

bool base64url_decode(std::string_view input, std::string& output) {
    output.clear();
    // Trim whitespace
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t' || input.front() == '\r' || input.front() == '\n')) {
        input.remove_prefix(1);
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t' || input.back() == '\r' || input.back() == '\n')) {
        input.remove_suffix(1);
    }
    if (input.empty()) return false;

    // Strip trailing '=' padding
    size_t pad_count = 0;
    while (!input.empty() && input.back() == '=') {
        input.remove_suffix(1);
        ++pad_count;
        if (pad_count > 2) return false;
    }

    size_t len = input.size();
    if (len % 4 == 1) return false;

    output.reserve((len * 3) / 4);
    size_t i = 0;
    while (i + 4 <= len) {
        int a = b64url_val(static_cast<unsigned char>(input[i]));
        int b = b64url_val(static_cast<unsigned char>(input[i + 1]));
        int c = b64url_val(static_cast<unsigned char>(input[i + 2]));
        int d = b64url_val(static_cast<unsigned char>(input[i + 3]));
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        uint32_t v = (static_cast<uint32_t>(a) << 18) |
                     (static_cast<uint32_t>(b) << 12) |
                     (static_cast<uint32_t>(c) << 6) |
                     static_cast<uint32_t>(d);
        output.push_back(static_cast<char>((v >> 16) & 0xFF));
        output.push_back(static_cast<char>((v >> 8) & 0xFF));
        output.push_back(static_cast<char>(v & 0xFF));
        i += 4;
    }
    size_t rem = len - i;
    if (rem == 2) {
        int a = b64url_val(static_cast<unsigned char>(input[i]));
        int b = b64url_val(static_cast<unsigned char>(input[i + 1]));
        if (a < 0 || b < 0) return false;
        uint32_t v = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12);
        output.push_back(static_cast<char>((v >> 16) & 0xFF));
    } else if (rem == 3) {
        int a = b64url_val(static_cast<unsigned char>(input[i]));
        int b = b64url_val(static_cast<unsigned char>(input[i + 1]));
        int c = b64url_val(static_cast<unsigned char>(input[i + 2]));
        if (a < 0 || b < 0 || c < 0) return false;
        uint32_t v = (static_cast<uint32_t>(a) << 18) |
                     (static_cast<uint32_t>(b) << 12) |
                     (static_cast<uint32_t>(c) << 6);
        output.push_back(static_cast<char>((v >> 16) & 0xFF));
        output.push_back(static_cast<char>((v >> 8) & 0xFF));
    }
    return true;
}

} // namespace homei
