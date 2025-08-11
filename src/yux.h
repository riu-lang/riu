// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/3.
//

#ifndef YUX_LANG_YUX_H
#define YUX_LANG_YUX_H

#include "node/fn_node.h"

enum class Type:u8 {
    Struct,
    Function
};

struct Symbol {
    Type type;
    string name;
    bool writeable = false;
};

struct FnSymbol {
    string name;
    vector<string> params;
    string retType;
};

class Yux {
    using Location = string;

    std::map<Location, Symbol> _symbols;
    std::map<Location, FnSymbol> _fnSymbols;
    std::map<Location, vector<Location>> _members;

    std::map<Location, Symbol> _symbolsInner;
    std::map<Location, FnSymbol> _fnSymbolsInner;
    std::map<Location, vector<Location>> _membersInner;

public:
    static string getCName(const string& name, const vector<string>& parmsType);
    static string getFnSign(const string& name, const vector<string>& parmsType);

    Yux();
    ~Yux();

    void registerSymbol(const Location& loc, Symbol symbol);
    void registerFnSymbol(const Location& loc, FnSymbol symbol);

    void addSymbolMember(const Location& symbol, const Location& member);
    vector<string> getSymbolMember(const Location& symbol);

    [[nodiscard]] const Symbol& getSymbol(const Location& loc) const;
    [[nodiscard]] const FnSymbol& getFnSymbol(const Location& loc) const;
    [[nodiscard]] bool isInnerFn(const Location& loc) const;
    [[nodiscard]] bool hasSymbol(const Location& loc) const;
};


#endif //YUX_LANG_YUX_H
