// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "node.h"
#include "file_node.h"

string Node::getLocation() const {
    return "";
}

Node* Node::parent() const {
    return _parent;
}

Token Named::name() const {
    return _name;
}

void ScopeNode::registerSymbol(const string& name, SymbolInfo info) {
    _symbols[name] = std::move(info);
}

void ScopeNode::eraseSymbol(const string& name) {
    _symbols.erase(name);
}

void ScopeNode::registerFnSymbol(const string& name, FnSymbolInfo info) {
    _fnSymbols[name].push_back(std::move(info));
}

void ScopeNode::setParentScope(ScopeNode* scope) {
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

bool ScopeNode::matchFnParams(const FnSymbolInfo& fnInfo, const vector<TypeInfo>& paramTypes) const {
    if (fnInfo.params.size() != paramTypes.size()) return false;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (fnInfo.params[i] == paramTypes[i]) continue;
        if (fnInfo.params[i].isRef()) {
            auto refElemType = fnInfo.params[i].refElementType();
            if (refElemType && *refElemType == paramTypes[i]) continue;
        }
        if (fnInfo.params[i].isPtr() && paramTypes[i].isRef()) continue;
        // Phase 7c (DRAFT §9.3): extern 边界 Ptr 形参接受堆句柄类型自动转换
        if (fnInfo.isExternal && fnInfo.params[i].isPtr() && paramTypes[i].isRcHandle()) {
            continue;
        }
        // Nullable<T> 形参接受 T 值实参（自动包装 T → {_has=true, _value=T}）
        if (fnInfo.params[i].isNullable()) {
            auto inner = fnInfo.params[i].nullableInnerType();
            if (inner && *inner == paramTypes[i]) continue;
        }
        return false;
    }
    return true;
}

FnSymbolInfo* ScopeNode::lookupFnSymbolWithParams(const string& name, const vector<TypeInfo>& paramTypes) {
    auto it = _fnSymbols.find(name);
    if (it != _fnSymbols.end()) {
        for (auto& fnInfo : it->second) {
            if (matchFnParams(fnInfo, paramTypes)) {
                return &fnInfo;
            }
        }
    }
    if (_parentScope) {
        return _parentScope->lookupFnSymbolWithParams(name, paramTypes);
    }
    return nullptr;
}

void ScopeNode::collectFnOverloads(const string& name, vector<FnSymbolInfo*>& out) {
    auto it = _fnSymbols.find(name);
    if (it != _fnSymbols.end()) {
        for (auto& fn : it->second) {
            out.push_back(&fn);
        }
    }
    if (_parentScope) {
        _parentScope->collectFnOverloads(name, out);
    }
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

const map<string, SymbolInfo>& ScopeNode::localSymbols() const {
    return _symbols;
}

const map<string, vector<FnSymbolInfo>>& ScopeNode::localFnSymbols() const {
    return _fnSymbols;
}

ScopeNode* ScopeNode::parentScope() const {
    return _parentScope;
}

ScopeNode* Node::findNearestScope() const {
    Node* current = _parent;
    while (current) {
        if (auto scope = dynamic_cast<ScopeNode*>(current)) {
            return scope;
        }
        current = current->_parent;
    }
    return nullptr;
}

FileNode* Node::enclosingFile() const {
    const Node* cur = this;
    while (cur) {
        if (auto f = dynamic_cast<const FileNode*>(cur)) return const_cast<FileNode*>(f);
        cur = cur->parent();
    }
    auto scope = findNearestScope();
    while (scope) {
        if (auto f = dynamic_cast<FileNode*>(scope)) return f;
        scope = scope->parentScope();
    }
    return nullptr;
}

void attachDiagFile(YuxError& e, const Node* n) {
    if (!e.file().empty() || !n) return;
    auto* f = n->enclosingFile();
    if (f && !f->sourcePath().empty()) e.setFile(f->sourcePath());
}

int Node::resolveLineNumber() const {
    return _line;
}

int Node::resolveColumn() const {
    return _col;
}

SourceLocation Node::resolveLocation() const {
    return {resolveLineNumber(), resolveColumn()};
}
