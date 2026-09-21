// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "types.h"

#include "node/file_node.h"
#include "riu.h"

#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void hashCombine(size_t& h, size_t v) {
    h ^= v + 0x9e3779b9u + (h << 6u) + (h >> 2u);
}

struct TypeInternKey {
    TypeKind kind{};
    bool fnNullable = false;
    u64 arraySize = 0;
    const TypeInfo* element = nullptr;
    vector<const TypeInfo*> args;
    string name;
    string ownerModule;
    string fallibleErr;

    bool operator==(const TypeInternKey& o) const {
        return kind == o.kind && fnNullable == o.fnNullable && arraySize == o.arraySize && element == o.element &&
               args == o.args && name == o.name && ownerModule == o.ownerModule && fallibleErr == o.fallibleErr;
    }
};

struct TypeInternKeyHash {
    size_t operator()(const TypeInternKey& k) const {
        size_t h = std::hash<u8>{}(static_cast<u8>(k.kind));
        hashCombine(h, std::hash<bool>{}(k.fnNullable));
        hashCombine(h, std::hash<u64>{}(k.arraySize));
        hashCombine(h, std::hash<const void*>{}(static_cast<const void*>(k.element)));
        hashCombine(h, std::hash<string>{}(k.name));
        hashCombine(h, std::hash<string>{}(k.ownerModule));
        hashCombine(h, std::hash<string>{}(k.fallibleErr));
        for (const TypeInfo* a : k.args) {
            hashCombine(h, std::hash<const void*>{}(static_cast<const void*>(a)));
        }
        return h;
    }
};

TypeInternKey makeKey(const TypeInfo& t) {
    TypeInternKey k;
    k.kind = t.kind;
    k.fnNullable = t.fnNullable;
    k.arraySize = t.arraySize;
    k.element = t.elementType.get();
    k.args.reserve(t.genericArgs.size());
    for (const auto& a : t.genericArgs)
        k.args.push_back(a.get());
    k.name = t.name;
    k.ownerModule = t.ownerModule;
    k.fallibleErr = t.fallibleErr;
    return k;
}

bool isEmptyType(const TypeInfo& t) {
    return t.empty() && t.kind == TypeKind::Normal && !t.elementType && t.genericArgs.empty() && t.arraySize == 0 &&
           !t.fnNullable && t.fallibleErr.empty() && t.ownerModule.empty();
}

const TypeInfo& emptyType() {
    static const TypeInfo kEmpty;
    return kEmpty;
}

sp<TypeInfo> shareStatic(const TypeInfo& t) {
    return {const_cast<TypeInfo*>(&t), [](TypeInfo*) {}};
}

thread_local vector<TypeIntern*> g_interns;

TypeIntern& fallbackIntern() {
    static TypeIntern intern;
    return intern;
}

} // namespace

struct TypeIntern::Impl {
    std::unordered_map<TypeInternKey, sp<TypeInfo>, TypeInternKeyHash> byKey;
};

TypeIntern::TypeIntern() : _impl(std::make_unique<Impl>()) {}

TypeIntern::~TypeIntern() = default;

TypeIntern::TypeIntern(TypeIntern&&) noexcept = default;

TypeIntern& TypeIntern::operator=(TypeIntern&&) noexcept = default;

sp<TypeInfo> TypeIntern::internSp(TypeInfo t) {
    ensurePtrGenericArg(t);
    if (isEmptyType(t)) return shareStatic(emptyType());
    if (const TypeInfo* p = internTypePtr(t)) return shareStatic(*p);

    if (t.elementType) t.elementType = internSp(*t.elementType);
    for (auto& a : t.genericArgs) {
        if (a) a = internSp(*a);
    }

    if (!t.fallibleErr.empty() && !t._withoutFallible) {
        TypeInfo stripped = t;
        typeInfoStripFallible(stripped);
        t._withoutFallible = internSp(std::move(stripped)).get();
    }

    TypeInternKey key = makeKey(t);
    auto it = _impl->byKey.find(key);
    if (it != _impl->byKey.end()) return it->second;

    auto held = std::make_shared<TypeInfo>(std::move(t));
    _impl->byKey.emplace(std::move(key), held);
    return held;
}

