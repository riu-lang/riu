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
#include <iostream>
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

extern bool debug;

#define DEBUG_LOG(msg) if(debug) { std::cerr << "[DEBUG] " << msg << std::endl; }
#define DEBUG_LOG_VAL(msg, val) if(debug) { std::cerr << "[DEBUG] " << msg << ": " << val << std::endl; }

#else

#define DEBUG_LOG(msg)
#define DEBUG_LOG_VAL(msg, val)

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

enum class TypeKind : u8 {
    Normal,
    Generic,
    Array
};

struct TypeInfo {
    TypeKind kind = TypeKind::Normal;
    string name;
    u64 arraySize = 0;
    sp<TypeInfo> elementType = nullptr;
    vector<sp<TypeInfo>> genericArgs;
    
    TypeInfo() = default;
    
    explicit TypeInfo(string n) : kind(TypeKind::Normal), name(std::move(n)) {}
    
    TypeInfo(string n, vector<sp<TypeInfo>> args) :
        kind(TypeKind::Generic),
        name(std::move(n)),
        genericArgs(std::move(args)) {}
    
    TypeInfo(sp<TypeInfo> elemType, u64 size) :
        kind(TypeKind::Array),
        arraySize(size),
        elementType(std::move(elemType)) {
        name = "[" + elementType->name + " * " + to_string(arraySize) + "]";
    }
    
    [[nodiscard]] bool isArray() const { return kind == TypeKind::Array; }
    
    [[nodiscard]] bool isNormal() const { return kind == TypeKind::Normal; }
    
    [[nodiscard]] bool isGeneric() const { return kind == TypeKind::Generic; }
    
    [[nodiscard]] bool empty() const { return name.empty(); }
    
    [[nodiscard]] bool startsWith(char c) const { return !name.empty() && name[0] == c; }
    
    [[nodiscard]] bool isRef() const {
        return kind == TypeKind::Generic && name == "Ref" && genericArgs.size() == 1;
    }
    
    [[nodiscard]] sp<TypeInfo> refElementType() const {
        if (isRef() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }
    
    [[nodiscard]] bool isBox() const {
        return kind == TypeKind::Generic && name == "Box" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> boxElementType() const {
        if (isBox() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    [[nodiscard]] bool isPtr() const {
        return kind == TypeKind::Generic && name == "Ptr" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> ptrElementType() const {
        if (isPtr() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }
    
    string getFullName() const {
        if (kind == TypeKind::Generic && !genericArgs.empty()) {
            string result = name;
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                result += "_" + genericArgs[i]->getFullName();
            }
            return result;
        }
        return name;
    }
    
    bool operator==(const TypeInfo& other) const {
        if (kind != other.kind) return false;
        if (name != other.name) return false;
        if (kind == TypeKind::Array) {
            if (arraySize != other.arraySize) return false;
            if (!elementType && !other.elementType) return true;
            if (!elementType || !other.elementType) return false;
            return *elementType == *other.elementType;
        }
        if (kind == TypeKind::Generic) {
            if (genericArgs.size() != other.genericArgs.size()) return false;
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (!genericArgs[i] && !other.genericArgs[i]) continue;
                if (!genericArgs[i] || !other.genericArgs[i]) return false;
                if (*genericArgs[i] != *other.genericArgs[i]) return false;
            }
        }
        return true;
    }
    
    bool operator!=(const TypeInfo& other) const {
        return !(*this == other);
    }
};
