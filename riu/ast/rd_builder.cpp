// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "rd_builder.h"

#include "ast_builder_helpers.h"
#include "node/alias_node.h"
#include "node/enum_node.h"
#include "node/expr_node.h"
#include "node/fn_node.h"
#include "node/global_const_node.h"
#include "node/global_var_node.h"
#include "node/literal_node.h"
#include "node/spec_node.h"
#include "node/statement_node.h"
#include "node/struct_node.h"
#include "node/type_node.h"
#include "type_validate.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <set>

namespace {

bool isTypeKind(rd::NodeKind k) {
    switch (k) {
    case rd::NodeKind::TypePath:
    case rd::NodeKind::TypeGeneric:
    case rd::NodeKind::TypeNullable:
    case rd::NodeKind::TypeFallible:
    case rd::NodeKind::TypeSelf:
    case rd::NodeKind::TypeArray:
    case rd::NodeKind::TypeUnit:
    case rd::NodeKind::TypeTuple:
        return true;
    default:
        return false;
    }
}

bool isPublicTypeName(const string& name) {
    return !name.empty() && name[0] != '_';
}

bool hasPublicType(FileNode* f, const string& name) {
    if (!f || !isPublicTypeName(name)) return false;
    return f->localStructDecl(name, true) || f->localEnumDecl(name) || f->localAliasDecl(name);
}

void registerFqPrefix(FileNode* file, const TypePath& path, FileNode* target) {
    if (!file || !target || path.size() < 2) return;
    string first = path[0].getText();
    string childKey;
    for (size_t i = 1; i < path.size(); ++i) {
        if (i > 1) childKey += '.';
        childKey += path[i].getText();
    }
    auto* firstSym = file->lookupSymbol(first);
    if (firstSym && firstSym->kind == SymbolKind::Package) {
        file->addPackageChild(first, childKey, target);
        return;
    }
    if (!file->localSymbols().contains(first)) {
        SymbolInfo pkg(SymbolKind::Package, first, TypeInfo());
        pkg.moduleName = first;
        file->registerSymbol(first, pkg);
        file->addPackageAlias(first, first);
        file->addPackageChild(first, childKey, target);
    }
}

void registerLastSegModuleAlias(FileNode* file, const string& alias, FileNode* target, const string& modName, int line,
                                bool failIfExists) {
    if (!file || !target || alias.empty()) return;
    if (file->localSymbols().contains(alias)) {
        if (failIfExists) throw RiuError(line, ErrorCode::E2004, alias);
        return;
    }
    SymbolInfo aliasSym(SymbolKind::Module, alias, TypeInfo());
    aliasSym.moduleName = modName;
    file->registerSymbol(alias, aliasSym);
    file->addModuleAlias(alias, target);
}

void injectFileWildcard(FileNode* file, FileNode* target, const string& childMod) {
    for (auto& [name, overloads] : target->localFnSymbols()) {
        for (auto& fnInfo : overloads) {
            if (fnInfo->moduleName != target->moduleName()) continue;
            if (fnInfo->isPrivate) continue;
            file->registerFnSymbol(name, *fnInfo);
        }
    }
    for (auto& [name, sym] : target->localSymbols()) {
        if (sym->moduleName != target->moduleName()) continue;
        if (sym->isPrivate) continue;
        if (file->localSymbols().count(name)) continue;
        file->registerSymbol(name, *sym);
    }
    file->addWildcardImport(target);
    for (auto* decl : target->getStructDecls()) {
        if (!decl) continue;
        string sname = decl->name().getText();
        if (sname.empty() || sname[0] == '_') continue;
        if (file->localStructDecl(sname, true) || file->localEnumDecl(sname) || file->localAliasDecl(sname)) continue;
        if (file->localSymbols().count(sname)) continue;
        SymbolInfo sym{SymbolKind::Struct, sname, TypeInfo(sname, target->moduleName())};
        sym.moduleName = target->moduleName();
        file->registerSymbol(sname, sym);
        (void)childMod;
    }
}

string lastSeg(std::string_view dotted) {
    auto pos = dotted.rfind('.');
    if (pos == std::string_view::npos) return string(dotted);
    return string(dotted.substr(pos + 1));
}

string dottedJoin(const string& prefix, const string& name) {
    string s = prefix;
    s += '.';
    s += name;
    return s;
}

} // namespace

RdBuilder::RdBuilder(Riu& riu, string src, string moduleName, bool isTestFile, string sourcePath)
    : _riu(riu), _isTestFile(isTestFile), _moduleName(std::move(moduleName)), _sourcePath(std::move(sourcePath)),
      _src(std::move(src)) {
    // 无 riu.toml 的单文件 check 不写 .ud，不必再拷一份 item 原文。
    // parseSdkDir 写 SDK 缓存前会 setKeepItemSourceText，否则 v5 .ud 缺 Eq 骨架。
    _keepSourceText = _riu.keepItemSourceText();
}

