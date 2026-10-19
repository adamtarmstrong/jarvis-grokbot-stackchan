// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jarvis contributors
#pragma once

#include <string>
#include <string_view>

namespace jarvis {

// Must match worker/src/crypto.ts canonicalString byte for byte.
// JARVIS1\nMETHOD\nPATH\nDEVICE_ID\nTIMESTAMP\nNONCE\nHEX(SHA256(body))
inline std::string canonical_string(std::string_view method, std::string_view path, std::string_view device_id,
                                    std::string_view timestamp, std::string_view nonce,
                                    std::string_view body_sha256_hex)
{
    std::string upper;
    upper.reserve(method.size());
    for (unsigned char c : method) {
        if (c >= 'a' && c <= 'z') upper.push_back(static_cast<char>(c - 32));
        else upper.push_back(static_cast<char>(c));
    }
    std::string out;
    out.reserve(64 + path.size() + device_id.size() + body_sha256_hex.size());
    out += "JARVIS1\n";
    out += upper;
    out += '\n';
    out.append(path);
    out += '\n';
    out.append(device_id);
    out += '\n';
    out.append(timestamp);
    out += '\n';
    out.append(nonce);
    out += '\n';
    out.append(body_sha256_hex);
    return out;
}

} // namespace jarvis
