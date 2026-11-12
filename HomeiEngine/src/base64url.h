#pragma once

#include <string>
#include <string_view>

namespace homei {

// Encodes raw UTF-8 string to base64url without '=' padding
std::string base64url_encode(std::string_view input);

// Decodes base64url (with or without '=' padding) to raw string.
// Returns false on invalid base64url characters or malformed length.
bool base64url_decode(std::string_view input, std::string& output);

} // namespace homei
