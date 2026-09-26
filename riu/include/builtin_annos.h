// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// #6 内置注解 schema（编译器白名单；不读 SDK `#Anno struct` 声明）。

#ifndef RIU_LANG_BUILTIN_ANNOS_H
#define RIU_LANG_BUILTIN_ANNOS_H

#include "types.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

enum class AnnoAttachSite : u8 {
    Code,
    StructDecl,
    FnDecl,
    ExternFn,
    Field,
    EnumDecl,
    GlobalLet,
    AliasDecl,
};

enum class BuiltinAnnoFieldKind : u8 {
    Bool,
    I32,
    String,
    Type,
    AnnoTypeList,
};

struct BuiltinAnnoField {
    std::string name;
    BuiltinAnnoFieldKind kind;
    bool valueSugar = false; // 名为 value 时可省略字段名
};

struct BuiltinAnnoSpec {
    std::string name;
    bool repeatable;
    std::vector<AnnoAttachSite> on;
    std::vector<BuiltinAnnoField> fields;
};

// `AnnoType` 枚举变体名（SDK 文档 struct 对照用）。
inline const std::set<std::string>& builtinAnnoTypeVariants() {
    static const std::set<std::string> s = {"Code",  "StructDecl", "FnDecl",    "ExternFn",
                                            "Field", "EnumDecl",   "GlobalLet", "AliasDecl"};
    return s;
}

inline const std::vector<BuiltinAnnoSpec>& builtinAnnoSpecs() {
    static const std::vector<BuiltinAnnoSpec> s = {
        {.name="If",
         .repeatable=false,
         .on={AnnoAttachSite::Code, AnnoAttachSite::StructDecl, AnnoAttachSite::FnDecl, AnnoAttachSite::ExternFn,
          AnnoAttachSite::Field, AnnoAttachSite::EnumDecl, AnnoAttachSite::GlobalLet, AnnoAttachSite::AliasDecl},
         .fields={{.name="value", .kind=BuiltinAnnoFieldKind::Bool, .valueSugar=true}}},
        {.name="Impl", .repeatable=true, .on={AnnoAttachSite::StructDecl}, .fields={{.name="value", .kind=BuiltinAnnoFieldKind::Type, .valueSugar=true}}},
        {.name="Align",
         .repeatable=false,
         .on={AnnoAttachSite::StructDecl, AnnoAttachSite::Field},
         .fields={{.name="value", .kind=BuiltinAnnoFieldKind::I32, .valueSugar=true}}},
        {.name="CName", .repeatable=false, .on={AnnoAttachSite::ExternFn}, .fields={{.name="value", .kind=BuiltinAnnoFieldKind::String, .valueSugar=true}}},
        {.name="Anno",
         .repeatable=false,
         .on={},
         .fields={{.name="repeatable", .kind=BuiltinAnnoFieldKind::Bool, .valueSugar=false}, {.name="on", .kind=BuiltinAnnoFieldKind::AnnoTypeList, .valueSugar=false}}},
    };
    return s;
}

inline const BuiltinAnnoSpec* lookupBuiltinAnnoSpec(const std::string& name) {
    for (const auto& spec : builtinAnnoSpecs()) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

inline bool builtinAnnoAllowsSite(const BuiltinAnnoSpec& spec, AnnoAttachSite site) {
    if (spec.on.empty()) return true;
    for (auto s : spec.on) {
        if (s == site) return true;
    }
    return false;
}

#endif // RIU_LANG_BUILTIN_ANNOS_H
