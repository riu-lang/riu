// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "literal_node.h"
#include "file_node.h"

#include <regex>
#include <utility>

LiteralNode::LiteralNode(const Token& value) : Node(nullptr), _value(value) {
    _line = static_cast<int>(value.getLine());
}

Token LiteralNode::getValue() const {
    return _value;
}

string LiteralNode::getLocation() const {
    return _value.getText();
}

LiteralNumberNode::LiteralNumberNode(const Token& value) : LiteralNode(value) {}

LiteralIntNode::LiteralIntNode(const Token& value) : LiteralNumberNode(value) {
    const auto& v = value.getText();
    // language=RegExp
    static const std::regex type_regex(R"([ui](\d+|size)$)");
    if (std::smatch match; std::regex_search(v, match, type_regex)) {
        _type = TypeInfo(match.str());
        _hasSuffix = true;
    } else
        _type = TypeInfo("i32");
}

const TypeInfo& LiteralIntNode::getType() const {
    return internType(_type);
}

LiteralFloatNode::LiteralFloatNode(const Token& value) : LiteralNumberNode(value) {
    const auto& v = value.getText();
    // language=RegExp
    static const std::regex type_regex(R"(f(\d+)$)");
    if (std::smatch match; std::regex_search(v, match, type_regex)) {
        _type = TypeInfo(match.str());
        _hasSuffix = true;
    } else
        _type = TypeInfo("f64");
}

const TypeInfo& LiteralFloatNode::getType() const {
    return internType(_type);
}

LiteralBoolNode::LiteralBoolNode(const Token& value) : LiteralNode(value) {}

const TypeInfo& LiteralBoolNode::getType() const {
    return internNamedType("bool");
}

LiteralObjNode::LiteralObjNode(Node* parent, const Token& value) : LiteralNode(value) {
    _parent = parent;
}

const TypeInfo& LiteralObjNode::getType() const {
    auto name = _value.getText();
    auto scope = findNearestScope();
    if (scope) {
        auto sym = scope->lookupSymbol(name);
        if (sym) {
            if (sym->kind == SymbolKind::Function) {
                // 泛型函数也走 FnTag；ExprCallNode::getType 的 isFn() 路径用实参 unify。
                if (auto* fnSym = scope->lookupFnSymbol(name)) {
                    vector<sp<TypeInfo>> paramTypes;
                    paramTypes.reserve(fnSym->params.size());
                    for (auto* p : fnSym->params)
                        paramTypes.push_back(internTypeSpAt(this, *p));
                    sp<TypeInfo> retType = nullptr;
                    if (!fnSym->retTypeRef().empty() && fnSym->retTypeRef().name != "()")
                        retType = internTypeSpAt(this, fnSym->retTypeRef());
                    return internTypeAt(this, TypeInfo(FnTag{}, std::move(paramTypes), std::move(retType)));
                }
                sp<TypeInfo> retType = nullptr;
                if (!sym->type->empty() && sym->type->name != "()") retType = internTypeSpAt(this, *sym->type);
                return internTypeAt(this, TypeInfo(FnTag{}, {}, std::move(retType)));
            }
            // Phase 4a: T& 局部 / 参数 在表达式上下文按值语义出现（自动解引用为 T）；
            // 借用绑定 / 调用借用形参 等需要原始 Ref 类型的场景，在调用点直接读 sym 表
            if (sym->type->isRef()) {
                auto inner = sym->type->refElementType();
                if (inner) return internTypeAt(this, *inner);
            }
            return internTypeAt(this, *sym->type);
        }
    }

    if (name == "_stdout_write") {
        return internTypeAt(this, TypeInfo(FnTag{}, {}, nullptr));
    }

    throw RiuError(static_cast<int>(_value.getLine()), static_cast<int>(_value.getCharPositionInLine()) + 1,
                   ErrorCode::E3030, name);
}

string LiteralObjNode::getLocation() const {
    return _parent->getLocation() + "." + _value.getText();
}

LiteralNullNode::LiteralNullNode(const Token& value) : LiteralNode(value) {}

const TypeInfo& LiteralNullNode::getType() const {
    if (!_type.empty()) return internType(_type);
    return internNamedType("Ptr");
}

