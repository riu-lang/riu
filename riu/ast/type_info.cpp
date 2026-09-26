// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#include "type_info.h"

#include <string_view>
#include <utility>

TypeInfo::TypeInfo(FnTag, vector<sp<TypeInfo>> paramTypes, sp<TypeInfo> retType, bool nullable)
    : kind(TypeKind::Fn), elementType(std::move(retType)), genericArgs(std::move(paramTypes)), fnNullable(nullable) {
    if (elementType && elementType->kind == TypeKind::Tuple && elementType->genericArgs.empty()) {
        elementType = nullptr;
    }
    rebuildFnName();
}

void TypeInfo::rebuildFnName() {
    name = "Function<";
    for (size_t i = 0; i < genericArgs.size(); ++i) {
        if (i > 0) name += ',';
        name += genericArgs[i] ? genericArgs[i]->getFullName() : "?";
    }
    if (!genericArgs.empty()) name += ',';
    name += elementType ? elementType->getFullName() : "()";
    name += '>';
    if (fnNullable) name += '?';
}

TypeInfo::TypeInfo(string n, vector<sp<TypeInfo>> args) {
    // Function<P..., Ret>：末位是返回类型，其余是形参
    if (n == "Function") {
        kind = TypeKind::Fn;
        if (!args.empty()) {
            elementType = args.back();
            genericArgs.assign(args.begin(), args.end() - 1);
            if (elementType && elementType->kind == TypeKind::Tuple && elementType->genericArgs.empty()) {
                elementType = nullptr;
            }
        }
        rebuildFnName();
        return;
    }
    // Function<...>? 走标准 `?` 后缀，AST 会包一层 Nullable；折叠回 fat-ptr 可空
    if (n == "Nullable" && args.size() == 1 && args[0] && args[0]->isFn() && !args[0]->fnNullable) {
        *this = *args[0];
        fnNullable = true;
        rebuildFnName();
        return;
    }
    kind = kindForBuiltinWrapper(n);
    name = std::move(n);
    genericArgs = std::move(args);
    if (kind == TypeKind::Ptr && genericArgs.empty()) {
        genericArgs.push_back(std::make_shared<TypeInfo>(TupleTag{}, vector<sp<TypeInfo>>{}));
    }
}

TypeInfo::TypeInfo(sp<TypeInfo> elemType, u64 size)
    : kind(TypeKind::Array), arraySize(size), elementType(std::move(elemType)) {
    name = "[" + elementType->name + " * " + to_string(arraySize) + "]";
}

TypeInfo::TypeInfo(TupleTag, vector<sp<TypeInfo>> elements) : kind(TypeKind::Tuple), genericArgs(std::move(elements)) {
    name = "(";
    for (size_t i = 0; i < genericArgs.size(); ++i) {
        if (i > 0) name += ',';
        name += genericArgs[i] ? genericArgs[i]->name : "?";
    }
    name += ')';
}

bool TypeInfo::hasGenericArgs() const {
    switch (kind) {
    case TypeKind::Generic:
    case TypeKind::Rc:
    case TypeKind::Ref:
    case TypeKind::Weak:
    case TypeKind::Heap:
    case TypeKind::Dyn:
    case TypeKind::ArrayGeneric:
    case TypeKind::Nullable:
    case TypeKind::Ptr:
        return true;
    default:
        return false;
    }
}

string TypeInfo::getFullName() const {
    if (hasGenericArgs() && !genericArgs.empty()) {
        string result = name + "<";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) result += ',';
            result += genericArgs[i]->getFullName();
        }
        result += '>';
        return result;
    }
    if (kind == TypeKind::Array && elementType) {
        return "[" + elementType->getFullName() + "*" + std::to_string(arraySize) + "]";
    }
    // 元组：(T1,T2)
    if (kind == TypeKind::Tuple) {
        string result = "(";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) result += ',';
            result += genericArgs[i] ? genericArgs[i]->getFullName() : string("?");
        }
        result += ')';
        return result;
    }
    // 函数：Function<P...,Ret> / Function<...>?
    if (kind == TypeKind::Fn) {
        return formatFnGeneric(false);
    }
    if (!fallibleErr.empty()) {
        const string suffix = "!" + fallibleErr;
        if (!name.ends_with(suffix)) return name + suffix;
    }
    return name;
}

