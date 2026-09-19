// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_NODE_H
#define RIU_LANG_NODE_H

#include "types.h"

enum class SymbolKind : u8 {
    Struct,
    Function,
    Variable,
    TypeParam,
    Module,  // `use a.b.c` 引入的模块别名；moduleName 存全限定名
    Package, // `use a.b` 中 a.b 是目录时引入的包别名；moduleName 存点分路径
};

struct SymbolInfo {
    SymbolKind kind;
    string name;
    string moduleName;
    TypeInfo type;
    bool writeable = false;
    bool isPrivate = false;
    bool isExternal = false;
    // DRAFT-const-mut §3：true 表示该符号是 `cval`（局部或全局编译期常量）。
    // 仅供 const_mut_checker §3.3 区分 cval / val / 参数；codegen 不读。
    bool isConst = false;
    // DRAFT-const-mut §5：true 表示该符号是 `#Frozen` 参数（或由 §5.4 传染来的本地绑定）。
    // 重赋走 writeable=false → E3093；字段写 → E3106；传给可写形参 → E3107；
    // 仅 copy_of 可作为脱 const 出口。
    bool isFrozen = false;

    SymbolInfo() = default;
    SymbolInfo(SymbolKind k, string n, TypeInfo t = TypeInfo(), bool w = false)
        : kind(k), name(std::move(n)), type(std::move(t)), writeable(w) {
        isPrivate = !this->name.empty() && this->name[0] == '_';
    }

    [[nodiscard]] string getFullName() const {
        if (moduleName.empty()) return name;
        return moduleName + "_" + name;
    }
};

struct FnSymbolInfo {
    string name;
    string moduleName;
    vector<TypeInfo> params;
    TypeInfo retType;
    bool isPrivate = false;
    bool isExternal = false;
    // DRAFT-错误.md §8.3 / spec §11.5.1：该函数声明带 `#NoReturn` 注解，
    // 调用点视为流终止节点（用于 §4.9.1.4 / §4.9.3.5 arm 排除及 E7014 检查）。
    bool isNoReturn = false;
    // DRAFT-const-mut §4：`#Const fn` 标记——函数体内不得写 $/参数字段、写非 cval 全局、
    // 调非 #Const fn。供 const_mut_checker §4.2 在调用点判定。
    bool isConst = false;
    // DRAFT-错误.md [#3.A]：`T ! E` 的错误类型 E 的 getFullName。空字符串表示
    // 该函数未声明错误通道。比较 / 透传 / mangle 用完整写法（`Box<String>` ≠ `Box<i32>`）。
    string fallibleErrType;
    // `#CName("link_symbol")`：extern fn 的链接时符号名（§6.6）。
    // 非空时 codegen 用此字符串声明 LLVM Function 而非 riu 声明名。
    // 空字符串表示使用 riu 声明名（当前行为）。
    string cName;
    // extern 声明行（1-based）；非 extern 为 0。SemaPass 白名单诊断用。
    int declLine = 0;

    FnSymbolInfo() = default;
    FnSymbolInfo(string n, string mod, vector<TypeInfo> p, TypeInfo r)
        : name(std::move(n)), moduleName(std::move(mod)), params(std::move(p)), retType(std::move(r)) {
        // BUG4 fix: struct 方法的 name 格式为 "StructName.method"，
        // 取最后一个 "." 之后的部分来判断 _ 前缀。
        auto dotPos = this->name.rfind('.');
        string baseName = (dotPos != string::npos) ? this->name.substr(dotPos + 1) : this->name;
        isPrivate = !baseName.empty() && baseName[0] == '_';
    }

    [[nodiscard]] string getFullName() const {
        if (moduleName.empty()) return name;
        return moduleName + "_" + name;
    }

    // extern 的链接名：`#CName` 否则 riu 声明名。非 extern 为空。
    [[nodiscard]] string externLinkName() const {
        if (!isExternal) return {};
        return cName.empty() ? name : cName;
    }

    // 形参 + 返回是否同一 C 签名（§6.6.1：同链接名须同签名）。
    [[nodiscard]] bool sameExternCSig(const FnSymbolInfo& other) const {
        if (!(retType == other.retType)) return false;
        if (params.size() != other.params.size()) return false;
        for (size_t i = 0; i < params.size(); ++i) {
            if (!(params[i] == other.params[i])) return false;
        }
        return true;
    }
};

