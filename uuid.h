// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include <string>

namespace uuid {

// A random (version 4) UUID in the canonical 8-4-4-4-12 lowercase hex form,
// e.g. "9f2c41e8-3b7a-4c1d-8e55-1a2b3c4d5e6f". Used for the identity of both
// IDataSource (data-source.h) and Title (title.h) — a Title refers to its
// source by that source's id, and a Lua script refers to a Title by its id.
std::string GenerateV4();

// True if `s` looks like a canonical v4 UUID (length, dash positions, hex
// digits, version nibble '4'). Used by Title::Load to tell a real id from a
// pre-1.1 .ogt file's human-readable "id" (which becomes `name` instead).
bool IsV4(const std::string& s);

}  // namespace uuid
