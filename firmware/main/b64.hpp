// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jarvis {

inline std::string b64_encode(const std::uint8_t* data, std::size_t len)
{
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const unsigned n = (static_cast<unsigned>(data[i]) << 16) |
                           ((i + 1 < len ? data[i + 1] : 0) << 8) |
                           (i + 2 < len ? data[i + 2] : 0);
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? kTable[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? kTable[n & 63] : '=');
    }
    return out;
}

inline int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::vector<std::uint8_t> b64_decode(std::string_view in)
{
    std::vector<std::uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0;
    int bits = -8;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') break;
        const int d = b64_value(c);
        if (d < 0) continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

} // namespace jarvis