// Phase 2.3 Sema/Codegen 拆分：表达式经语义检查后定位到的符号引用。
// 当前 codegen 在 compileLiteralExpr / compileCallExpr 等处现场 lookupSymbol /
// lookupFnSymbol，把同一个表达式的符号查表反复跑。SemaPass 抽出后这一步将提前完成，
// 结果挂在 ExprNode::_resolvedSymbol；codegen 改成直接读，无需再次入 scope。
//
// 存放策略：
// - 只持指针，不复制。ScopeNode 持有的 SymbolInfo / FnSymbolInfo 与 AST 同生命周期，
//   在编译流程内地址稳定（ScopeNode 用 map 而非 vector，rehash 不影响 value 地址）。
// - var / fn 互斥：一个表达式要么解析到变量符号，要么解析到函数符号；都为空表示
//   "尚未解析"（由外层 optional 区分 "未写入" vs "解析为 null"）。
struct ResolvedSymbol {
    SymbolInfo* var = nullptr;
    FnSymbolInfo* fn = nullptr;

    [[nodiscard]] bool isVar() const { return var != nullptr; }
    [[nodiscard]] bool isFn() const { return fn != nullptr; }
};

class ScopeNode;

class FileNode;

class AliasDeclNode;

class Node {
protected:
    Node* _parent;
    int _line = 0;
    int _col = 0; // 1-based 列号；0 表示未知（合成节点 / 旧路径）
    // 对应 parse 规则的 default-channel token 下标；-1 表示未知（合成节点）。
    // formatter 用它做 raw 回退，不依赖 ParserRuleContext 指针。
    int _tokStart = -1;
    int _tokStop = -1;

public:
    explicit Node(Node* parent) : _parent(parent) {}

    virtual ~Node() = default;

    [[nodiscard]] virtual string getLocation() const;

    [[nodiscard]] Node* parent() const;

    [[nodiscard]] ScopeNode* findNearestScope() const;

    // 沿 parent / parentScope 找到所属 FileNode；合成节点可能为空。
    // spec 默认体 fall-through 只临时改 parentScope，parent 仍指向 spec 文件。
    [[nodiscard]] FileNode* enclosingFile() const;

    void setLineNumber(int line) { _line = line; }

    void setColumn(int col) { _col = col; }

    void setLocation(int line, int col) {
        _line = line;
        _col = col;
    }

    void setLocation(SourceLocation loc) {
        _line = loc.line;
        _col = loc.col;
    }

    [[nodiscard]] int getLineNumber() const { return _line; }

    [[nodiscard]] int getColumn() const { return _col; }

    [[nodiscard]] SourceLocation location() const { return {_line, _col}; }

    [[nodiscard]] virtual int resolveLineNumber() const;

    [[nodiscard]] virtual int resolveColumn() const;

    [[nodiscard]] virtual SourceLocation resolveLocation() const;

    void setTokenRange(int start, int stop) {
        _tokStart = start;
        _tokStop = stop;
    }
    [[nodiscard]] int tokenStart() const { return _tokStart; }
    [[nodiscard]] int tokenStop() const { return _tokStop; }
};

// 把出错节点所属文件路径写入 RiuError（已有路径则不动）。
void attachDiagFile(RiuError& e, const Node* n);

// 按节点所属 FileNode 的 Riu intern；无 Riu 则走 TLS / fallback。
// SDK FileNode 跨多次 check 复用，不能把类型指针挂到用户 Riu 的 intern 表上。
[[nodiscard]] const TypeInfo& internTypeAt(const Node* n, TypeInfo t);
[[nodiscard]] sp<TypeInfo> internTypeSpAt(const Node* n, TypeInfo t);

class Named {
protected:
    Token _name;

public:
    explicit Named(Token name) : _name(std::move(name)) {}

    virtual ~Named() = default;

    [[nodiscard]] virtual const Token& name() const;
};

class Typed {
public:
    virtual ~Typed() = default;
    [[nodiscard]] virtual const TypeInfo& getType() const = 0;
};

