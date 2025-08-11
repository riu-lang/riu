// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/3.
//

#include "yux.h"

string Yux::getCName(const string& name, const vector<string>& parmsType) {
    string res = "yux_" + name;
    for (auto p : parmsType) {
        res += "_" + p;
    }
    return res;
}

string Yux::getFnSign(const string& name, const vector<string>& parmsType) {
    string res = "fn " + name + "(";
    for (auto p : parmsType) {
        res += p + ",";
    }
    if (parmsType.size()) {
        res.pop_back();
    }
    res += ")";
    return res;
}


Yux::Yux() {
    const initializer_list<string> TYPES = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
    for (auto t : TYPES) {
        _symbolsInner.insert({t, {Type::Struct, t}});

        vector<string> members;

        for (auto f : TYPES) {
            auto m = t + ".to_" + f;
            members.push_back(m);
            auto fnName = string("to_") + f;
            _symbolsInner.insert({m, {Type::Function, fnName}});
            _fnSymbolsInner.insert({m, {fnName, {}, f}});
        }

        _membersInner.insert({t, members});

        _fnSymbolsInner.insert({"print", {"print", {t}}});
        _fnSymbolsInner.insert({"println", {"println", {t}}});
    }

    _symbolsInner.insert({"print", {Type::Function, "print"}});
    _symbolsInner.insert({"println", {Type::Function, "println"}});
}

Yux::~Yux() {
}

void Yux::registerSymbol(const Location& loc, Symbol symbol) {
    if (_symbols.contains(loc)) {
        throw YuxError(format("Symbol {} already exists", loc));
    }
    _symbols.insert({loc, symbol});
}

void Yux::registerFnSymbol(const Location& loc, FnSymbol symbol) {
    _fnSymbols.insert({loc, symbol});
}

void Yux::addSymbolMember(const Location& symbol, const Location& member) {
    if (!_members.contains(symbol)) {
        _members.insert({symbol, {}});
    }
    _members[symbol].emplace_back(member);
}

vector<string> Yux::getSymbolMember(const Location& symbol) {
    if (_members.contains(symbol)) {
        return _members.at(symbol);
    }
    if (_membersInner.contains(symbol)) {
        return _membersInner.at(symbol);
    }
    return {};
}

const Symbol& Yux::getSymbol(const Location& loc) const {
    if (!_symbols.contains(loc)) {
        if (_symbolsInner.contains(loc))
            return _symbolsInner.at(loc);
        throw YuxError(format("Symbol {} not found", loc));
    }
    return _symbols.at(loc);
}

const FnSymbol& Yux::getFnSymbol(const Location& loc) const {
    if (!_fnSymbols.contains(loc)) {
        if (_fnSymbolsInner.contains(loc))
            return _fnSymbolsInner.at(loc);
        throw YuxError(format("Fun {} not found", loc));
    }
    return _fnSymbols.at(loc);
}

bool Yux::isInnerFn(const Location& loc) const {
    return _fnSymbolsInner.contains(loc);
}

bool Yux::hasSymbol(const Location& loc) const {
    return _symbols.contains(loc) || _symbolsInner.contains(loc);
}
