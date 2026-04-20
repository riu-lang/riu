// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/3.
//

#include "literal_node.h"
#include "file_node.h"

#include <utility>
#include <regex>

LiteralNode::LiteralNode(Token value) :
    Node(nullptr), _value(value) {
}

Token LiteralNode::getValue() const {
    return _value;
}

string LiteralNode::getLocation() const {
    return _value.getText();
}

LiteralNumberNode::LiteralNumberNode(Token value) : LiteralNode(value) {
}

LiteralIntNode::LiteralIntNode(Token value) : LiteralNumberNode(value) {
    const auto v = value.getText();
    // language=RegExp
    static const std::regex type_regex(R"([ui](\d+)$)");
    if (std::smatch match; std::regex_search(v, match, type_regex)) {
        _type = TypeInfo(match.str());
        _hasSuffix = true;
    } else
        _type = TypeInfo("i32");
}

TypeInfo LiteralIntNode::getType() const {
    return _type;
}

LiteralFloatNode::LiteralFloatNode(const Token& value) : LiteralNumberNode(value) {
    const auto v = value.getText();
    // language=RegExp
    static const std::regex type_regex(R"(f(\d+)$)");
    if (std::smatch match; std::regex_search(v, match, type_regex)) {
        _type = TypeInfo(match.str());
        _hasSuffix = true;
    } else
        _type = TypeInfo("f64");
}

TypeInfo LiteralFloatNode::getType() const {
    return _type;
}

LiteralBoolNode::LiteralBoolNode(Token value) : LiteralNode(std::move(value)) {
}

TypeInfo LiteralBoolNode::getType() const {
    return TypeInfo("bool");
}

LiteralObjNode::LiteralObjNode(const p<Node>& parent, const Token& value) : LiteralNode(value) {
    _parent = parent;
}

TypeInfo LiteralObjNode::getType() const {
    auto name = _value.getText();
    auto scope = findNearestScope();
    if (scope) {
        auto sym = scope->lookupSymbol(name);
        if (sym) {
            if (sym->kind == SymbolKind::Function) {
                return TypeInfo("fn() " + sym->type.name);
            }
            return sym->type;
        }
    }
    
    if (name == "_stdout_write") {
        return TypeInfo("fn() ");
    }
    
    YuxError err("Symbol {} not found", name);
    err.setLineNumber(static_cast<int>(_value.getLine()));
    throw err;
}

string LiteralObjNode::getLocation() const {
    return _parent->getLocation() + "." + _value.getText();
}

LiteralNullNode::LiteralNullNode(Token value) : LiteralNode(std::move(value)) {
}

TypeInfo LiteralNullNode::getType() const {
    vector<sp<TypeInfo>> genericArgs;
    genericArgs.push_back(make_shared<TypeInfo>("__nullable"));
    return TypeInfo("Ptr", genericArgs);
}

LiteralStringNode::LiteralStringNode(Token value) : LiteralNode(std::move(value)) {
    auto text = _value.getText();
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        string content = text.substr(1, text.size() - 2);
        for (size_t i = 0; i < content.size(); ) {
            u32 cp = 0;
            if (content[i] == '\\' && i + 1 < content.size()) {
                i++;
                switch (content[i]) {
                    case 'n': cp = '\n'; break;
                    case 'r': cp = '\r'; break;
                    case 't': cp = '\t'; break;
                    case '\\': cp = '\\'; break;
                    case '"': cp = '"'; break;
                    case '0': cp = '\0'; break;
                    case 'x':
                        if (i + 2 < content.size()) {
                            cp = static_cast<u32>(stoi(content.substr(i + 1, 2), nullptr, 16));
                            i += 2;
                        }
                        break;
                    case 'u':
                        if (i + 4 < content.size()) {
                            cp = static_cast<u32>(stoi(content.substr(i + 1, 4), nullptr, 16));
                            i += 4;
                        }
                        break;
                    default: cp = static_cast<u8>(content[i]); break;
                }
                i++;
            } else {
                u8 c = static_cast<u8>(content[i]);
                if (c < 0x80) {
                    cp = c;
                    i++;
                } else if ((c & 0xE0) == 0xC0 && i + 1 < content.size()) {
                    cp = ((c & 0x1F) << 6) | (static_cast<u8>(content[i + 1]) & 0x3F);
                    i += 2;
                } else if ((c & 0xF0) == 0xE0 && i + 2 < content.size()) {
                    cp = ((c & 0x0F) << 12) | ((static_cast<u8>(content[i + 1]) & 0x3F) << 6) | (static_cast<u8>(content[i + 2]) & 0x3F);
                    i += 3;
                } else if ((c & 0xF8) == 0xF0 && i + 3 < content.size()) {
                    cp = ((c & 0x07) << 18) | ((static_cast<u8>(content[i + 1]) & 0x3F) << 12) | ((static_cast<u8>(content[i + 2]) & 0x3F) << 6) | (static_cast<u8>(content[i + 3]) & 0x3F);
                    i += 4;
                } else {
                    cp = c;
                    i++;
                }
            }
            _codePoints.push_back(cp);
        }
    }
}

TypeInfo LiteralStringNode::getType() const {
    return TypeInfo("String");
}
