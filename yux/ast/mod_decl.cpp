// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 模块 .decl：二进制接口 + skeleton 源文本再 parse。
// 格式：magic "YUXD" | u32 version | u64 source_hash | 正文。

#include "mod_decl.h"

#include "ast_builder.h"
#include "node/alias_node.h"
#include "node/enum_node.h"
#include "node/file_node.h"
#include "node/fn_node.h"
#include "node/global_const_node.h"
#include "node/global_var_node.h"
#include "node/spec_node.h"
#include "node/struct_node.h"
#include "node/type_node.h"
#include "parse_program.h"
#include "tools/syntax_error_listener.h"
#include "yux.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mod_decl {
namespace {

namespace fs = std::filesystem;

constexpr std::array<char, 4> kMagic{'Y', 'U', 'X', 'D'};

// ==================== 二进制读写 ====================

struct Writer {
    std::vector<uint8_t> buf;

    void u8(uint8_t v) { buf.push_back(v); }
    void u32(uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8u));
        buf.push_back(static_cast<uint8_t>(v >> 16u));
        buf.push_back(static_cast<uint8_t>(v >> 24u));
    }
    void u64(uint64_t v) {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32u));
    }
    void str(const string& s) {
        u32(static_cast<uint32_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void i32(int v) { u32(static_cast<uint32_t>(v)); }
};

struct Reader {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;

    explicit Reader(const std::vector<uint8_t>& b) : p(b.data()), end(b.data() + b.size()) {}

    void need(size_t n) const {
        if (std::cmp_less(end - p, n)) throw std::runtime_error("decl truncated");
    }
    uint8_t u8() {
        need(1);
        return *p++;
    }
    uint32_t u32() {
        need(4);
        uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
                     (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
        p += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t lo = u32();
        uint64_t hi = u32();
        return lo | (hi << 32u);
    }
    string str() {
        uint32_t n = u32();
        need(n);
        string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    int i32() { return static_cast<int>(u32()); }
};

void writeType(Writer& w, const TypeInfo& t) {
    w.u8(static_cast<uint8_t>(t.kind));
    w.str(t.name);
    w.u64(t.arraySize);
    if (t.elementType) {
        w.u8(1);
        writeType(w, *t.elementType);
    } else {
        w.u8(0);
    }
    w.u32(static_cast<uint32_t>(t.genericArgs.size()));
    for (auto& a : t.genericArgs) {
        if (a) {
            w.u8(1);
            writeType(w, *a);
        } else {
            w.u8(0);
        }
    }
    w.u8(t.fnNullable ? 1 : 0);
    w.str(t.fallibleErr);
}

TypeInfo readType(Reader& r) {
    TypeInfo t;
    t.kind = static_cast<TypeKind>(r.u8());
    t.name = r.str();
    t.arraySize = r.u64();
    if (r.u8()) {
        t.elementType = std::make_shared<TypeInfo>(readType(r));
    }
    uint32_t n = r.u32();
    t.genericArgs.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (r.u8()) {
            t.genericArgs.push_back(std::make_shared<TypeInfo>(readType(r)));
        } else {
            t.genericArgs.emplace_back();
        }
    }
    t.fnNullable = r.u8() != 0;
    t.fallibleErr = r.str();
    return t;
}

void writeAnnos(Writer& w, const Annotated& a) {
    const auto& names = a.annos();
    const auto& args = a.annoArgs();
    w.u32(static_cast<uint32_t>(names.size()));
    for (size_t i = 0; i < names.size(); ++i) {
        w.str(names[i]);
        w.str(i < args.size() ? args[i] : string());
    }
}

void readAnnos(Reader& r, vector<string>& names, vector<string>& args) {
    uint32_t n = r.u32();
    names.resize(n);
    args.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        names[i] = r.str();
        args[i] = r.str();
    }
}

void writeStrings(Writer& w, const vector<string>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (auto& s : v)
        w.str(s);
}

vector<string> readStrings(Reader& r) {
    uint32_t n = r.u32();
    vector<string> v(n);
    for (uint32_t i = 0; i < n; ++i)
        v[i] = r.str();
    return v;
}

void writeHeader(Writer& w, FnHeaderNode* h) {
    w.str(h->name().getText());
    w.i32(h->getLineNumber());
    w.i32(h->getColumn());
    writeAnnos(w, *h);
    writeStrings(w, h->typeParams());
    w.u32(static_cast<uint32_t>(h->typeParamBounds().size()));
    for (auto& b : h->typeParamBounds())
        writeStrings(w, b);
    auto params = h->params();
    w.u32(static_cast<uint32_t>(params.size()));
    for (auto p : params) {
        w.str(p->name().getText());
        w.i32(p->getLineNumber());
        w.i32(p->getColumn());
        w.u8(p->isFrozen() ? 1 : 0);
        if (p->type()) {
            w.u8(1);
            writeType(w, p->type()->getType());
        } else {
            w.u8(0);
        }
    }
    if (h->retType()) {
        w.u8(1);
        writeType(w, h->retType()->getType());
    } else {
        w.u8(0);
    }
    w.str(h->resolvedFallibleErr());
}

TypeNode* typeNodeFromInfo(NodeOwner& own, Node* parent, const TypeInfo& t, int line) {
    if (t.empty() && t.kind == TypeKind::Normal) return nullptr;
    size_t ln = line > 0 ? static_cast<size_t>(line) : 1;
    Token tok(t.name, ln);
    switch (t.kind) {
    case TypeKind::Array: {
        auto elem = t.elementType ? typeNodeFromInfo(own, parent, *t.elementType, line) : nullptr;
        return own.make<TypeArrayNode>(parent, elem, TokenInfo(std::to_string(t.arraySize), ln));
    }
    case TypeKind::Tuple: {
        vector<TypeNode*> elems;
        elems.reserve(t.genericArgs.size());
        for (auto& e : t.genericArgs) {
            elems.push_back(e ? typeNodeFromInfo(own, parent, *e, line) : nullptr);
        }
        return own.make<TypeTupleNode>(parent, std::move(elems));
    }
    case TypeKind::Fn: {
        vector<TypeNode*> params;
        params.reserve(t.genericArgs.size());
        for (auto& e : t.genericArgs) {
            params.push_back(e ? typeNodeFromInfo(own, parent, *e, line) : nullptr);
        }
        TypeNode* ret = t.elementType ? typeNodeFromInfo(own, parent, *t.elementType, line) : nullptr;
        return own.make<TypeFnNode>(parent, std::move(params), ret, t.fnNullable);
    }
    case TypeKind::Normal:
        if (t.name == "Self") return own.make<TypeSelfNode>(parent, tok, string());
        return own.make<TypeNormalNode>(parent, tok);
    case TypeKind::Ptr:
        if (t.genericArgs.empty()) return own.make<TypeNormalNode>(parent, tok);
        [[fallthrough]];
    default: {
        vector<TypeNode*> args;
        args.reserve(t.genericArgs.size());
        for (auto& e : t.genericArgs) {
            args.push_back(e ? typeNodeFromInfo(own, parent, *e, line) : nullptr);
        }
        return own.make<TypeGenericNode>(parent, tok, std::move(args));
    }
    }
}

bool fnHasRealBody(FnNode* fn) {
    if (!fn || !fn->header()) return false;
    if (fn->header()->hasAnno("Builtin")) return false;
    return !fn->body().empty();
}

bool specNeedsSkeleton(SpecDeclNode* s) {
    if (!s) return false;
    // 静态字段契约包含字段注解（#Cval / #Inline 等），
    // 与默认方法体一样保留源码骨架，避免 .decl 丢失契约。
    if (!s->staticFields().empty()) return true;
    for (size_t i = 0; i < s->signatures().size(); ++i) {
        if (s->hasDefaultBody(i)) return true;
    }
    return false;
}

bool structNeedsSkeleton(StructDeclNode* d, StructImplNode* impl) {
    if (!d) return false;
    if (!d->staticFields().empty()) return true;
    if (!d->isGeneric() || !impl) return false;
    for (auto m : impl->methods()) {
        if (fnHasRealBody(m)) return true;
    }
    if (impl->destructor() && fnHasRealBody(impl->destructor())) return true;
    return false;
}

struct SkelItem {
    int line = 0;
    string text;
};

string buildSkeleton(vector<SkelItem> items) {
    std::ranges::sort(items, [](const SkelItem& a, const SkelItem& b) { return a.line < b.line; });
    string out;
    int line = 1;
    for (auto& it : items) {
        if (it.text.empty()) continue;
        int target = it.line > 0 ? it.line : 1;
        while (line < target) {
            out += '\n';
            ++line;
        }
        out += it.text;
        for (char c : it.text) {
            if (c == '\n') ++line;
        }
        if (out.empty() || out.back() != '\n') {
            out += '\n';
            ++line;
        }
    }
    return out;
}

bool onlyWs(const string& s) {
    return std::ranges::all_of(s, [](unsigned char c) { return std::isspace(c) != 0; });
}

uint64_t fnv1a64(const string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

string readAll(const string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void registerTopFn(FileNode* file, FnNode* fn) {
    auto* h = fn->header();
    string name = h->name().getText();
    TypeInfo ret = h->retType() ? h->retType()->getType() : TypeInfo();
    SymbolInfo fnSym(SymbolKind::Function, name, ret);
    fnSym.moduleName = file->moduleName();
    file->registerSymbol(name, fnSym);

    vector<TypeInfo> params;
    for (auto p : h->params()) {
        params.push_back(p->type() ? p->type()->getType() : TypeInfo());
    }
    FnSymbolInfo info{name, file->moduleName(), params, ret};
    info.isNoReturn = h->hasAnno("NoReturn");
    info.isConst = h->hasAnno("Const");
    info.fallibleErrType = h->resolvedFallibleErr();
    if (auto e = h->getAnnoArg("CName")) info.cName = *e;
    file->registerFnSymbol(name, info);

    fn->setParentScope(file);
    for (auto& tp : h->typeParams()) {
        fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    }
    for (auto param : h->params()) {
        TypeInfo pt = param->type() ? param->type()->getType() : TypeInfo();
        SymbolInfo si{SymbolKind::Variable, param->name().getText(), pt};
        if (param->isFrozen()) si.isFrozen = true;
        fn->registerSymbol(param->name().getText(), si);
    }
    file->addFunction(fn);
}

void registerMethod(FileNode* file, StructImplNode* impl, FnNode* method) {
    string structName = impl->structName();
    string methodName = method->header()->name().getText();
    string fullName = structName + "." + methodName;

    vector<TypeInfo> paramTypes;
    paramTypes.emplace_back(structName);
    for (auto param : method->header()->params()) {
        paramTypes.push_back(param->type() ? param->type()->getType() : TypeInfo());
    }
    TypeInfo retType;
    if (method->header()->retType()) retType = method->header()->retType()->getType();

    SymbolInfo methodSym(SymbolKind::Function, methodName, retType);
    methodSym.moduleName = file->moduleName();
    file->registerSymbol(fullName, methodSym);

    FnSymbolInfo methodFnSym{fullName, file->moduleName(), paramTypes, retType};
    methodFnSym.isNoReturn = method->header()->hasAnno("NoReturn");
    methodFnSym.isConst = method->header()->hasAnno("Const");
    methodFnSym.fallibleErrType = method->header()->resolvedFallibleErr();
    file->registerFnSymbol(fullName, methodFnSym);

    method->setParentScope(file);
    for (auto& tp : impl->typeParams()) {
        method->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    }
    for (auto& tp : method->header()->typeParams()) {
        method->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    }
    {
        vector<sp<TypeInfo>> selfArgs;
        auto selfInner = std::make_shared<TypeInfo>(structName);
        selfInner->ownerModule = file->moduleName();
        selfArgs.push_back(std::move(selfInner));
        method->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo("Ref", selfArgs)});
    }
    for (auto param : method->header()->params()) {
        TypeInfo pt = param->type() ? param->type()->getType() : TypeInfo();
        SymbolInfo si{SymbolKind::Variable, param->name().getText(), pt};
        if (param->isFrozen()) si.isFrozen = true;
        method->registerSymbol(param->name().getText(), si);
    }
}

// FnHeaderNode 没有 setRetType，readHeader 先读完再构造。
struct HeaderData {
    string name;
    int line = 0;
    int col = 0;
    vector<string> annos;
    vector<string> annoArgs;
    vector<string> typeParams;
    vector<vector<string>> bounds;
    struct Param {
        string name;
        int line = 0;
        int col = 0;
        bool frozen = false;
        bool hasType = false;
        TypeInfo type;
    };
    vector<Param> params;
    bool hasRet = false;
    TypeInfo ret;
    string fallibleErr;
};

HeaderData readHeaderData(Reader& r) {
    HeaderData d;
    d.name = r.str();
    d.line = r.i32();
    d.col = r.i32();
    readAnnos(r, d.annos, d.annoArgs);
    d.typeParams = readStrings(r);
    uint32_t nb = r.u32();
    d.bounds.resize(nb);
    for (uint32_t i = 0; i < nb; ++i)
        d.bounds[i] = readStrings(r);
    uint32_t np = r.u32();
    d.params.resize(np);
    for (uint32_t i = 0; i < np; ++i) {
        d.params[i].name = r.str();
        d.params[i].line = r.i32();
        d.params[i].col = r.i32();
        d.params[i].frozen = r.u8() != 0;
        d.params[i].hasType = r.u8() != 0;
        if (d.params[i].hasType) d.params[i].type = readType(r);
    }
    d.hasRet = r.u8() != 0;
    if (d.hasRet) d.ret = readType(r);
    d.fallibleErr = r.str();
    return d;
}

FnHeaderNode* makeHeader(NodeOwner& own, Node* parent, const HeaderData& d) {
    TypeNode* ret = d.hasRet ? typeNodeFromInfo(own, parent, d.ret, d.line) : nullptr;
    auto* header = own.make<FnHeaderNode>(parent, Token(d.name, static_cast<size_t>(d.line > 0 ? d.line : 1)), ret);
    header->setLocation(d.line, d.col);
    header->setAnnos(d.annos, d.annoArgs);
    header->setTypeParams(d.typeParams);
    header->setTypeParamBounds(d.bounds);
    for (auto& pd : d.params) {
        TypeNode* ty = pd.hasType ? typeNodeFromInfo(own, header, pd.type, pd.line) : nullptr;
        auto* param = own.make<FnParamNode>(header, Token(pd.name, static_cast<size_t>(pd.line > 0 ? pd.line : 1)), ty);
        param->setLocation(pd.line, pd.col);
        param->setFrozen(pd.frozen);
        header->addParam(param);
    }
    if (!d.fallibleErr.empty()) {
        header->setFallibleErrType(typeNodeFromInfo(own, header, TypeInfo(d.fallibleErr), d.line));
    }
    return header;
}

FnNode* makeFn(NodeOwner& own, Node* parent, const HeaderData& d) {
    auto* header = makeHeader(own, parent, d);
    auto* fn = own.make<FnNode>(parent, header);
    fn->setLocation(d.line, d.col);
    return fn;
}

bool parseSkeletonInto(Yux& yux, FileNode* file, const string& src, const string& absPath) {
    if (src.empty() || onlyWs(src)) return true;
    antlr4::ANTLRInputStream stream(src);
    yux::yuxLexer lexer(&stream);
    std::ostringstream sink;
    SyntaxErrorListener errListener(absPath, sink);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errListener);
    antlr4::CommonTokenStream tokens(&lexer);
    yux::yuxParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&errListener);
    auto* program = parseYuxProgram(parser, tokens, &errListener);
    if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
        DEBUG_LOG_VAL("  .decl skeleton syntax fail", absPath << " " << sink.str());
        return false;
    }
    auto builder = std::make_unique<ASTBuilder>(yux, file->moduleName(), false, absPath);
    builder->setTargetFile(file);
    try {
        builder->build(program);
    } catch (const std::exception& e) {
        DEBUG_LOG_VAL("  .decl skeleton AST fail", absPath << " : " << e.what());
        return false;
    }
    yux.keepBuilder(std::move(builder));
    return true;
}

} // namespace