RdBuilder::~RdBuilder() {
    for (auto it = _nodes.rbegin(); it != _nodes.rend(); ++it)
        (*it)->~Node();
}

void* RdBuilder::arenaAlloc(size_t size, size_t align) {
    if (align < 1) align = 1;
    const size_t mask = align - 1;
    constexpr size_t kBlock = size_t{1} << 20u;
    size_t used = (_arenaUsed + mask) & ~mask;
    if (!_arenaCur || used + size > _arenaCap) {
        size_t cap = kBlock;
        if (size + align + 16 > cap) cap = size + align + 16;
        auto block = std::make_unique<char[]>(cap); // NOLINT(modernize-avoid-c-arrays)
        _arenaCur = block.get();
        _arenaCap = cap;
        _arenaBlocks.push_back(std::move(block));
        used = 0;
    }
    _arenaUsed = used + size;
    return _arenaCur + used;
}

void RdBuilder::releaseParseTemps() {
    _ast = {};
    _defaultToks.clear();
    _defaultToks.shrink_to_fit();
    _errors.clear();
    _errors.shrink_to_fit();
    _src.clear();
    _src.shrink_to_fit();
}

void RdBuilder::indexDefaultTokens() {
    rd::Scanner sc(_src);
    _defaultToks.reserve(_src.size() / 4);
    for (;;) {
        rd::Token t = sc.next();
        if (t.kind == rd::Kind::Eof) break;
        _defaultToks.push_back(t);
    }
}

std::pair<int, int> RdBuilder::tokenRange(const rd::Pos& pos) const {
    if (_defaultToks.empty()) return {-1, -1};
    // 按 offset 升序；找第一个不完全落在 pos.offset 之前的 token。
    auto first = std::lower_bound(_defaultToks.begin(), _defaultToks.end(), pos.offset, // NOLINT(modernize-use-ranges)
                                  [](const rd::Token& t, rd::i32 off) { return t.pos.end <= off; });
    if (first == _defaultToks.end()) return {-1, -1};
    auto last = first;
    if (pos.end > pos.offset) {
        last = std::lower_bound(first, _defaultToks.end(), pos.end, // NOLINT(modernize-use-ranges)
                                [](const rd::Token& t, rd::i32 off) { return t.pos.offset < off; });
        if (last == first) return {-1, -1};
        --last;
    }
    return {first->index, last->index};
}

size_t RdBuilder::tokenIndexAt(rd::i32 offset) const {
    if (_defaultToks.empty()) return 0;
    auto it = std::lower_bound(_defaultToks.begin(), _defaultToks.end(), offset, // NOLINT(modernize-use-ranges)
                               [](const rd::Token& t, rd::i32 off) { return t.pos.end <= off; });
    if (it == _defaultToks.end()) return static_cast<size_t>(_defaultToks.back().index);
    return static_cast<size_t>(it->index);
}

// 文本进 Riu StringIntern；releaseParseTemps 可丢源 / FlatAst。
Token RdBuilder::makeTok(std::string_view text, const rd::Pos& pos) const {
    const size_t stop = pos.end > 0 ? static_cast<size_t>(pos.end - 1) : 0;
    return {text,
            static_cast<size_t>(pos.line),
            static_cast<size_t>(pos.column),
            tokenIndexAt(pos.offset),
            static_cast<size_t>(pos.offset),
            stop};
}

Token RdBuilder::makeTok(rd::NodeId id) const {
    const auto& n = at(id);
    return makeTok(n.value, n.pos);
}

string RdBuilder::srcSlice(const rd::Pos& pos) const {
    if (pos.offset < 0 || pos.end < pos.offset) return {};
    const auto off = static_cast<size_t>(pos.offset);
    auto n = static_cast<size_t>(pos.end - pos.offset);
    if (off > _src.size()) return {};
    if (off + n > _src.size()) n = _src.size() - off;
    return _src.substr(off, n);
}

TypePath RdBuilder::pathFromDotted(std::string_view dotted, const rd::Pos& pos) const {
    TypePath p;
    size_t start = 0;
    for (size_t i = 0; i <= dotted.size(); ++i) {
        if (i != dotted.size() && dotted[i] != '.') continue;
        auto seg = dotted.substr(start, i - start);
        if (!seg.empty()) p.push_back(makeTok(seg, pos));
        start = i + 1;
    }
    return p;
}

