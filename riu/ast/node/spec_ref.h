// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// `#Impl(D<A>)` / `<T : D<A>>` 里对 spec 的引用：基名 + 类型实参。
// 从 spec_node.h 拆出，避免与 fn_node.h 循环包含。

#ifndef RIU_LANG_SPEC_REF_H
#define RIU_LANG_SPEC_REF_H

#include "types.h"

#include <map>
#include <string>
#include <vector>

struct SpecRef {
    std::string name;
    std::vector<TypeInfo> typeArgs;
    int line = 0;
    int col = 0;
};

inline std::vector<TypeInfo> substSpecTypeArgs(const std::vector<TypeInfo>& args,
                                               const std::map<std::string, TypeInfo>& subst) {
    std::vector<TypeInfo> out;
    out.reserve(args.size());
    for (auto& a : args)
        out.push_back(a.substitute(subst));
    return out;
}

inline bool specTypeArgsEqual(const std::vector<TypeInfo>& a, const std::vector<TypeInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!(a[i] == b[i])) return false;
    }
    return true;
}

inline std::string formatSpecBound(const std::string& qualified, const std::vector<TypeInfo>& args) {
    if (args.empty()) return qualified;
    std::string s = qualified;
    s += '<';
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s += ',';
        s += args[i].getFullName();
    }
    s += '>';
    return s;
}

#endif // RIU_LANG_SPEC_REF_H
