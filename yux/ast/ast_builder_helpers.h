// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 拆分文件共享的注解 / 注解校验辅助。
//
// 来源：原 ast_builder.cpp 顶部匿名命名空间块（P1 Phase 2 拆分前）。
// 每个 .cpp 单独包含本头会各得到一份 anonymous-namespace 副本，
// 等价于原来「同一个 .cpp 内 file-local static」，不破坏链接。
//
// 内容：
//   - knownAnnos / argAnnos / nonFnAllowedAnnos / externFnAllowedAnnos —— 注解白名单
//   - AnnoList 容器 + checkAnnoArity / collectAnnos* —— 注解收集 / 形态校验
//   - LetAnnoFlags + readLetAnnos —— let 注解三档（Mut/Frozen/Cval）
//   - checkNoReturnHeader —— #NoReturn 头部级校验（E7012 / E7013）

#ifndef YUX_LANG_AST_BUILDER_HELPERS_H
#define YUX_LANG_AST_BUILDER_HELPERS_H

#include "types.h"
#include "node/fn_node.h"
#include "yux/yuxParser.h"

namespace {

// 已知的构建注解名字白名单；未知注解在 AST 构建期报错
// NoReturn / Fallible 由 DRAFT-错误.md 引入（spec §11.5.1）：
//   #NoReturn        零参；标在 fn / structImpl 内方法上
//   #Fallible(E)     单参；E 为错误 enum 类型名（语义校验推 10e）
// spec-unify v1（[#1.AD]）新增：
//   #Spec            零参；标在 struct 上 — 把声明转为 spec（仅签名）
//   #Impl(SpecName)  单参；标在 struct 上 — 实现关系，替代旧 `: D1 + D2` 头部槽
inline const set<string>& knownAnnos() {
    static const set<string> s = {"Builtin", "Test", "DraftLike", "NoReturn", "Fallible", "Const",
                                  "Static",  "Spec", "Impl",      "Reflect",  "NoCopy",   "CName"};
    return s;
}

// 单参注解白名单（spec §11.1.1.1）。其它注解出现 (arg) 形式视为非法（E2005 形式错配）。
inline const set<string>& argAnnos() {
    static const set<string> s = {"Fallible", "Impl", "CName"};
    return s;
}

// 注解可附着位置的限定集合
// fn 之外的位置（structDecl / extern / globalConst）只接受 #Builtin / #Spec / #Impl，
// 不接受 #Test（spec §11.3.1.2）
inline const set<string>& nonFnAllowedAnnos() {
    static const set<string> s = {"Builtin", "Spec", "Impl", "Reflect", "NoCopy"};
    return s;
}

// 注解名 + 单参槽位（与 _annoArgs 对齐）。无参注解的 args[i] 为空字符串。
struct AnnoList {
    vector<string> names;
    vector<string> args;
};

// 从 BuildAnnoContext 取注解参数文本（§11.1.1.1 扩展：ID / 数字 / 字符串 / type）
template <typename A>
inline string getBuildAnnoArgText(A* ctx) {
    if (auto* aa = ctx->annoArg()) {
        if (aa->arg) {
            string t = aa->arg->getText();
            if (auto* gd = aa->genericDef()) t += gd->getText();
            return t;
        }
        if (aa->argNum) return aa->argNum->getText(); // INT / FLOAT
        if (aa->argStr) return aa->argStr->getText(); // STR_LINE_RAW (r"...")
        if (aa->argTPL) {                             // "text"
            string t;
            for (auto* tn : aa->argText)
                t += tn->getText();
            return t;
        }
        if (aa->argType) return aa->argType->getText(); // type
    }
    return "";
}

// 校验单参 / 零参形态：argAnnos() 中的注解必须带括号参数，否则缺参；其他注解出现括号参数视为多余。
// hasArg 基于括号是否存在（ParStart != nullptr），空字符串 "" 也是合法参数值。
template <typename A>
static void checkAnnoArity(A* a, const string& name, bool hasArg) {
    bool needArg = argAnnos().contains(name);
    if (needArg && !hasArg) {
        throw YuxError(static_cast<int>(a->name->getLine()), static_cast<int>(a->name->getCharPositionInLine()) + 1,
                       ErrorCode::E2005, name);
    }
    if (!needArg && hasArg) {
        throw YuxError(static_cast<int>(a->name->getLine()), static_cast<int>(a->name->getCharPositionInLine()) + 1,
                       ErrorCode::E2005, name);
    }
}

template <typename AnnoVec>
AnnoList collectAnnos(const AnnoVec& annos) {
    AnnoList out;
    for (auto* a : annos) {
        string name = a->name->getText();
        if (!knownAnnos().contains(name)) {
            throw YuxError(static_cast<int>(a->name->getLine()), static_cast<int>(a->name->getCharPositionInLine()) + 1,
                           ErrorCode::E2005, name);
        }
        // §12.4.1.1：DraftLike 只能标在 draft 声明；其它位置（fn / struct / impl / extern / global）报 E1110
        if (name == "DraftLike") {
            throw YuxError(static_cast<int>(a->name->getLine()), static_cast<int>(a->name->getCharPositionInLine()) + 1,
                           ErrorCode::E1110);
        }
        string arg = getBuildAnnoArgText(a);
        checkAnnoArity(a, name, a->ParStart() != nullptr);
        out.names.push_back(std::move(name));
        out.args.push_back(std::move(arg));
    }
    return out;
}

// 仅 visitSpecDecl 使用：白名单同 collectAnnos，但保留 DraftLike
template <typename AnnoVec>
AnnoList collectAnnosForSpec(const AnnoVec& annos) {
    AnnoList out;
    for (auto* a : annos) {
        string name = a->name->getText();
        if (!knownAnnos().contains(name)) {
            throw YuxError(static_cast<int>(a->name->getLine()), static_cast<int>(a->name->getCharPositionInLine()) + 1,
                           ErrorCode::E2005, name);
        }
        // draft 声明上 #Test 不合法（§11.3.1.2）
        if (name == "Test") {
            throw YuxError(static_cast<int>(a->name->getLine()), static_cast<int>(a->name->getCharPositionInLine()) + 1,
                           ErrorCode::E2011, name);
        }
        string arg = getBuildAnnoArgText(a);
        checkAnnoArity(a, name, a->ParStart() != nullptr);
        out.names.push_back(std::move(name));
        out.args.push_back(std::move(arg));
    }
    return out;
}

// 用于非 fn 位置（struct / extern / globalConst）：进一步收紧到 fn-only 注解清单
template <typename AnnoVec>
AnnoList collectAnnosNonFn(const AnnoVec& annos) {
    AnnoList out = collectAnnos(annos);
    for (size_t i = 0; i < out.names.size(); ++i) {
        if (!nonFnAllowedAnnos().contains(out.names[i])) {
            // 取对应的 token 用于行列号
            auto* a = annos[i];
            throw YuxError(static_cast<int>(a->name->getLine()), static_cast<int>(a->name->getCharPositionInLine()) + 1,
                           ErrorCode::E2011, out.names[i]);
        }
    }
    return out;
}

// 用于 extern 块内 fnHeader：允许 `Builtin` 与 `#NoReturn`（DRAFT-错误.md §8.3）。
// `#Fallible` 在 extern 上仍被推迟（[#7]），不在白名单。
inline const set<string>& externFnAllowedAnnos() {
    static const set<string> s = {"Builtin", "NoReturn", "CName"};
    return s;
}

template <typename AnnoVec>
AnnoList collectAnnosExternFn(const AnnoVec& annos) {
    AnnoList out = collectAnnos(annos);
    for (size_t i = 0; i < out.names.size(); ++i) {
        if (!externFnAllowedAnnos().contains(out.names[i])) {
            auto* a = annos[i];
            throw YuxError(static_cast<int>(a->name->getLine()), static_cast<int>(a->name->getCharPositionInLine()) + 1,
                           ErrorCode::E2011, out.names[i]);
        }
    }
    return out;
}

// Phase 10d-1：`#NoReturn` 头部级语义校验（E7012 / E7013）
// 不依赖 fn body，仅看 header 注解 + retType。E7014（流终止）与调用点流终止注册推 10d-2。
//   E7012 — `#NoReturn` 函数声明带返回类型
//   E7013 — `#NoReturn` 与 `#Fallible(E)` 互斥
static void checkNoReturnHeader(p<FnHeaderNode> header) {
    if (!header->hasAnno("NoReturn")) return;
    int line = header->getLineNumber();
    int col = header->getColumn();
    if (header->retType()) {
        throw YuxError(line, col, ErrorCode::E7012, header->name().getText());
    }
    if (header->hasAnno("Fallible")) {
        // 取 #Fallible 的单参 E（若解析得到则填，否则空字符串）
        auto eOpt = header->getAnnoArg("Fallible");
        string e = eOpt.value_or("");
        throw YuxError(line, col, ErrorCode::E7013, e);
    }
}

// DRAFT-let-unify §3.4：`let` 注解只允许 #Mut / #Frozen / #Cval / #Inline，互斥；其他报 E3112。
// #Inline 仅与 #Cval 组合使用（相当于 C #define），不与 #Mut / #Frozen 共存。
struct LetAnnoFlags {
    bool isMut = false;
    bool isFrozen = false;
    bool isCval = false;
    bool isInline = false;
};

template <typename AnnoVec>
static LetAnnoFlags readLetAnnos(const AnnoVec& annos) {
    LetAnnoFlags r;
    for (auto* a : annos) {
        const string name = a->name->getText();
        auto* tk = a->name;
        int line = static_cast<int>(tk->getLine());
        int col = static_cast<int>(tk->getCharPositionInLine()) + 1;
        if (name == "Mut") {
            if (r.isFrozen) throw YuxError(line, col, ErrorCode::E3115, "Frozen", "Mut");
            if (r.isCval) throw YuxError(line, col, ErrorCode::E3115, "Cval", "Mut");
            if (r.isInline) throw YuxError(line, col, ErrorCode::E3115, "Inline", "Mut");
            r.isMut = true;
        } else if (name == "Frozen") {
            if (r.isMut) throw YuxError(line, col, ErrorCode::E3115, "Mut", "Frozen");
            if (r.isCval) throw YuxError(line, col, ErrorCode::E3115, "Cval", "Frozen");
            if (r.isInline) throw YuxError(line, col, ErrorCode::E3115, "Inline", "Frozen");
            r.isFrozen = true;
        } else if (name == "Cval") {
            if (r.isMut) throw YuxError(line, col, ErrorCode::E3115, "Mut", "Cval");
            if (r.isFrozen) throw YuxError(line, col, ErrorCode::E3115, "Frozen", "Cval");
            r.isCval = true;
        } else if (name == "Inline") {
            if (r.isMut) throw YuxError(line, col, ErrorCode::E3115, "Mut", "Inline");
            if (r.isFrozen) throw YuxError(line, col, ErrorCode::E3115, "Frozen", "Inline");
            r.isInline = true;
        } else {
            throw YuxError(line, col, ErrorCode::E3112, name);
        }
    }
    return r;
}

} // namespace

#endif // YUX_LANG_AST_BUILDER_HELPERS_H
