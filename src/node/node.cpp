// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/7.
//

#include "node.h"

string Node::getCName(const string& name, const vector<TypeInfo>& paramsType)  {
    string res = "fn_" + name + "(";
    for (size_t i = 0; i < paramsType.size(); ++i) {
        res += paramsType[i].getFullName();
        if (i < paramsType.size() - 1) {
            res += ",";
        }
    }
    res += ")";
    return res;
}

string Node::getLocation() const { return ""; }

p<Node> Node::parent() const { return _parent; }

Token Named::name() const {
    return _name;
}

void ScopeNode::registerSymbol(const string& name, SymbolInfo info) {
    _symbols[name] = std::move(info);
}

void ScopeNode::registerFnSymbol(const string& name, FnSymbolInfo info) {
    _fnSymbols[name].push_back(std::move(info));
}

void ScopeNode::setParentScope(const p<ScopeNode>& scope) {
    _parentScope = scope;
}

SymbolInfo* ScopeNode::lookupSymbol(const string& name) {
    auto it = _symbols.find(name);
    if (it != _symbols.end()) {
        return &it->second;
    }
    if (_parentScope) {
        return _parentScope->lookupSymbol(name);
    }
    return nullptr;
}

FnSymbolInfo* ScopeNode::lookupFnSymbol(const string& name) {
    auto it = _fnSymbols.find(name);
    if (it != _fnSymbols.end() && !it->second.empty()) {
        return &it->second[0];
    }
    if (_parentScope) {
        return _parentScope->lookupFnSymbol(name);
    }
    return nullptr;
}

FnSymbolInfo* ScopeNode::lookupFnSymbolWithParams(const string& name, const vector<TypeInfo>& paramTypes) {
    auto it = _fnSymbols.find(name);
    if (it != _fnSymbols.end()) {
        for (auto& fnInfo : it->second) {
            if (fnInfo.params.size() == paramTypes.size()) {
                bool match = true;
                for (size_t i = 0; i < paramTypes.size(); ++i) {
                    if (fnInfo.params[i] == paramTypes[i]) {
                        continue;
                    }
                    if (fnInfo.params[i].isRef()) {
                        auto refElemType = fnInfo.params[i].refElementType();
                        if (refElemType && *refElemType == paramTypes[i]) {
                            continue;
                        }
                    }
                    if (fnInfo.params[i].isPtr() && paramTypes[i].isRef()) {
                        continue;
                    }
                    match = false;
                    break;
                }
                if (match) {
                    return &fnInfo;
                }
            }
        }
    }
    if (_parentScope) {
        return _parentScope->lookupFnSymbolWithParams(name, paramTypes);
    }
    return nullptr;
}

bool ScopeNode::hasSymbol(const string& name) const {
    if (_symbols.contains(name)) {
        return true;
    }
    if (_parentScope) {
        return _parentScope->hasSymbol(name);
    }
    return false;
}

bool ScopeNode::hasFnSymbol(const string& name) const {
    if (_fnSymbols.contains(name)) {
        return true;
    }
    if (_parentScope) {
        return _parentScope->hasFnSymbol(name);
    }
    return false;
}

const map<string, SymbolInfo>& ScopeNode::localSymbols() const { return _symbols; }

const map<string, vector<FnSymbolInfo>>& ScopeNode::localFnSymbols() const { return _fnSymbols; }

p<ScopeNode> ScopeNode::parentScope() const { return _parentScope; }

p<ScopeNode> Node::findNearestScope() const {
    p<Node> current = _parent;
    while (current) {
        if (auto scope = dynamic_cast<ScopeNode*>(current)) {
            return scope;
        }
        current = current->_parent;
    }
    return nullptr;
}

int Node::resolveLineNumber() const {
    return _line;
}
