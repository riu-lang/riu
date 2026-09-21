// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/layout.h"

#include "ast/name_lookup.h"
#include "ast/node/enum_node.h"
#include "ast/node/file_node.h"
#include "ast/node/struct_node.h"

#include <bit>
#include <cctype>
#include <map>
#include <set>

namespace layout {
namespace {

uint64_t scalarSizeAlign(std::string_view name, uint64_t& align) {
    if (name == "i8" || name == "u8" || name == "bool") {
        align = 1;
        return 1;
    }
    if (name == "i16" || name == "u16") {
        align = 2;
        return 2;
    }
    if (name == "i32" || name == "u32" || name == "f32") {
        align = 4;
        return 4;
    }
    if (name == "i64" || name == "u64" || name == "f64" || name == "isize" || name == "usize") {
        align = 8;
        return 8;
    }
    return 0;
}

} // namespace

uint64_t parseAlignArg(std::string_view text) {
    if (text.empty()) return 0;
    size_t i = 0;
    if (text[i] == '+') ++i;
    if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) return 0;
    uint64_t n = 0;
    for (; i < text.size(); ++i) {
        auto c = static_cast<unsigned char>(text[i]);
        if (!std::isdigit(c)) break;
        auto d = static_cast<uint64_t>(c - '0');
        if (n > (UINT64_MAX - d) / 10) return 0;
        n = n * 10 + d;
    }
    if (i != text.size()) {
        // 允许 `8i32` / `8u64` 一类字面量后缀
        while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i])))
            ++i;
        if (i != text.size()) return 0;
    }
    if (n == 0 || n > (uint64_t{1} << 28u) || !std::has_single_bit(n)) return 0;
    return n;
}

bool isPacked(const StructDeclNode* sd) {
    return sd && sd->hasAnno("Packed");
}

uint64_t structAlignN(const StructDeclNode* sd) {
    if (!sd) return 0;
    auto arg = sd->getAnnoArg("Align");
    if (!arg) return 0;
    return parseAlignArg(*arg);
}

bool hasCustomLayout(const StructDeclNode* sd) {
    if (!sd) return false;
    if (isPacked(sd) || structAlignN(sd) != 0) return true;
    for (auto* f : sd->fields()) {
        if (f && f->alignN() != 0) return true;
    }
    return false;
}

