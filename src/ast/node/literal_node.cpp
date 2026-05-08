// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "literal_node.h"
#include "file_node.h"
#include "analyzer/symbol_suggest.h"

#include <utility>
#include <regex>

LiteralNode::LiteralNode(Token value) :
    Node(nullptr), _value(value) {
    _line = static_cast<int>(value.getLine());
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
            // Phase 4a: T& 局部 / 参数 在表达式上下文按值语义出现（自动解引用为 T）；
            // 借用绑定 / 调用借用形参 等需要原始 Ref 类型的场景，在调用点直接读 sym 表
            if (sym->type.isRef()) {
                auto inner = sym->type.refElementType();
                if (inner) return *inner;
            }
            return sym->type;
        }
    }
    
    if (name == "_stdout_write") {
        return TypeInfo("fn() ");
    }
    
    SymbolSuggest::throwSymbolNotFound(scope,
        static_cast<int>(_value.getLine()),
        static_cast<int>(_value.getCharPositionInLine()) + 1,
        ErrorCode::E3032, name);
}

string LiteralObjNode::getLocation() const {
    return _parent->getLocation() + "." + _value.getText();
}

LiteralNullNode::LiteralNullNode(Token value) : LiteralNode(std::move(value)) {
}

TypeInfo LiteralNullNode::getType() const {
    return TypeInfo("Ptr");
}

LiteralCodePointNode::LiteralCodePointNode(Token value) : LiteralNode(std::move(value)) {
    auto text = _value.getText();
    // text: c'...'
    if (text.size() < 4) return;
    string content = text.substr(2, text.size() - 3);
    if (content.empty()) return;
    if (content[0] == '\\' && content.size() >= 2) {
        switch (content[1]) {
            case 'n': _codePoint = '\n'; break;
            case 'r': _codePoint = '\r'; break;
            case 't': _codePoint = '\t'; break;
            case 'v': _codePoint = '\v'; break;
            case 'b': _codePoint = '\b'; break;
            case '0': _codePoint = '\0'; break;
            case '\\': _codePoint = '\\'; break;
            case '\'': _codePoint = '\''; break;
            default: _codePoint = static_cast<u8>(content[1]); break;
        }
    } else {
        u8 c = static_cast<u8>(content[0]);
        if (c < 0x80) {
            _codePoint = c;
        } else if ((c & 0xE0) == 0xC0 && content.size() >= 2) {
            _codePoint = ((c & 0x1F) << 6) | (static_cast<u8>(content[1]) & 0x3F);
        } else if ((c & 0xF0) == 0xE0 && content.size() >= 3) {
            _codePoint = ((c & 0x0F) << 12) | ((static_cast<u8>(content[1]) & 0x3F) << 6) | (static_cast<u8>(content[2]) & 0x3F);
        } else if ((c & 0xF8) == 0xF0 && content.size() >= 4) {
            _codePoint = ((c & 0x07) << 18) | ((static_cast<u8>(content[1]) & 0x3F) << 12) | ((static_cast<u8>(content[2]) & 0x3F) << 6) | (static_cast<u8>(content[3]) & 0x3F);
        } else {
            _codePoint = c;
        }
    }
}

TypeInfo LiteralCodePointNode::getType() const {
    return TypeInfo("u32");
}

LiteralStringNode::LiteralStringNode(Token value, bool raw) : LiteralNode(std::move(value)) {
    auto text = _value.getText();
    if (raw && text.size() >= 3 && text.front() == 'r') {
        text = text.substr(1);
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        string content = text.substr(1, text.size() - 2);
        for (size_t i = 0; i < content.size(); ) {
            u32 cp = 0;
            if (!raw && content[i] == '\\' && i + 1 < content.size()) {
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

StringTemplateNode::StringTemplateNode(Token openTok, vector<string> parts, vector<p<ExprNode>> interps)
    : LiteralNode(std::move(openTok)),
      _parts(std::move(parts)),
      _interps(std::move(interps)) {
}

TypeInfo StringTemplateNode::getType() const {
    return TypeInfo("String");
}