LiteralCodePointNode::LiteralCodePointNode(const Token& value) : LiteralNode(value) {
    auto text = _value.getText();
    // text: c'...'
    if (text.size() < 4) return;
    string content = text.substr(2, text.size() - 3);
    if (content.empty()) return;
    if (content[0] == '\\' && content.size() >= 2) {
        switch (content[1]) {
        case 'n':
            _codePoint = '\n';
            break;
        case 'r':
            _codePoint = '\r';
            break;
        case 't':
            _codePoint = '\t';
            break;
        case 'v':
            _codePoint = '\v';
            break;
        case 'b':
            _codePoint = '\b';
            break;
        case '0':
            _codePoint = '\0';
            break;
        case '\\':
            _codePoint = '\\';
            break;
        case '\'':
            _codePoint = '\'';
            break;
        default:
            _codePoint = static_cast<u8>(content[1]);
            break;
        }
    } else {
        u8 c = static_cast<u8>(content[0]);
        if ((c & 0xE0u) == 0xC0u && content.size() >= 2) {
            _codePoint = ((c & 0x1Fu) << 6u) | (static_cast<u8>(content[1]) & 0x3Fu);
        } else if ((c & 0xF0u) == 0xE0u && content.size() >= 3) {
            _codePoint = ((c & 0x0Fu) << 12u) | ((static_cast<u8>(content[1]) & 0x3Fu) << 6u) |
                         (static_cast<u8>(content[2]) & 0x3Fu);
        } else if ((c & 0xF8u) == 0xF0u && content.size() >= 4) {
            _codePoint = ((c & 0x07u) << 18u) | ((static_cast<u8>(content[1]) & 0x3Fu) << 12u) |
                         ((static_cast<u8>(content[2]) & 0x3Fu) << 6u) | (static_cast<u8>(content[3]) & 0x3Fu);
        } else {
            // 单字节 ASCII 或非法 utf-8 起始字节兜底
            _codePoint = c;
        }
    }
}

const TypeInfo& LiteralCodePointNode::getType() const {
    return internNamedType("u32");
}

LiteralStringNode::LiteralStringNode(const Token& value, bool raw) : LiteralNode(value) {
    auto text = _value.getText();
    if (raw && text.size() >= 3 && text.front() == 'r') {
        text = text.substr(1);
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        string content = text.substr(1, text.size() - 2);
        for (size_t i = 0; i < content.size();) {
            u32 cp = 0;
            if (!raw && content[i] == '\\' && i + 1 < content.size()) {
                i++;
                switch (content[i]) {
                case 'n':
                    cp = '\n';
                    break;
                case 'r':
                    cp = '\r';
                    break;
                case 't':
                    cp = '\t';
                    break;
                case '\\':
                    cp = '\\';
                    break;
                case '"':
                    cp = '"';
                    break;
                case '0':
                    cp = '\0';
                    break;
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
                default:
                    cp = static_cast<u8>(content[i]);
                    break;
                }
                i++;
            } else {
                u8 c = static_cast<u8>(content[i]);
                if ((c & 0xE0u) == 0xC0u && i + 1 < content.size()) {
                    cp = ((c & 0x1Fu) << 6u) | (static_cast<u8>(content[i + 1]) & 0x3Fu);
                    i += 2;
                } else if ((c & 0xF0u) == 0xE0u && i + 2 < content.size()) {
                    cp = ((c & 0x0Fu) << 12u) | ((static_cast<u8>(content[i + 1]) & 0x3Fu) << 6u) |
                         (static_cast<u8>(content[i + 2]) & 0x3Fu);
                    i += 3;
                } else if ((c & 0xF8u) == 0xF0u && i + 3 < content.size()) {
                    cp = ((c & 0x07u) << 18u) | ((static_cast<u8>(content[i + 1]) & 0x3Fu) << 12u) |
                         ((static_cast<u8>(content[i + 2]) & 0x3Fu) << 6u) | (static_cast<u8>(content[i + 3]) & 0x3Fu);
                    i += 4;
                } else {
                    // 单字节 ASCII 或非法 utf-8 起始字节兜底
                    cp = c;
                    i++;
                }
            }
            _codePoints.push_back(cp);
        }
    }
}

const TypeInfo& LiteralStringNode::getType() const {
    return internTypeAt(this, TypeInfo("String"));
}

StringTemplateNode::StringTemplateNode(const Token& openTok, vector<string> parts, vector<ExprNode*> interps)
    : LiteralNode(openTok), _parts(std::move(parts)), _interps(std::move(interps)) {}

const TypeInfo& StringTemplateNode::getType() const {
    return internTypeAt(this, TypeInfo("String"));
}