string RdBuilder::findEnclosingStructName() const {
    for (auto it = _scopeStack.rbegin(); it != _scopeStack.rend(); ++it) {
        if (auto* impl = dynamic_cast<StructImplNode*>(*it)) return impl->structName();
    }
    return {};
}

TypeNode* RdBuilder::wrapRefIf(TypeNode* inner, bool isAnd, const rd::Pos& pos) {
    if (!isAnd || !inner) return inner;
    Token refName("Ref", static_cast<size_t>(pos.line));
    return static_cast<TypeNode*>(create<TypeGenericNode>(pos, currentScope(), refName, vector<TypeNode*>{inner}));
}

TypeNode* RdBuilder::applyNullableSuffix(TypeNode* inner, const rd::Pos& questPos) {
    if (auto* fn = dynamic_cast<TypeFnNode*>(inner)) {
        if (!fn->nullable()) {
            fn->setNullable(true);
            return inner;
        }
    }
    Token nullableName("Nullable", static_cast<size_t>(questPos.line));
    return static_cast<TypeNode*>(
        create<TypeGenericNode>(questPos, currentScope(), nullableName, vector<TypeNode*>{inner}));
}

TypeNode* RdBuilder::makeFunctionType(const rd::Pos& pos, vector<TypeNode*> typeArgs, bool nullable) {
    if (typeArgs.empty()) {
        throw RiuError(pos.line, pos.column + 1, ErrorCode::E6011, std::string("Function"), static_cast<size_t>(1),
                       static_cast<size_t>(0))
            .withHint("`Function` 须带类型实参：`Function<Ret>` 或 `Function<P1, P2, ..., Ret>`");
    }
    TypeNode* retType = typeArgs.back();
    vector<TypeNode*> paramTypes(typeArgs.begin(), typeArgs.end() - 1);
    if (auto* tup = dynamic_cast<TypeTupleNode*>(retType)) {
        if (tup->elementTypes().empty()) retType = nullptr;
    }
    return static_cast<TypeNode*>(create<TypeFnNode>(pos, currentScope(), std::move(paramTypes), retType, nullable));
}

string RdBuilder::requireBareTypeParamName(rd::NodeId id) {
    const auto& n = at(id);
    if (n.kind != rd::NodeKind::TypePath || n.op == rd::Kind::SymbolAnd) {
        throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E4037, std::string("type parameter"))
            .withHint("声明头写 `<T>`，借用写在形参上：`fn f<T>(x T&)`");
    }
    string name = lastSeg(n.value);
    checkDiscardDeclName(name, "type parameter", n.pos.line, n.pos.column + 1);
    return name;
}

