// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/3/29.
//

#ifndef YUX_LANG_NODE_H
#define YUX_LANG_NODE_H

#include "types.h"

enum class SymbolKind : u8 {
    Struct,
    Function,
    Variable
};

struct SymbolInfo {
    SymbolKind kind;
    string name;
    string moduleName;
    TypeInfo type;
    bool writeable = false;
    
    SymbolInfo() = default;
    SymbolInfo(SymbolKind k, string n, TypeInfo t = TypeInfo(), bool w = false)
        : kind(k), name(std::move(n)), type(std::move(t)), writeable(w) {}
    
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
    
    string getFullName() const {
        if (moduleName.empty()) return name;
        return moduleName + "_" + name;
    }
};

class ScopeNode;

class Node {
protected:
    p<Node> _parent;

public:
    static string getCName(const string& name, const vector<TypeInfo>& paramsType);

    explicit Node(const p<Node>& parent) : _parent(parent) {
    }

    virtual ~Node() = default;

    [[nodiscard]] virtual string getLocation() const;

    [[nodiscard]] p<Node> parent() const;

    [[nodiscard]] p<ScopeNode> findNearestScope() const;
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

    [[nodiscard]] bool hasSymbol(const string& name) const;

    [[nodiscard]] bool hasFnSymbol(const string& name) const;

    [[nodiscard]] const map<string, SymbolInfo>& localSymbols() const;
    [[nodiscard]] const map<string, vector<FnSymbolInfo>>& localFnSymbols() const;
    [[nodiscard]] p<ScopeNode> parentScope() const;
};


#endif //YUX_LANG_NODE_H