string TypeInfo::getMangleName() const {
    auto head = [this]() -> string {
        string n = withoutFallible().name;
        if (kind == TypeKind::Array || kind == TypeKind::Tuple || kind == TypeKind::Fn) return n;
        if (ownerModule.empty()) return n;
        return ownerModule + "." + n;
    };
    if (hasGenericArgs() && !genericArgs.empty()) {
        string result = head() + "<";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) result += ',';
            result += genericArgs[i] ? genericArgs[i]->getMangleName() : string("?");
        }
        result += '>';
        if (!fallibleErr.empty()) result += "!" + fallibleErr;
        return result;
    }
    if (kind == TypeKind::Array && elementType) {
        return "[" + elementType->getMangleName() + "*" + std::to_string(arraySize) + "]";
    }
    // 元组：(T1,T2)
    if (kind == TypeKind::Tuple) {
        string result = "(";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) result += ',';
            result += genericArgs[i] ? genericArgs[i]->getMangleName() : string("?");
        }
        result += ')';
        return result;
    }
    // 函数：Function<P...,Ret> / Function<...>?
    if (kind == TypeKind::Fn) {
        return formatFnGeneric(true);
    }
    if (!fallibleErr.empty()) return head() + "!" + fallibleErr;
    return head();
}

string TypeInfo::baseStructName() const {
    string n = name;
    auto cut = n.find_first_of("$<");
    if (cut != string::npos) {
        n.resize(cut);
    }
    return n;
}

string TypeInfo::formatFnGeneric(bool mangle) const {
    string result = "Function<";
    for (size_t i = 0; i < genericArgs.size(); ++i) {
        if (i > 0) result += ',';
        if (!genericArgs[i]) {
            result += '?';
        } else {
            result += mangle ? genericArgs[i]->getMangleName() : genericArgs[i]->getFullName();
        }
    }
    if (!genericArgs.empty()) result += ',';
    if (!elementType) {
        result += "()";
    } else {
        result += mangle ? elementType->getMangleName() : elementType->getFullName();
    }
    result += '>';
    if (fnNullable) result += '?';
    return result;
}

TypeInfo TypeInfo::substitute(const std::map<std::string, TypeInfo>& subst) const {
    auto withErr = [&](TypeInfo r) -> TypeInfo {
        if (fallibleErr.empty()) return r;
        TypeInfo err = TypeInfo::fromFullName(fallibleErr).substitute(subst);
        r.attachFallibleErr(err.getFullName());
        return r;
    };
    if (kind == TypeKind::Normal || (kind == TypeKind::Generic && genericArgs.empty())) {
        auto it = subst.find(name);
        if (it != subst.end()) return withErr(it->second);
        if (kind != TypeKind::Generic) return withErr(*this);
    }
    if (hasGenericArgs() && !genericArgs.empty()) {
        vector<sp<TypeInfo>> newArgs;
        newArgs.reserve(genericArgs.size());
        for (auto& a : genericArgs) {
            newArgs.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
        }
        TypeInfo r{name, std::move(newArgs)};
        r.ownerModule = ownerModule;
        return withErr(std::move(r));
    }
    if (kind == TypeKind::Array && elementType) {
        auto sub = elementType->substitute(subst);
        return withErr({std::make_shared<TypeInfo>(std::move(sub)), arraySize});
    }
    if (kind == TypeKind::Tuple) {
        vector<sp<TypeInfo>> newElems;
        newElems.reserve(genericArgs.size());
        for (auto& a : genericArgs) {
            newElems.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
        }
        return withErr(TypeInfo(TupleTag{}, std::move(newElems)));
    }
    if (kind == TypeKind::Fn) {
        vector<sp<TypeInfo>> newParams;
        newParams.reserve(genericArgs.size());
        for (auto& a : genericArgs) {
            newParams.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
        }
        sp<TypeInfo> newRet = nullptr;
        if (elementType) newRet = std::make_shared<TypeInfo>(elementType->substitute(subst));
        return withErr(TypeInfo(FnTag{}, std::move(newParams), newRet, fnNullable));
    }
    return withErr(*this);
}

