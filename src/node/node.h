// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_NODE_H
#define YUX_LANG_NODE_H

#include "types.h"

enum class SymbolKind : u8 {
    Struct,
    Function,
    Variable,
    TypeParam,
    Module, // `use a.b.c` 引入的模块别名；moduleName 存全限定名
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

    SymbolInfo() = default;
    SymbolInfo(SymbolKind k, string n, TypeInfo t = TypeInfo(), bool w = false)
        : kind(k), name(std::move(n)), type(std::move(t)), writeable(w) {
        isPrivate = !this->name.empty() && this->name[0] == '_';
    }

    string getFullName() const {
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

    FnSymbolInfo() = default;
    FnSymbolInfo(string n, string mod, vector<TypeInfo> p, TypeInfo r)
        : name(std::move(n)), moduleName(std::move(mod)), params(std::move(p)), retType(std::move(r)) {
        isPrivate = !this->name.empty() && this->name[0] == '_';
    }

    string getFullName() const {
        if (moduleName.empty()) return name;
        return moduleName + "_" + name;
    }
};

class ScopeNode;

class Node {
protected:
    p<Node> _parent;
    int _line = 0;
    int _col = 0; // 1-based 列号；0 表示未知（合成节点 / 旧路径）

public:
    explicit Node(const p<Node>& parent) : _parent(parent) {
    }

    virtual ~Node() = default;

    [[nodiscard]] virtual string getLocation() const;

    [[nodiscard]] p<Node> parent() const;

    [[nodiscard]] p<ScopeNode> findNearestScope() const;

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
};

class Named {
protected:
    Token _name;

public:
    explicit Named(const Token& name) : _name(name) {
    }

    virtual ~Named() = default;

    [[nodiscard]] virtual Token name() const;
};


class Typed {
public:
    virtual ~Typed() = default;
    [[nodiscard]] virtual TypeInfo getType() const = 0;
};


class Annotated {
protected:
    vector<string> _annos;

public:
    virtual ~Annotated() = default;

    void addAnno(const string& name) { _annos.push_back(name); }
    void setAnnos(vector<string> annos) { _annos = std::move(annos); }
    [[nodiscard]] const vector<string>& annos() const { return _annos; }
    [[nodiscard]] bool hasAnno(const string& name) const {
        for (auto& a : _annos) if (a == name) return true;
        return false;
    }
};


class ScopeNode : public Node {
protected:
    map<string, SymbolInfo> _symbols;
    map<string, vector<FnSymbolInfo>> _fnSymbols;
    p<ScopeNode> _parentScope = nullptr;

public:
    explicit ScopeNode(const p<Node>& parent) : Node(parent) {}
    
    void registerSymbol(const string& name, SymbolInfo info);

    void registerFnSymbol(const string& name, FnSymbolInfo info);

    void setParentScope(const p<ScopeNode>& scope);

    SymbolInfo* lookupSymbol(const string& name);

    FnSymbolInfo* lookupFnSymbol(const string& name);
    
    FnSymbolInfo* lookupFnSymbolWithParams(const string& name, const vector<TypeInfo>& paramTypes);

    void collectFnOverloads(const string& name, vector<FnSymbolInfo*>& out);

    [[nodiscard]] bool hasSymbol(const string& name) const;

    [[nodiscard]] bool hasFnSymbol(const string& name) const;

    [[nodiscard]] const map<string, SymbolInfo>& localSymbols() const;
    [[nodiscard]] const map<string, vector<FnSymbolInfo>>& localFnSymbols() const;
    [[nodiscard]] p<ScopeNode> parentScope() const;

    // 用 resolver 把每个 fn 符号的 params / retType 透明替换（v0.6 类型别名落地）
    template <typename Resolver>
    void normalizeFnSymbolTypes(Resolver resolver) {
        for (auto& [name, overloads] : _fnSymbols) {
            for (auto& fn : overloads) {
                for (auto& p : fn.params) p = resolver(p);
                if (!fn.retType.empty()) fn.retType = resolver(fn.retType);
            }
        }
        for (auto& [name, sym] : _symbols) {
            if (!sym.type.empty()) sym.type = resolver(sym.type);
        }
    }
};


#endif //YUX_LANG_NODE_H