TypeNode* RdBuilder::buildType(rd::NodeId id) {
    if (id == rd::kEmptyNode) return nullptr;
    const auto& n = at(id);
    Node* parent = currentScope();
    const bool trailingAnd = n.op == rd::Kind::SymbolAnd;

    switch (n.kind) {
    case rd::NodeKind::TypePath: {
        auto path = pathFromDotted(n.value, n.pos);
        if (path.empty()) return nullptr;
        Token last = path.last();
        if (path.isBare() && last.getText() == "Arc") {
            throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E4029, std::string("?"));
        }
        if (path.isBare() && last.getText() == "Function") {
            throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E6011, std::string("Function"),
                           static_cast<size_t>(1), static_cast<size_t>(0))
                .withHint(
                    "`Function` 是特殊泛型，须写 `Function<Ret>` 或 `Function<P1, P2, ..., Ret>`（末位为返回类型）");
        }
        auto* inner = static_cast<TypeNode*>(create<TypeNormalNode>(id, parent, std::move(path)));
        return wrapRefIf(inner, trailingAnd, n.pos);
    }
    case rd::NodeKind::TypeSelf: {
        Token tk = makeTok("Self", n.pos);
        auto* inner = static_cast<TypeNode*>(create<TypeSelfNode>(id, parent, tk, findEnclosingStructName()));
        return wrapRefIf(inner, trailingAnd, n.pos);
    }
    case rd::NodeKind::TypeGeneric: {
        auto path = pathFromDotted(n.value, n.pos);
        Token last = path.last();
        vector<TypeNode*> typeArgs;
        typeArgs.reserve(n.children_count);
        for (rd::i32 i = 0; i < n.children_count; ++i)
            typeArgs.push_back(buildType(child(id, i)));
        if (path.isBare() && last.getText() == "Arc") {
            std::string innerName = typeArgs.empty() ? std::string("?") : typeArgs[0]->getType().name;
            throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E4029, innerName);
        }
        if (path.isBare() && last.getText() == "Weak" && typeArgs.size() == 1) {
            if (dynamic_cast<TypeFnNode*>(typeArgs[0]) || typeArgs[0]->getType().isFn()) {
                throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E2001)
                    .withHint("函数值不是堆句柄、无 RC 头，不能用 Weak<...> 包裹（spec §3.7 / §5.5）");
            }
        }
        TypeNode* inner;
        if (path.isBare() && last.getText() == "Function") {
            inner = makeFunctionType(n.pos, std::move(typeArgs), false);
        } else {
            inner = static_cast<TypeNode*>(create<TypeGenericNode>(id, parent, std::move(path), typeArgs));
        }
        return wrapRefIf(inner, trailingAnd, n.pos);
    }
    case rd::NodeKind::TypeArray: {
        TypeNode* elem = n.children_count > 0 ? buildType(child(id, 0)) : nullptr;
        Token count = n.children_count > 1 ? makeTok(child(id, 1)) : Token("0", n.pos.line);
        auto* inner = static_cast<TypeNode*>(create<TypeArrayNode>(id, parent, elem, count));
        return wrapRefIf(inner, trailingAnd, n.pos);
    }
    case rd::NodeKind::TypeTuple: {
        vector<TypeNode*> elems;
        elems.reserve(n.children_count);
        for (rd::i32 i = 0; i < n.children_count; ++i)
            elems.push_back(buildType(child(id, i)));
        return static_cast<TypeNode*>(create<TypeTupleNode>(id, parent, std::move(elems)));
    }
    case rd::NodeKind::TypeUnit:
        return static_cast<TypeNode*>(create<TypeTupleNode>(id, parent, vector<TypeNode*>{}));
    case rd::NodeKind::TypeFallible: {
        TypeNode* base = n.children_count > 0 ? buildType(child(id, 0)) : nullptr;
        TypeNode* err = n.children_count > 1 ? buildType(child(id, 1)) : nullptr;
        if (err && err->getType().isNullable()) {
            throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E2001)
                .withHint("`T ! E?` is invalid — error type `E` must not be nullable");
        }
        auto* inner = static_cast<TypeNode*>(create<TypeFallibleNode>(id, parent, base, err));
        return wrapRefIf(inner, trailingAnd, n.pos);
    }
    case rd::NodeKind::TypeNullable: {
        TypeNode* inner = n.children_count > 0 ? buildType(child(id, 0)) : nullptr;
        if (inner && inner->getType().isWeak()) {
            throw RiuError(n.pos.line, n.pos.column + 1, ErrorCode::E2001)
                .withHint("Weak<T> 本身已可空；若需在持有者失效后取值，使用 `upgrade(weak)`，其结果即为 Rc<T>?");
        }
        auto* nul = applyNullableSuffix(inner, n.pos);
        return wrapRefIf(nul, trailingAnd, n.pos);
    }
    default:
        return nullptr;
    }
}

void RdBuilder::parseTypeParams(rd::NodeId generic, vector<string>& names, vector<vector<SpecRef>>& bounds,
                                vector<TypeNode*>* defaults) {
    if (generic == rd::kEmptyNode) return;
    const auto& g = at(generic);
    for (rd::i32 i = 0; i < g.children_count; ++i) {
        rd::NodeId p = child(generic, i);
        const auto& pn = at(p);
        TypeNode* def = nullptr;
        if (pn.kind == rd::NodeKind::Generic && pn.children_count >= 1) {
            names.push_back(requireBareTypeParamName(child(p, 0)));
            const bool hasDefault = pn.op == rd::Kind::SymbolEq;
            const rd::i32 boundEnd = hasDefault ? pn.children_count - 1 : pn.children_count;
            vector<SpecRef> b;
            for (rd::i32 j = 1; j < boundEnd; ++j)
                b.push_back(specRefFromType(child(p, j)));
            bounds.push_back(std::move(b));
            if (hasDefault && boundEnd >= 1) {
                def = buildType(child(p, boundEnd));
                if (def) {
                    TypeInfo ti = def->getType();
                    validateOwnedTypeArgs("type parameter default", {ti}, def->getLineNumber(), def->getColumn());
                }
            }
        } else {
            names.push_back(requireBareTypeParamName(p));
            bounds.emplace_back();
        }
        if (defaults) defaults->push_back(def);
    }
}