bool TypeInfo::operator==(const TypeInfo& other) const {
    if (kind != other.kind) return false;
    if (fallibleErr != other.fallibleErr) return false;
    if (kind == TypeKind::Array) {
        if (arraySize != other.arraySize) return false;
        if (!elementType && !other.elementType) return true;
        if (!elementType || !other.elementType) return false;
        return *elementType == *other.elementType;
    }
    // 裸 Ptr 与 Ptr<()> 同型（旧 .ud / 未 intern 的空 genericArgs）
    if (kind == TypeKind::Ptr) {
        auto payload = [](const TypeInfo& t) -> const TypeInfo* {
            return t.genericArgs.size() == 1 ? t.genericArgs[0].get() : nullptr;
        };
        const TypeInfo* a = payload(*this);
        const TypeInfo* b = payload(other);
        const bool aUnit = !a || a->isUnit();
        const bool bUnit = !b || b->isUnit();
        if (aUnit && bUnit) {
            return name == other.name && sameOwner(other);
        }
        if (!a || !b) return false;
        if (*a != *b) return false;
        return name == other.name && sameOwner(other);
    }
    if (hasGenericArgs() || kind == TypeKind::Tuple) {
        if (genericArgs.size() != other.genericArgs.size()) return false;
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (!genericArgs[i] && !other.genericArgs[i]) continue;
            if (!genericArgs[i] || !other.genericArgs[i]) return false;
            if (*genericArgs[i] != *other.genericArgs[i]) return false;
        }
        if (kind == TypeKind::Generic) {
            if (name != other.name) return false;
            if (!sameOwner(other)) return false;
        }
    }
    if (kind == TypeKind::Fn) {
        if (fnNullable != other.fnNullable) return false;
        if (genericArgs.size() != other.genericArgs.size()) return false;
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (!genericArgs[i] && !other.genericArgs[i]) continue;
            if (!genericArgs[i] || !other.genericArgs[i]) return false;
            if (*genericArgs[i] != *other.genericArgs[i]) return false;
        }
        // 返回类型：unit（nullptr）也参与判等
        if (!elementType && !other.elementType) return true;
        if (!elementType || !other.elementType) return false;
        return *elementType == *other.elementType;
    }
    if (kind == TypeKind::Normal || kind == TypeKind::Ptr) {
        // fallibleErr 已相等。热路径两边都是 `i32`：直接比 name，不要
        // withoutFallible() 整份拷 TypeInfo（含 vector/shared_ptr）。
        if (name != other.name) {
            auto strip = [](const TypeInfo& t) -> string_view {
                if (t.fallibleErr.empty()) return t.name;
                const size_t n = t.fallibleErr.size() + 1;
                if (t.name.size() >= n && t.name[t.name.size() - n] == '!' &&
                    string_view(t.name).substr(t.name.size() - t.fallibleErr.size()) == t.fallibleErr) {
                    return string_view(t.name).substr(0, t.name.size() - n);
                }
                return t.name;
            };
            if (strip(*this) != strip(other)) return false;
        }
        if (!sameOwner(other)) return false;
    } else if (!hasGenericArgs() && kind != TypeKind::Tuple) {
        if (name != other.name) return false;
        if (!sameOwner(other)) return false;
    }
    return true;
}