NodeOwner::~NodeOwner() {
    for (auto n : _nodes)
        delete n;
}

std::string pathFor(const std::string& projectRoot, const std::string& buildDir, const std::string& srcAbs) {
    std::error_code ec;
    fs::path rel = fs::relative(srcAbs, projectRoot, ec);
    if (ec || rel.empty()) rel = fs::path(srcAbs).filename();
    rel.replace_extension(".decl");
    return (fs::path(buildDir) / rel).generic_string();
}

uint64_t sourceHash(const std::string& srcAbs) {
    return fnv1a64(readAll(srcAbs));
}

void write(FileNode* file, const std::string& srcAbs, const std::string& declPath) {
    if (!file || srcAbs.empty() || declPath.empty()) return;

    std::set<FnNode*> skelFns;
    std::set<StructDeclNode*> skelStructs;
    std::set<SpecDeclNode*> skelSpecs;
    vector<SkelItem> skelItems;

    for (auto fn : file->getFunctions()) {
        if (fn->header() && fn->header()->isGeneric() && fnHasRealBody(fn)) {
            skelFns.insert(fn);
            skelItems.push_back({.line = fn->getLineNumber(), .text = fn->sourceText()});
        }
    }
    for (auto d : file->getStructDecls()) {
        auto* impl = file->getStructImpl(d->name().getText());
        if (structNeedsSkeleton(d, impl)) {
            skelStructs.insert(d);
            skelItems.push_back({.line = d->getLineNumber(), .text = d->sourceText()});
        }
    }
    for (auto s : file->getSpecDecls()) {
        if (specNeedsSkeleton(s)) {
            skelSpecs.insert(s);
            skelItems.push_back({.line = s->getLineNumber(), .text = s->sourceText()});
        }
    }
    for (auto g : file->getGlobalConsts()) {
        skelItems.push_back({.line = g->getLineNumber(), .text = g->sourceText()});
    }
    for (auto g : file->getGlobalVars()) {
        skelItems.push_back({.line = g->getLineNumber(), .text = g->sourceText()});
    }

    Writer w;
    w.buf.insert(w.buf.end(), kMagic.begin(), kMagic.end());
    w.u32(kFormatVersion);
    w.u64(sourceHash(srcAbs));
    w.str(file->moduleName());

    w.u32(static_cast<uint32_t>(file->useSpecs().size()));
    for (auto& u : file->useSpecs()) {
        w.str(u.moduleName);
        w.str(u.alias);
        w.u8(u.wildcard ? 1 : 0);
        w.i32(u.line);
    }

    // extern：符号表里 isExternal、非方法
    vector<const FnSymbolInfo*> externs;
    for (auto& [name, overloads] : file->localFnSymbols()) {
        for (auto& fn : overloads) {
            if (!fn.isExternal) continue;
            if (fn.moduleName != file->moduleName()) continue;
            if (fn.name.find('.') != string::npos) continue;
            externs.push_back(&fn);
        }
    }
    w.u32(static_cast<uint32_t>(externs.size()));
    for (auto* fn : externs) {
        w.str(fn->name);
        w.u32(static_cast<uint32_t>(fn->params.size()));
        for (auto& p : fn->params)
            writeType(w, p);
        writeType(w, fn->retType);
        w.u8(fn->isNoReturn ? 1 : 0);
        w.str(fn->cName);
    }

    vector<FnNode*> binFns;
    for (auto fn : file->getFunctions()) {
        if (skelFns.contains(fn)) continue;
        binFns.push_back(fn);
    }
    w.u32(static_cast<uint32_t>(binFns.size()));
    for (auto fn : binFns)
        writeHeader(w, fn->header());

    vector<StructDeclNode*> binStructs;
    for (auto d : file->getStructDecls()) {
        if (skelStructs.contains(d)) continue;
        binStructs.push_back(d);
    }
    w.u32(static_cast<uint32_t>(binStructs.size()));
    for (auto d : binStructs) {
        w.str(d->name().getText());
        w.i32(d->getLineNumber());
        w.i32(d->getColumn());
        writeAnnos(w, *d);
        writeStrings(w, d->typeParams());
        w.u32(static_cast<uint32_t>(d->fields().size()));
        for (auto f : d->fields()) {
            w.str(f->name().getText());
            w.i32(f->getLineNumber());
            w.i32(f->getColumn());
            writeType(w, f->getType());
            w.u8(f->isVal() ? 1 : 0);
            w.u8(f->isFrozen() ? 1 : 0);
        }
        auto* impl = file->getStructImpl(d->name().getText());
        w.u8(impl ? 1 : 0);
        if (impl) {
            writeAnnos(w, *impl);
            w.u32(static_cast<uint32_t>(impl->specRefs().size()));
            for (auto& sr : impl->specRefs()) {
                w.str(sr.name);
                w.u32(static_cast<uint32_t>(sr.typeArgs.size()));
                for (auto& ta : sr.typeArgs)
                    writeType(w, ta);
                w.i32(sr.line);
                w.i32(sr.col);
            }
            w.u8(impl->destructor() ? 1 : 0);
            if (impl->destructor()) writeHeader(w, impl->destructor()->header());
            w.u32(static_cast<uint32_t>(impl->methods().size()));
            for (auto m : impl->methods())
                writeHeader(w, m->header());
        }
    }

    w.u32(static_cast<uint32_t>(file->getEnumDecls().size()));
    for (auto e : file->getEnumDecls()) {
        w.str(e->name().getText());
        w.i32(e->getLineNumber());
        w.i32(e->getColumn());
        writeAnnos(w, *e);
        w.u32(static_cast<uint32_t>(e->variants().size()));
        for (auto v : e->variants()) {
            w.str(v->name().getText());
            w.u32(static_cast<uint32_t>(v->payloadTypes().size()));
            for (auto t : v->payloadTypes())
                writeType(w, t->getType());
        }
    }

    w.u32(static_cast<uint32_t>(file->getAliasDecls().size()));
    for (auto a : file->getAliasDecls()) {
        w.str(a->name().getText());
        w.i32(a->getLineNumber());
        writeStrings(w, a->typeParams());
        writeType(w, a->target() ? a->target()->getType() : TypeInfo());
    }

    vector<SpecDeclNode*> binSpecs;
    for (auto s : file->getSpecDecls()) {
        if (skelSpecs.contains(s)) continue;
        binSpecs.push_back(s);
    }
    w.u32(static_cast<uint32_t>(binSpecs.size()));
    for (auto s : binSpecs) {
        w.str(s->name().getText());
        w.i32(s->getLineNumber());
        w.i32(s->getColumn());
        writeAnnos(w, *s);
        writeStrings(w, s->typeParams());
        w.u32(static_cast<uint32_t>(s->signatures().size()));
        for (auto sig : s->signatures())
            writeHeader(w, sig);
    }

    w.str(buildSkeleton(std::move(skelItems)));

    std::error_code ec;
    fs::create_directories(fs::path(declPath).parent_path(), ec);
    static std::atomic<uint32_t> tmpSeq{0};
    string tmp = declPath + ".tmp.";
    tmp += std::to_string(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    tmp += '.';
    tmp += std::to_string(tmpSeq.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc); // NOLINT(bugprone-signed-bitwise)
        if (!f) return;
        f.write(reinterpret_cast<const char*>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
        f.flush();
        if (!f) return;
    }
    fs::rename(tmp, declPath, ec);
    if (ec) {
        fs::remove(declPath, ec);
        ec.clear();
        fs::rename(tmp, declPath, ec);
    }
}

FileNode* tryLoad(Yux& yux, const std::string& declPath, const std::string& srcAbs, const std::string& moduleName) {
    std::error_code ec;
    if (!fs::exists(declPath, ec)) return nullptr;
    auto bytes = [&]() {
        std::ifstream f(declPath, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }();
    if (bytes.size() < 16) return nullptr;
    if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) return nullptr;

    auto owner = std::make_unique<NodeOwner>();
    FileNode* file = nullptr;
    try {
        Reader r(bytes);
        r.p += 4;
        uint32_t ver = r.u32();
        if (ver != kFormatVersion) return nullptr;
        uint64_t hash = r.u64();
        if (hash != sourceHash(srcAbs)) return nullptr;
        string mod = r.str();
        if (!moduleName.empty() && mod != moduleName) return nullptr;

        file = new FileNode(mod);
        file->setFromDecl(true);
        if (auto sdk = yux.sdkFile(); sdk && sdk != file) {
            file->setParentScope(sdk);
            file->addImport("yux.core");
        }

        uint32_t nUses = r.u32();
        for (uint32_t i = 0; i < nUses; ++i) {
            FileNode::UseSpec u;
            u.moduleName = r.str();
            u.alias = r.str();
            u.wildcard = r.u8() != 0;
            u.line = r.i32();
            file->addUseSpec(u);
            file->addImport(u.moduleName);
        }

        uint32_t nExt = r.u32();
        for (uint32_t i = 0; i < nExt; ++i) {
            string name = r.str();
            uint32_t np = r.u32();
            vector<TypeInfo> params(np);
            for (uint32_t j = 0; j < np; ++j)
                params[j] = readType(r);
            TypeInfo ret = readType(r);
            bool noRet = r.u8() != 0;
            string cName = r.str();
            SymbolInfo fnSym(SymbolKind::Function, name, ret);
            fnSym.moduleName = file->moduleName();
            fnSym.isExternal = true;
            file->registerSymbol(name, fnSym);
            FnSymbolInfo info{name, file->moduleName(), params, ret};
            info.isExternal = true;
            info.isNoReturn = noRet;
            info.cName = std::move(cName);
            file->registerFnSymbol(name, info);
        }

        uint32_t nFn = r.u32();
        for (uint32_t i = 0; i < nFn; ++i) {
            auto d = readHeaderData(r);
            auto* fn = makeFn(*owner, file, d);
            registerTopFn(file, fn);
        }

        uint32_t nSt = r.u32();
        for (uint32_t i = 0; i < nSt; ++i) {
            string name = r.str();
            int line = r.i32();
            int col = r.i32();
            vector<string> annos, annoArgs;
            readAnnos(r, annos, annoArgs);
            auto tps = readStrings(r);
            auto* decl = owner->make<StructDeclNode>(file, Token(name, static_cast<size_t>(line > 0 ? line : 1)));
            decl->setLocation(line, col);
            decl->setAnnos(std::move(annos), std::move(annoArgs));
            decl->setTypeParams(tps);
            for (auto& tp : tps) {
                decl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
            }
            uint32_t nf = r.u32();
            for (uint32_t j = 0; j < nf; ++j) {
                string fnm = r.str();
                int fl = r.i32();
                int fc = r.i32();
                TypeInfo ty = readType(r);
                bool isVal = r.u8() != 0;
                bool isFrozen = r.u8() != 0;
                auto* field = owner->make<StructFieldNode>(decl, Token(fnm, static_cast<size_t>(fl > 0 ? fl : 1)),
                                                           typeNodeFromInfo(*owner, decl, ty, fl));
                field->setLocation(fl, fc);
                field->setVal(isVal);
                field->setFrozen(isFrozen);
                decl->addField(field);
            }
            file->addStructDecl(decl);
            for (auto field : decl->fields()) {
                string key = name + "." + field->name().getText();
                SymbolInfo fieldSym(SymbolKind::Variable, field->name().getText(), field->getType());
                fieldSym.moduleName = file->moduleName();
                file->registerSymbol(key, fieldSym);
            }

            if (r.u8()) {
                auto* impl = owner->make<StructImplNode>(file, Token(name, static_cast<size_t>(line > 0 ? line : 1)));
                impl->setLocation(line, col);
                impl->setTypeParams(tps);
                impl->setParentScope(file);
                vector<string> iAnnos, iArgs;
                readAnnos(r, iAnnos, iArgs);
                impl->setAnnos(std::move(iAnnos), std::move(iArgs));
                for (auto& tp : tps) {
                    impl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
                }
                uint32_t nsr = r.u32();
                vector<SpecRef> refs(nsr);
                for (uint32_t j = 0; j < nsr; ++j) {
                    refs[j].name = r.str();
                    uint32_t nta = r.u32();
                    refs[j].typeArgs.resize(nta);
                    for (uint32_t k = 0; k < nta; ++k)
                        refs[j].typeArgs[k] = readType(r);
                    refs[j].line = r.i32();
                    refs[j].col = r.i32();
                }
                impl->setSpecRefs(std::move(refs));
                if (r.u8()) {
                    auto dh = readHeaderData(r);
                    auto* dtor = makeFn(*owner, impl, dh);
                    dtor->setParentScope(file);
                    impl->setDestructor(dtor);
                    string destructorName = name;
                    destructorName += ".~";
                    destructorName += name;
                    SymbolInfo destructorSym(SymbolKind::Function, "~" + name, TypeInfo());
                    destructorSym.moduleName = file->moduleName();
                    file->registerSymbol(destructorName, destructorSym);
                    file->registerFnSymbol(destructorName,
                                           {destructorName, file->moduleName(), {TypeInfo(name)}, TypeInfo()});
                }
                uint32_t nm = r.u32();
                for (uint32_t j = 0; j < nm; ++j) {
                    auto md = readHeaderData(r);
                    auto* method = makeFn(*owner, impl, md);
                    impl->addMethod(method);
                    registerMethod(file, impl, method);
                }
                file->addStructImpl(impl);
            }
        }

        uint32_t nEn = r.u32();
        for (uint32_t i = 0; i < nEn; ++i) {
            string name = r.str();
            int line = r.i32();
            int col = r.i32();
            vector<string> annos, annoArgs;
            readAnnos(r, annos, annoArgs);
            auto* en = owner->make<EnumDeclNode>(file, Token(name, static_cast<size_t>(line > 0 ? line : 1)));
            en->setLocation(line, col);
            en->setAnnos(std::move(annos), std::move(annoArgs));
            en->setParentScope(file);
            uint32_t nv = r.u32();
            for (uint32_t j = 0; j < nv; ++j) {
                string vn = r.str();
                auto* v = owner->make<EnumVariantNode>(en, Token(vn, static_cast<size_t>(line > 0 ? line : 1)));
                uint32_t np = r.u32();
                for (uint32_t k = 0; k < np; ++k) {
                    v->addPayloadType(typeNodeFromInfo(*owner, v, readType(r), line));
                }
                en->addVariant(v);
            }
            file->addEnumDecl(en);
        }

        uint32_t nAl = r.u32();
        for (uint32_t i = 0; i < nAl; ++i) {
            string name = r.str();
            int line = r.i32();
            auto tps = readStrings(r);
            TypeInfo tgt = readType(r);
            auto* al = owner->make<AliasDeclNode>(file, Token(name, static_cast<size_t>(line > 0 ? line : 1)),
                                                  typeNodeFromInfo(*owner, nullptr, tgt, line));
            al->setLocation(line, 1);
            al->setTypeParams(tps);
            for (auto& tp : tps) {
                al->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
            }
            file->addAliasDecl(al);
        }

        uint32_t nSp = r.u32();
        for (uint32_t i = 0; i < nSp; ++i) {
            string name = r.str();
            int line = r.i32();
            int col = r.i32();
            vector<string> annos, annoArgs;
            readAnnos(r, annos, annoArgs);
            auto tps = readStrings(r);
            auto* spec = owner->make<SpecDeclNode>(file, Token(name, static_cast<size_t>(line > 0 ? line : 1)));
            spec->setLocation(line, col);
            spec->setAnnos(std::move(annos), std::move(annoArgs));
            spec->setTypeParams(tps);
            for (auto& tp : tps) {
                spec->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
            }
            uint32_t ns = r.u32();
            for (uint32_t j = 0; j < ns; ++j) {
                auto hd = readHeaderData(r);
                spec->addSignature(makeHeader(*owner, spec, hd), nullptr);
            }
            file->addSpecDecl(spec);
        }

        string skeleton = r.str();
        if (!parseSkeletonInto(yux, file, skeleton, srcAbs)) {
            DEBUG_LOG_VAL("  .decl skeleton parse fail", srcAbs);
            delete file;
            return nullptr;
        }

        yux.addFile(file);
        yux.bindModule(file, srcAbs, file->moduleName());
        yux.adoptDeclOwner(std::move(owner));
        DEBUG_LOG_VAL("  loaded .decl", srcAbs);
        return file;
    } catch (const std::exception& e) {
        DEBUG_LOG_VAL("  .decl load fail", srcAbs << " : " << e.what());
        delete file;
        return nullptr;
    } catch (...) {
        DEBUG_LOG_VAL("  .decl load fail", srcAbs);
        delete file;
        return nullptr;
    }
}

} // namespace mod_decl
