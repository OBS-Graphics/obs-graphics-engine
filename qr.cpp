// Adapted from https://github.com/nth-eye/qr
// Original work Copyright (c) nth-eye contributors — BSD-3-Clause
// Modifications Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com> — MIT

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4267) // vendored qr.hpp: narrowing size_t/long → int/uint16_t
#endif

#define QR_IMPLEMENTATION
#include "qr.hpp"

#ifdef _MSC_VER
#pragma warning(pop)
#endif