const TypeInfo& TypeIntern::intern(TypeInfo t) {
    return *internSp(std::move(t));
}

void bindTypeIntern(TypeIntern* intern) {
    if (intern)
        g_interns.push_back(intern);
    else if (!g_interns.empty())
        g_interns.pop_back();
}

TypeIntern& currentTypeIntern() {
    return g_interns.empty() ? fallbackIntern() : *g_interns.back();
}

const TypeInfo& internType(TypeInfo t) {
    ensurePtrGenericArg(t);
    if (isEmptyType(t)) return emptyType();
    if (const TypeInfo* p = internTypePtr(t)) return *p;
    return currentTypeIntern().intern(std::move(t));
}

sp<TypeInfo> internTypeSp(TypeInfo t) {
    ensurePtrGenericArg(t);
    if (isEmptyType(t)) return shareStatic(emptyType());
    if (const TypeInfo* p = internTypePtr(t)) return shareStatic(*p);
    return currentTypeIntern().internSp(std::move(t));
}

const TypeInfo& internTypeAt(const Node* n, TypeInfo t) {
    ensurePtrGenericArg(t);
    if (isEmptyType(t)) return emptyType();
    if (const TypeInfo* p = internTypePtr(t)) return *p;
    if (n) {
        if (FileNode* f = n->enclosingFile()) {
            if (Riu* r = f->riu()) return r->typeIntern().intern(std::move(t));
        }
    }
    return internType(std::move(t));
}

sp<TypeInfo> internTypeSpAt(const Node* n, TypeInfo t) {
    ensurePtrGenericArg(t);
    if (isEmptyType(t)) return shareStatic(emptyType());
    if (const TypeInfo* p = internTypePtr(t)) return shareStatic(*p);
    if (n) {
        if (FileNode* f = n->enclosingFile()) {
            if (Riu* r = f->riu()) return r->typeIntern().internSp(std::move(t));
        }
    }
    return internTypeSp(std::move(t));
}

namespace {

struct TokenTextHash {
    using is_transparent = void;
    size_t operator()(string_view s) const noexcept { return std::hash<string_view>{}(s); }
    size_t operator()(const string& s) const noexcept { return std::hash<string_view>{}(s); }
};

struct TokenTextEq {
    using is_transparent = void;
    bool operator()(string_view a, string_view b) const noexcept { return a == b; }
    bool operator()(const string& a, string_view b) const noexcept { return a == b; }
    bool operator()(string_view a, const string& b) const noexcept { return a == b; }
    bool operator()(const string& a, const string& b) const noexcept { return a == b; }
};

thread_local vector<StringIntern*> g_stringInterns;

StringIntern& fallbackStringIntern() {
    static StringIntern intern;
    return intern;
}

StringIntern& currentStringIntern() {
    return g_stringInterns.empty() ? fallbackStringIntern() : *g_stringInterns.back();
}

} // namespace

struct StringIntern::Impl {
    // 元素指针在 rehash 后仍有效；透明查找避免 makeTok 热路径再分配。
    std::unordered_set<string, TokenTextHash, TokenTextEq> texts;
};

StringIntern::StringIntern() : _impl(std::make_unique<Impl>()) {}

StringIntern::~StringIntern() = default;

StringIntern::StringIntern(StringIntern&&) noexcept = default;

StringIntern& StringIntern::operator=(StringIntern&&) noexcept = default;

const string& StringIntern::intern(string_view s) {
    if (s.empty()) return emptyTokenText();
    if (auto it = _impl->texts.find(s); it != _impl->texts.end()) return *it;
    return *_impl->texts.emplace(s).first;
}

void StringIntern::reserve(size_t n) {
    _impl->texts.reserve(n);
}

void bindStringIntern(StringIntern* intern) {
    if (intern)
        g_stringInterns.push_back(intern);
    else if (!g_stringInterns.empty())
        g_stringInterns.pop_back();
}

const string& internTokenText(string_view s) {
    if (s.empty()) return emptyTokenText();
    return currentStringIntern().intern(s);
}