class Annotated {
protected:
    vector<string> _annos;
    // 与 _annos 同长的参数槽：buildAnno 可选单参数糖（spec §11.1.1.1）。
    // 空字符串表示零参注解。当前仅 #Fallible(E) 使用单参形态。
    vector<string> _annoArgs;

public:
    virtual ~Annotated() = default;

    void addAnno(const string& name) {
        _annos.push_back(name);
        _annoArgs.emplace_back();
    }
    void addAnno(const string& name, const string& arg) {
        _annos.push_back(name);
        _annoArgs.push_back(arg);
    }
    void setAnnos(vector<string> annos) {
        _annoArgs.assign(annos.size(), "");
        _annos = std::move(annos);
    }
    void setAnnos(vector<string> annos, vector<string> args) {
        _annos = std::move(annos);
        _annoArgs = std::move(args);
    }
    [[nodiscard]] const vector<string>& annos() const { return _annos; }
    [[nodiscard]] const vector<string>& annoArgs() const { return _annoArgs; }
    [[nodiscard]] bool hasAnno(const string& name) const {
        for (auto& a : _annos)
            if (a == name) return true;
        return false;
    }
    // 返回注解的单参数糖（spec §11.1.1.1）。未找到返回 nullopt；找到但零参返回空字符串包装。
    [[nodiscard]] std::optional<string> getAnnoArg(const string& name) const {
        for (size_t i = 0; i < _annos.size(); ++i) {
            if (_annos[i] == name) return _annoArgs[i];
        }
        return std::nullopt;
    }
};

class ScopeNode : public Node {
protected:
    map<string, SymbolInfo> _symbols;
    map<string, vector<FnSymbolInfo>> _fnSymbols;
    map<string, AliasDeclNode*> _localAliases;
    ScopeNode* _parentScope = nullptr;

    // 参数匹配辅助：检查 fnInfo.params 是否与 paramTypes 兼容
    [[nodiscard]] bool matchFnParams(const FnSymbolInfo& fnInfo, const vector<TypeInfo>& paramTypes) const;

public:
    explicit ScopeNode(Node* parent) : Node(parent) {}

    void registerSymbol(const string& name, SymbolInfo info);

    void eraseSymbol(const string& name);

    void registerFnSymbol(const string& name, FnSymbolInfo info);

    void setParentScope(ScopeNode* scope);

    virtual SymbolInfo* lookupSymbol(const string& name);

    virtual FnSymbolInfo* lookupFnSymbol(const string& name);

    virtual FnSymbolInfo* lookupFnSymbolWithParams(const string& name, const vector<TypeInfo>& paramTypes);

    virtual void collectFnOverloads(const string& name, vector<FnSymbolInfo*>& out);

    [[nodiscard]] bool hasSymbol(const string& name) const;

    [[nodiscard]] bool hasFnSymbol(const string& name) const;

    [[nodiscard]] const map<string, SymbolInfo>& localSymbols() const;
    [[nodiscard]] const map<string, vector<FnSymbolInfo>>& localFnSymbols() const;
    [[nodiscard]] ScopeNode* parentScope() const;

    // 块 / struct 内 `type Name = T`（文件顶层仍走 FileNode::_aliasMap）
    void addLocalAlias(AliasDeclNode* alias);
    [[nodiscard]] AliasDeclNode* localAlias(const string& name) const;
    [[nodiscard]] const map<string, AliasDeclNode*>& localAliases() const { return _localAliases; }
    void copyLocalAliasesFrom(const ScopeNode* from);

    // 用 resolver 把每个 fn 符号的 params / retType 透明替换（v0.6 类型别名落地）
    template <typename Resolver>
    void normalizeFnSymbolTypes(Resolver resolver) {
        for (auto& [name, overloads] : _fnSymbols) {
            for (auto& fn : overloads) {
                for (auto& p : fn.params)
                    p = resolver(p);
                if (!fn.retType.empty()) fn.retType = resolver(fn.retType);
            }
        }
        for (auto& [name, sym] : _symbols) {
            if (!sym.type.empty()) sym.type = resolver(sym.type);
        }
    }
};

#endif // RIU_LANG_NODE_H
