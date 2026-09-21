// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

using namespace std;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using f32 = float;
using f64 = double;

template <typename T>
using sp = shared_ptr<T>;

#ifdef _DEBUG

extern bool debug;

// NOLINTBEGIN(bugprone-macro-parentheses)
#define DEBUG_LOG(msg)                                                                                                 \
    if (debug) {                                                                                                       \
        std::cerr << "[DEBUG] " << msg << '\n';                                                                        \
    }
#define DEBUG_LOG_VAL(msg, val)                                                                                        \
    if (debug) {                                                                                                       \
        std::cerr << "[DEBUG] " << msg << ": " << val << '\n';                                                         \
    }
// NOLINTEND(bugprone-macro-parentheses)

#else

#define DEBUG_LOG(msg)
#define DEBUG_LOG_VAL(msg, val)

#endif