TypeInfo TypeInfo::fromFullName(const string& s) {
    if (s.empty()) return {};
    struct Parser {
        const string& s;
        size_t i = 0;
        TypeInfo parse() {
            const size_t start = i;
            while (i < s.size()) {
                const char c = s[i];
                if (c == '<' || c == '>' || c == ',' || c == '&' || c == '?') break;
                ++i;
            }
            string n = s.substr(start, i - start);
            TypeInfo t;
            if (i < s.size() && s[i] == '<') {
                ++i;
                vector<sp<TypeInfo>> args;
                if (i < s.size() && s[i] != '>') {
                    while (true) {
                        args.push_back(std::make_shared<TypeInfo>(parse()));
                        if (i < s.size() && s[i] == ',') {
                            ++i;
                            continue;
                        }
                        break;
                    }
                }
                if (i < s.size() && s[i] == '>') ++i;
                if (n.empty()) return {};
                t = TypeInfo{std::move(n), std::move(args)};
            } else if (n.empty()) {
                return {};
            } else if (n == "()") {
                t = TypeInfo(TupleTag{}, vector<sp<TypeInfo>>{});
            } else {
                t = TypeInfo(std::move(n));
            }
            while (i < s.size()) {
                if (s[i] == '&') {
                    ++i;
                    t = TypeInfo("Ref", {std::make_shared<TypeInfo>(t)});
                } else if (s[i] == '?') {
                    ++i;
                    t = TypeInfo("Nullable", {std::make_shared<TypeInfo>(t)});
                } else {
                    break;
                }
            }
            return t;
        }
    };
    Parser p{.s = s};
    TypeInfo t = p.parse();
    if (p.i != s.size() && t.empty()) return TypeInfo(s);
    return t;
}

const TypeInfo& internNamedType(string_view name) {
    switch (name.size()) {
    case 2:
        if (name == "i8") {
            static const TypeInfo t{"i8"};
            return t;
        }
        if (name == "u8") {
            static const TypeInfo t{"u8"};
            return t;
        }
        if (name == "Rc") {
            static const TypeInfo t{"Rc"};
            return t;
        }
        break;
    case 3:
        if (name == "i16") {
            static const TypeInfo t{"i16"};
            return t;
        }
        if (name == "u16") {
            static const TypeInfo t{"u16"};
            return t;
        }
        if (name == "i32") {
            static const TypeInfo t{"i32"};
            return t;
        }
        if (name == "u32") {
            static const TypeInfo t{"u32"};
            return t;
        }
        if (name == "i64") {
            static const TypeInfo t{"i64"};
            return t;
        }
        if (name == "u64") {
            static const TypeInfo t{"u64"};
            return t;
        }
        if (name == "f32") {
            static const TypeInfo t{"f32"};
            return t;
        }
        if (name == "f64") {
            static const TypeInfo t{"f64"};
            return t;
        }
        if (name == "Ptr") {
            static const TypeInfo t{"Ptr"};
            return t;
        }
        if (name == "Dyn") {
            static const TypeInfo t{"Dyn"};
            return t;
        }
        if (name == "Ref") {
            static const TypeInfo t{"Ref"};
            return t;
        }
        break;
    case 4:
        if (name == "bool") {
            static const TypeInfo t{"bool"};
            return t;
        }
        if (name == "Self") {
            static const TypeInfo t{"Self"};
            return t;
        }
        if (name == "Heap") {
            static const TypeInfo t{"Heap"};
            return t;
        }
        if (name == "Weak") {
            static const TypeInfo t{"Weak"};
            return t;
        }
        break;
    case 5:
        if (name == "isize") {
            static const TypeInfo t{"isize"};
            return t;
        }
        if (name == "usize") {
            static const TypeInfo t{"usize"};
            return t;
        }
        if (name == "Array") {
            static const TypeInfo t{"Array"};
            return t;
        }
        break;
    case 8:
        if (name == "Function") {
            static const TypeInfo t{"Function"};
            return t;
        }
        if (name == "Nullable") {
            static const TypeInfo t{"Nullable"};
            return t;
        }
        break;
    default:
        break;
    }
    static const TypeInfo kEmpty;
    return kEmpty;
}

inline const TypeInfo& TypeInfo::withoutFallible() const {
    if (fallibleErr.empty()) return *this;
    if (_withoutFallible) return *_withoutFallible;
    TypeInfo t = *this;
    typeInfoStripFallible(t);
    // 不把 TLS intern 指针写回 *this：SDK 节点上的 interned 对象可能活过用户 Riu。
    return internType(std::move(t));
}

void TypeInfo::attachFallibleErr(string err) {
    fallibleErr = std::move(err);
    if (fallibleErr.empty()) return;
    const string base = withoutFallible().getFullName();
    name = base + "!" + fallibleErr;
}
