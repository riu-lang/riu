// Copyright (c) 2025-2026. Yin-Jinlong@github

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <map>
#include <xstring>
#include <exception>
#include <stdexcept>
#include "antlr4-runtime.h"

using namespace std;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
// typedef __int128 i128;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
// typedef unsigned __int128 u128;

typedef float f32;
typedef double f64;

template <typename T>
using sp = shared_ptr<T>;

template <typename T>
using p = T*;

#ifdef _DEBUG

// 用于输出调试
extern bool debug;

#endif

using Token = antlr4::Token*;

class YuxError : public std::runtime_error {
public:
    explicit YuxError(const string& msg) : runtime_error(msg) {
    }

    template <class... _Types>
    explicit YuxError(const format_string<_Types...> format, _Types&&... args) : runtime_error(
        std::vformat(format.get(), std::make_format_args(args...))) {
    }
};

template <typename T>
p<T> any_cast_p(const std::any& a) {
    return std::any_cast<p<T>>(a);
}
