// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "uuid.h"

#include <array>
#include <cstdint>
#include <random>

namespace {

// One generator per thread, seeded once. std::random_device is used only for
// seeding (it can be slow, and on some platforms is not even random per call).
std::mt19937_64& Engine()
{
    static thread_local std::mt19937_64 engine([] {
        std::random_device rd;
        // random_device yields 32 bits at a time; mix two draws into the seed.
        uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        return std::mt19937_64(seed);
    }());
    return engine;
}

constexpr char kHex[] = "0123456789abcdef";

}  // namespace

std::string uuid::GenerateV4()
{
    // 128 random bits, then the version (4) and variant (10xx) nibbles fixed
    // per RFC 4122 §4.4.
    uint64_t hi = Engine()();
    uint64_t lo = Engine()();

    std::array<uint8_t, 16> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[i]     = static_cast<uint8_t>((hi >> (8 * (7 - i))) & 0xFF);
        bytes[8 + i] = static_cast<uint8_t>((lo >> (8 * (7 - i))) & 0xFF);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);  // variant 10xx

    std::string out;
    out.reserve(36);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out.push_back('-');
        out.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

bool uuid::IsV4(const std::string& s)
{
    if (s.size() != 36) return false;

    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
            continue;
        }
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!isHex) return false;
    }
    return s[14] == '4';  // version nibble
}