namespace {

std::optional<AbiLayout> layoutRec(TypeInfo t, FileNode* file, FileNode* sdkFile, std::set<string>& visiting);

AbiLayout structLayout(const TypeInfo& structTy, StructDeclNode* sd, FileNode* file, FileNode* sdkFile,
                       std::set<string>& visiting) {
    std::map<string, TypeInfo> subst;
    if (sd->isGeneric() && structTy.genericArgs.size() == sd->typeParams().size()) {
        for (size_t i = 0; i < sd->typeParams().size(); ++i) {
            if (structTy.genericArgs[i]) subst[sd->typeParams()[i]] = *structTy.genericArgs[i];
        }
    }

    const bool packed = isPacked(sd);
    const uint64_t reqAlign = structAlignN(sd);
    uint64_t offset = 0;
    uint64_t maxAlign = 1;
    AbiLayout out;
    out.fieldOffsets.resize(sd->fields().size(), 0);

    for (size_t i = 0; i < sd->fields().size(); ++i) {
        auto* f = sd->fields()[i];
        TypeInfo ft = f->getType();
        if (!subst.empty()) ft = ft.substitute(subst);
        auto fl = layoutRec(ft, file, sdkFile, visiting);
        if (!fl) {
            out.size = 0;
            out.align = 0;
            return out;
        }
        uint64_t fAlign = fl->align;
        if (f->alignN() != 0) {
            fAlign = f->alignN() > fAlign ? f->alignN() : fAlign;
        }
        if (packed) {
            if (f->alignN() != 0) offset = alignUp(offset, fAlign);
        } else {
            offset = alignUp(offset, fAlign);
            if (fAlign > maxAlign) maxAlign = fAlign;
        }
        out.fieldOffsets[i] = offset;
        offset += fl->size;
    }

    uint64_t typeAlign = packed ? 1 : maxAlign;
    if (reqAlign != 0) {
        typeAlign = packed ? reqAlign : (reqAlign > typeAlign ? reqAlign : typeAlign);
    }
    if (typeAlign == 0) typeAlign = 1;
    uint64_t size = offset;
    if (!packed || reqAlign != 0) {
        size = alignUp(offset, typeAlign);
    }
    out.size = size;
    out.align = typeAlign;
    return out;
}

std::optional<AbiLayout> layoutRec(TypeInfo t, FileNode* file, FileNode* sdkFile, std::set<string>& visiting) {
    try {
        t = sema::resolveAlias(t, file, sdkFile);
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        return std::nullopt;
    }
    t = t.withoutFallible();
    if (t.empty()) {
        AbiLayout z;
        z.size = 0;
        z.align = 1;
        return z;
    }

    auto sized = [](uint64_t size, uint64_t align) {
        return AbiLayout{.size = size, .align = align, .fieldOffsets = {}};
    };

    uint64_t align = 1;
    if (uint64_t sz = scalarSizeAlign(t.name, align)) {
        return sized(sz, align);
    }
    if (t.isPtr() || t.isRef() || t.isHeap() || t.name == "Ptr") {
        return sized(8, 8);
    }
    if (t.isRc() || t.isWeak()) {
        return sized(8, 8);
    }
    if (t.isArrayGeneric()) {
        return sized(24, 8);
    }
    if (t.isDyn() || t.isFn()) {
        return sized(16, 8);
    }
    if (t.isNullable()) {
        auto inner = t.nullableInnerType();
        if (!inner) return std::nullopt;
        auto il = layoutRec(*inner, file, sdkFile, visiting);
        if (!il) return std::nullopt;
        // LLVM { i1, T }：i1 宽 1、对齐 1，随后按 T 对齐
        uint64_t off = alignUp(1, il->align);
        uint64_t al = il->align > 1 ? il->align : 1;
        return sized(alignUp(off + il->size, al), al);
    }
    if (t.isArray()) {
        if (!t.elementType) return std::nullopt;
        auto el = layoutRec(*t.elementType, file, sdkFile, visiting);
        if (!el) return std::nullopt;
        return sized(el->size * t.arraySize, el->align);
    }
    if (t.isTuple()) {
        uint64_t off = 0;
        uint64_t al = 1;
        for (auto& e : t.tupleElements()) {
            if (!e) return std::nullopt;
            auto el = layoutRec(*e, file, sdkFile, visiting);
            if (!el) return std::nullopt;
            off = alignUp(off, el->align);
            off += el->size;
            if (el->align > al) al = el->align;
        }
        return sized(alignUp(off, al), al);
    }

    const string key = t.identityKey();
    if (visiting.contains(key)) return std::nullopt;
    visiting.insert(key);

    sema::NameResolver nr(file, sdkFile);
    if (auto* sd = nr.lookupStruct(t)) {
        auto lay = structLayout(t, sd, file, sdkFile, visiting);
        visiting.erase(key);
        if (lay.align == 0) return std::nullopt;
        return lay;
    }
    if (auto* ed = nr.lookupEnum(t)) {
        uint64_t maxPayload = 0;
        for (auto* v : ed->variants()) {
            if (!v || !v->hasPayload()) continue;
            uint64_t poff = 0;
            uint64_t pal = 1;
            std::map<string, TypeInfo> subst = sema::enumInstSubst(ed, t);
            for (auto* pt : v->payloadTypes()) {
                TypeInfo py = pt->getType();
                if (!subst.empty()) py = py.substitute(subst);
                auto pl = layoutRec(py, file, sdkFile, visiting);
                if (!pl) {
                    visiting.erase(key);
                    return std::nullopt;
                }
                poff = alignUp(poff, pl->align);
                poff += pl->size;
                if (pl->align > pal) pal = pl->align;
            }
            uint64_t psz = alignUp(poff, pal);
            if (psz > maxPayload) maxPayload = psz;
        }
        visiting.erase(key);
        return AbiLayout{.size = alignUp(4 + maxPayload, 4), .align = 4, .fieldOffsets = {}};
    }
    visiting.erase(key);
    return std::nullopt;
}

bool cLayoutRec(TypeInfo t, FileNode* file, FileNode* sdkFile, std::set<string>& visiting) {
    try {
        t = sema::resolveAlias(t, file, sdkFile);
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        return false;
    }
    t = t.withoutFallible();
    if (t.empty()) return true;
    if (t.isPtr()) return true;
    if (t.isNormal() && isBuiltinType(t.name) && t.name != "bool") return true;
    if (t.isArray()) {
        return t.elementType && cLayoutRec(*t.elementType, file, sdkFile, visiting);
    }
    if (t.isHeap() || t.isFn() || t.isDyn() || t.isRc() || t.isWeak() || t.isNullable() || t.isArrayGeneric() ||
        t.isRef() || t.isTuple() || t.isString() || t.isStringBuilder() || t.isFallible() ||
        (t.isNormal() && t.name == "bool")) {
        return false;
    }
    const string key = t.identityKey();
    if (visiting.contains(key)) return false;
    visiting.insert(key);
    sema::NameResolver nr(file, sdkFile);
    if (nr.lookupEnum(t)) {
        visiting.erase(key);
        return false;
    }
    auto* sd = nr.lookupStruct(t);
    if (!sd) {
        visiting.erase(key);
        return false;
    }
    std::map<string, TypeInfo> subst;
    if (sd->isGeneric()) {
        if (t.genericArgs.size() != sd->typeParams().size()) {
            visiting.erase(key);
            return false;
        }
        for (size_t i = 0; i < sd->typeParams().size(); ++i) {
            if (t.genericArgs[i]) subst[sd->typeParams()[i]] = *t.genericArgs[i];
        }
    }
    for (auto* f : sd->fields()) {
        if (!f) continue;
        TypeInfo ft = f->getType();
        if (!subst.empty()) ft = ft.substitute(subst);
        if (!cLayoutRec(ft, file, sdkFile, visiting)) {
            visiting.erase(key);
            return false;
        }
    }
    visiting.erase(key);
    return true;
}

} // namespace

std::optional<AbiLayout> tryAbiLayout(const TypeInfo& t, FileNode* file, FileNode* sdkFile) {
    std::set<string> visiting;
    return layoutRec(t, file, sdkFile, visiting);
}

bool isCLayoutType(const TypeInfo& t, FileNode* file, FileNode* sdkFile) {
    std::set<string> visiting;
    return cLayoutRec(t, file, sdkFile, visiting);
}

} // namespace layout