SpecRef RdBuilder::specRefFromType(rd::NodeId id) {
    SpecRef r;
    if (id == rd::kEmptyNode) return r;
    const auto& n = at(id);
    r.line = n.pos.line;
    r.col = n.pos.column + 1;
    if (n.op == rd::Kind::SymbolAnd) {
        throw RiuError(r.line, r.col, ErrorCode::E4037, std::string("spec bound"))
            .withHint("边界写 `<T : D>` / `<T : D<A>>`，不要 `D&`");
    }
    if (n.kind == rd::NodeKind::TypePath) {
        r.name = lastSeg(n.value);
        return r;
    }
    if (n.kind == rd::NodeKind::TypeGeneric) {
        r.name = lastSeg(n.value);
        for (rd::i32 i = 0; i < n.children_count; ++i) {
            TypeNode* t = buildType(child(id, i));
            TypeInfo ti = t ? t->getType() : TypeInfo();
            validateOwnedTypeArgs("spec type arg", {ti}, r.line, r.col);
            r.typeArgs.push_back(std::move(ti));
        }
        return r;
    }
    throw RiuError(r.line, r.col, ErrorCode::E3030, string(n.value));
}

string RdBuilder::annoParenText(rd::NodeId anno) const {
    const auto& n = at(anno);
    if (n.op != rd::Kind::ParStart) return {};
    auto i = static_cast<size_t>(n.pos.offset);
    while (i < _src.size() && _src[i] != '(')
        ++i;
    if (i >= _src.size()) return {};
    ++i;
    int angle = 0;
    const size_t start = i;
    while (i < _src.size()) {
        const char c = _src[i];
        if (c == '<')
            ++angle;
        else if (c == '>' && angle > 0)
            --angle;
        else if (c == ')' && angle == 0)
            break;
        ++i;
    }
    string out;
    for (size_t j = start; j < i; ++j) {
        if (out.empty() && (_src[j] == ' ' || _src[j] == '\t')) continue;
        out.push_back(_src[j]);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    return out;
}

string RdBuilder::annoArgText(rd::NodeId anno) const {
    const auto& n = at(anno);
    if (n.children_count <= 0) return {};
    const rd::NodeId argId = child(anno, 0);
    const auto& arg = at(argId);
    // 字符串 / 数字用节点 value（StringLit 的 pos 含引号，不能 srcSlice）。
    switch (arg.kind) {
    case rd::NodeKind::StringLit:
    case rd::NodeKind::IntLit:
    case rd::NodeKind::FloatLit:
    case rd::NodeKind::Ident:
        return string(arg.value);
    case rd::NodeKind::StringInterp: {
        string t;
        for (rd::i32 i = 0; i < arg.children_count; ++i) {
            const auto& p = at(child(argId, i));
            if (p.kind == rd::NodeKind::TplText) t += p.value;
        }
        return t;
    }
    default:
        return srcSlice(arg.pos);
    }
}

AnnoCall RdBuilder::buildAnnoCall(rd::NodeId anno) {
    const auto& n = at(anno);
    AnnoCall call;
    call.name = string(n.value);
    call.line = n.pos.line;
    call.col = n.pos.column + 1;
    rd::i32 i = 0;
    while (i < n.children_count) {
        AnnoArg arg;
        rd::NodeId kid = child(anno, i);
        if (at(kid).kind == rd::NodeKind::Ident && i + 1 < n.children_count) {
            arg.field = string(at(kid).value);
            arg.expr = buildExpr(child(anno, i + 1));
            i += 2;
        } else {
            arg.expr = buildExpr(kid);
            ++i;
        }
        call.args.push_back(std::move(arg));
    }
    return call;
}

void RdBuilder::applyPrefixAnnos(StatementNode* stmt, vector<AnnoCall>&& annos) {
    if (!stmt || annos.empty()) return;
    stmt->setPrefixAnnos(std::move(annos));
}

vector<AnnoCall> RdBuilder::collectAnnos(rd::NodeId parent, rd::i32 from, rd::i32 to, bool nonFn, bool externFn) {
    vector<AnnoCall> out;
    const auto& n = at(parent);
    if (to < 0) to = n.children_count;
    for (rd::i32 i = from; i < to; ++i) {
        rd::NodeId a = child(parent, i);
        if (at(a).kind != rd::NodeKind::Anno) break;
        AnnoCall call = buildAnnoCall(a);
        const string& name = call.name;
        int line = call.line;
        int col = call.col;
        if (name == "Fallible") {
            throw RiuError(line, col, ErrorCode::E2005, name)
                .withHint("removed; declare failure with `T ! E` in the function signature instead");
        }
        if (!knownAnnos().contains(name)) throw RiuError(line, col, ErrorCode::E2005, name);
        if (name == "DraftLike") throw RiuError(line, col, ErrorCode::E1110);
        const bool hasArg = at(a).op == rd::Kind::ParStart;
        const bool needArg = argAnnos().contains(name);
        if (needArg != hasArg) throw RiuError(line, col, ErrorCode::E2005, name);
        if (nonFn && !nonFnAllowedAnnos().contains(name)) throw RiuError(line, col, ErrorCode::E2011, name);
        if (externFn && !externFnAllowedAnnos().contains(name)) throw RiuError(line, col, ErrorCode::E2011, name);
        out.push_back(std::move(call));
    }
    return out;
}

RdLetFlags RdBuilder::readLetAnnos(const vector<rd::NodeId>& annos) {
    RdLetFlags r;
    for (rd::NodeId a : annos) {
        const string name = string(at(a).value);
        int line = at(a).pos.line;
        int col = at(a).pos.column + 1;
        if (name == "Mut") {
            if (r.isFrozen) throw RiuError(line, col, ErrorCode::E3115, "Frozen", "Mut");
            if (r.isCval) throw RiuError(line, col, ErrorCode::E3115, "Cval", "Mut");
            if (r.isInline) throw RiuError(line, col, ErrorCode::E3115, "Inline", "Mut");
            r.isMut = true;
        } else if (name == "Frozen") {
            if (r.isMut) throw RiuError(line, col, ErrorCode::E3115, "Mut", "Frozen");
            if (r.isCval) throw RiuError(line, col, ErrorCode::E3115, "Cval", "Frozen");
            if (r.isInline) throw RiuError(line, col, ErrorCode::E3115, "Inline", "Frozen");
            r.isFrozen = true;
        } else if (name == "Cval") {
            if (r.isMut) throw RiuError(line, col, ErrorCode::E3115, "Mut", "Cval");
            if (r.isFrozen) throw RiuError(line, col, ErrorCode::E3115, "Frozen", "Cval");
            r.isCval = true;
        } else if (name == "Inline") {
            if (r.isMut) throw RiuError(line, col, ErrorCode::E3115, "Mut", "Inline");
            if (r.isFrozen) throw RiuError(line, col, ErrorCode::E3115, "Frozen", "Inline");
            r.isInline = true;
        } else {
            throw RiuError(line, col, ErrorCode::E3112, name);
        }
    }
    return r;
}

SpecRef RdBuilder::specRefFromAnno(rd::NodeId anno) {
    SpecRef r;
    const auto& n = at(anno);
    r.line = n.pos.line;
    r.col = n.pos.column + 1;
    if (n.children_count <= 0) {
        r.name = annoArgText(anno);
        return r;
    }
    rd::NodeId arg = child(anno, 0);
    if (isTypeKind(at(arg).kind)) return specRefFromType(arg);
    // 切片 2 桥接：注解槽走 expr，从源文本括号内再解析 spec。
    string text = annoParenText(anno);
    if (text.empty()) text = annoCallFirstArgText(buildAnnoCall(anno));
    if (text.empty()) text = annoArgText(anno);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    string compact;
    compact.reserve(text.size());
    for (unsigned char c : text) {
        if (c != ' ' && c != '\t') compact.push_back(static_cast<char>(c));
    }
    text = std::move(compact);
    TypeInfo ti = TypeInfo::fromFullName(text);
    if (ti.empty()) {
        r.name = text;
        return r;
    }
    r.name = ti.name;
    for (const auto& arg : ti.genericArgs) {
        if (!arg) continue;
        validateOwnedTypeArgs("spec type arg", {*arg}, r.line, r.col);
        r.typeArgs.push_back(*arg);
    }
    return r;
}

void RdBuilder::preloadPackageChildren(FileNode* file, const string& alias, const string& pkgModName,
                                       const string& relPrefix, int errorLine) {
    auto childKey = [&](const string& name) -> string {
        if (relPrefix.empty()) return name;
        return dottedJoin(relPrefix, name);
    };
    if (_riu.hasPkgFile(pkgModName)) {
        for (const auto& item : _riu.visiblePkgItems(file, pkgModName)) {
            string childMod = dottedJoin(pkgModName, item.name);
            string key = childKey(pkgExportName(item));
            auto kind = _riu.modulePathKind(childMod);
            if (kind == Riu::ModulePathKind::File) {
                auto childFile = childMod == file->moduleName() ? file : _riu.loadModule(childMod, errorLine);
                file->addPackageChild(alias, key, childFile);
            } else if (kind == Riu::ModulePathKind::Package) {
                preloadPackageChildren(file, alias, childMod, key, errorLine);
            }
        }
        return;
    }
    for (auto& ch : _riu.listPackageRiuChildren(pkgModName)) {
        string childMod = dottedJoin(pkgModName, ch);
        auto childFile = childMod == file->moduleName() ? file : _riu.loadModule(childMod, errorLine);
        file->addPackageChild(alias, childKey(ch), childFile);
    }
    for (auto& sub : _riu.listPackageSubdirs(pkgModName)) {
        preloadPackageChildren(file, alias, dottedJoin(pkgModName, sub), childKey(sub), errorLine);
    }
}

void RdBuilder::expandPackageWildcard(FileNode* file, const string& pkgModName, int line) {
    if (_riu.hasPkgFile(pkgModName)) {
        for (const auto& exportItem : _riu.visiblePkgItems(file, pkgModName)) {
            string childMod = dottedJoin(pkgModName, exportItem.name);
            if (childMod == file->moduleName()) continue;
            if (exportItem.wildcard) {
                auto childKind = _riu.modulePathKind(childMod);
                if (childKind == Riu::ModulePathKind::File) {
                    injectFileWildcard(file, _riu.loadModule(childMod, line), childMod);
                } else if (childKind == Riu::ModulePathKind::Package) {
                    expandPackageWildcard(file, childMod, line);
                }
            } else {
                string exportedName = pkgExportName(exportItem);
                auto childPathKind = _riu.modulePathKind(childMod);
                bool alreadyWildcard = file->wildcardAliasSources(exportedName) != nullptr;
                if (!alreadyWildcard && file->hasSymbol(exportedName)) continue;
                if (childPathKind == Riu::ModulePathKind::File) {
                    if (!alreadyWildcard) {
                        auto target = _riu.loadModule(childMod, line);
                        SymbolInfo aliasSym(SymbolKind::Module, exportedName, TypeInfo());
                        aliasSym.moduleName = childMod;
                        file->registerSymbol(exportedName, aliasSym);
                        file->addModuleAlias(exportedName, target);
                    }
                    file->addWildcardAliasSource(exportedName, childMod);
                } else if (childPathKind == Riu::ModulePathKind::Package) {
                    if (!alreadyWildcard) {
                        SymbolInfo aliasSym(SymbolKind::Package, exportedName, TypeInfo());
                        aliasSym.moduleName = childMod;
                        file->registerSymbol(exportedName, aliasSym);
                        file->addPackageAlias(exportedName, childMod);
                        preloadPackageChildren(file, exportedName, childMod, "", line);
                    }
                    file->addWildcardAliasSource(exportedName, childMod);
                }
            }
        }
        return;
    }
    for (auto& ch : _riu.listPackageRiuChildren(pkgModName)) {
        string childMod = dottedJoin(pkgModName, ch);
        bool alreadyWildcard = file->wildcardAliasSources(ch) != nullptr;
        auto existingSym = file->lookupSymbol(ch);
        bool canOverride =
            existingSym && (existingSym->kind == SymbolKind::Function || existingSym->kind == SymbolKind::Variable);
        if (!alreadyWildcard && existingSym && !canOverride) continue;
        if (!alreadyWildcard) {
            auto target = _riu.loadModule(childMod, line);
            SymbolInfo aliasSym(SymbolKind::Module, ch, TypeInfo());
            aliasSym.moduleName = childMod;
            file->registerSymbol(ch, aliasSym);
            file->addModuleAlias(ch, target);
        }
        file->addWildcardAliasSource(ch, childMod);
    }
    for (auto& sub : _riu.listPackageSubdirs(pkgModName)) {
        string subMod = dottedJoin(pkgModName, sub);
        bool alreadyWildcard = file->wildcardAliasSources(sub) != nullptr;
        if (!alreadyWildcard && file->hasSymbol(sub)) continue;
        if (!alreadyWildcard) {
            SymbolInfo subSym(SymbolKind::Package, sub, TypeInfo());
            subSym.moduleName = subMod;
            file->registerSymbol(sub, subSym);
            file->addPackageAlias(sub, subMod);
            preloadPackageChildren(file, sub, subMod, "", line);
        }
        file->addWildcardAliasSource(sub, subMod);
    }
}

void RdBuilder::addUse(rd::NodeId id) {
    auto* file = dynamic_cast<FileNode*>(currentScope());
    if (!file) return;
    const auto& n = at(id);
    string raw = string(n.value);
    bool wildcard = raw.size() >= 2 && raw.substr(raw.size() - 2) == ".*";
    string modName = wildcard ? raw.substr(0, raw.size() - 2) : raw;
    auto path = pathFromDotted(modName, n.pos);
    string alias = path.empty() ? string() : path.lastName();
    int line = n.pos.line;
    checkDiscardDeclName(alias, "import alias", line, n.pos.column + 1);

    FileNode::UseSpec spec;
    spec.moduleName = modName;
    spec.alias = alias;
    spec.wildcard = wildcard;
    spec.line = line;
    file->addUseSpec(spec);
    file->addImport(modName);

    if (!_expandImports) return;
    if (modName == "riu.core" || modName == file->moduleName()) return;

    modName = _riu.resolvePkgPath(file, modName, line);
    auto pathKind = _riu.modulePathKind(modName);
    if (pathKind == Riu::ModulePathKind::NotFound && _riu.module(modName)) {
        pathKind = Riu::ModulePathKind::File;
    }
    if (pathKind == Riu::ModulePathKind::Conflict) {
        throw RiuError(line, ErrorCode::E2003, modName, modName, modName);
    }

    if (!wildcard) {
        if (file->localSymbols().contains(alias)) {
            auto kind = file->localSymbols().at(alias)->kind;
            bool l1Type =
                file->localStructDecl(alias, true) || file->localEnumDecl(alias) || file->localAliasDecl(alias);
            if (l1Type || kind == SymbolKind::Module || kind == SymbolKind::Package || kind == SymbolKind::Function) {
                throw RiuError(line, ErrorCode::E2004, alias);
            }
        }
        if (pathKind == Riu::ModulePathKind::Package) {
            SymbolInfo aliasSym(SymbolKind::Package, alias, TypeInfo());
            aliasSym.moduleName = modName;
            file->registerSymbol(alias, aliasSym);
            file->addPackageAlias(alias, modName);
            preloadPackageChildren(file, alias, modName, "", line);
            if (path.size() > 1) {
                string first = path[0].getText();
                string childKey;
                for (size_t i = 1; i < path.size(); ++i) {
                    if (!childKey.empty()) childKey += '.';
                    childKey += path[i].getText();
                }
                auto* firstSym = file->lookupSymbol(first);
                if (!firstSym) {
                    SymbolInfo prefix(SymbolKind::Package, first, TypeInfo());
                    prefix.moduleName = first;
                    file->registerSymbol(first, prefix);
                    file->addPackageAlias(first, first);
                    firstSym = file->lookupSymbol(first);
                }
                if (firstSym && firstSym->kind == SymbolKind::Package) {
                    preloadPackageChildren(file, first, modName, childKey, line);
                }
            }
            return;
        }
        if (path.size() >= 2 && pathKind == Riu::ModulePathKind::NotFound) {
            TypePath parentPath = path;
            parentPath.pop_back();
            string parentMod = _riu.resolvePkgPath(file, parentPath.dotted(), line);
            const string& typeName = path.lastName();
            FileNode* parent = _riu.module(parentMod);
            auto parentKind = _riu.modulePathKind(parentMod);
            if (parentKind == Riu::ModulePathKind::NotFound && parent) parentKind = Riu::ModulePathKind::File;
            if (!parent && parentKind == Riu::ModulePathKind::File) parent = _riu.loadModule(parentMod, line);
            if (parent && hasPublicType(parent, typeName)) {
                registerLastSegModuleAlias(file, parentPath.lastName(), parent, parent->moduleName(), line, false);
                registerFqPrefix(file, parentPath, parent);
                file->addNamedTypeImport(typeName, parent);
                return;
            }
        }
        auto target = _riu.loadModule(modName, line);
        SymbolInfo aliasSym(SymbolKind::Module, alias, TypeInfo());
        aliasSym.moduleName = modName;
        file->registerSymbol(alias, aliasSym);
        file->addModuleAlias(alias, target);
        registerFqPrefix(file, path, target);
        return;
    }

    if (pathKind == Riu::ModulePathKind::Package) {
        expandPackageWildcard(file, modName, line);
        return;
    }

    auto imported = _riu.loadModule(modName, line);
    for (auto& [name, overloads] : imported->localFnSymbols()) {
        for (auto& fnInfo : overloads) {
            if (fnInfo->moduleName != imported->moduleName()) continue;
            if (fnInfo->isPrivate) continue;
            file->registerFnSymbol(name, *fnInfo);
        }
    }
    for (auto& [name, sym] : imported->localSymbols()) {
        if (sym->moduleName != imported->moduleName()) continue;
        if (sym->isPrivate) continue;
        if (file->localSymbols().count(name)) continue;
        file->registerSymbol(name, *sym);
    }
    file->addWildcardImport(imported);
    for (auto* decl : imported->getStructDecls()) {
        if (!decl) continue;
        string sname = decl->name().getText();
        if (sname.empty() || sname[0] == '_') continue;
        if (file->localStructDecl(sname, true) || file->localEnumDecl(sname) || file->localAliasDecl(sname)) continue;
        if (file->localSymbols().count(sname)) continue;
        SymbolInfo sym{SymbolKind::Struct, sname, TypeInfo(sname, imported->moduleName())};
        sym.moduleName = imported->moduleName();
        file->registerSymbol(sname, sym);
    }
    registerLastSegModuleAlias(file, alias, imported, modName, line, false);
    registerFqPrefix(file, path, imported);
}
