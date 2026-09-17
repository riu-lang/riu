// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/rd/parser.h"

#include <vector>

namespace rd {
namespace {

std::string_view sliceTokens(const Token& first, const Token& last) {
    if (first.text.empty()) return last.text;
    if (last.text.empty()) return first.text;
    const auto* begin = first.text.data();
    const auto* end = last.text.data() + last.text.size();
    if (end < begin) return first.text;
    return {begin, static_cast<std::size_t>(end - begin)};
}

Pos span(const Pos& start, const Pos& last) {
    Pos p = start;
    p.end = last.end;
    return p;
}

} // namespace

Parser::Parser(std::string_view src) : scanner_(src) {}

void Parser::next() {
    if (has_peek_) {
        tok_ = peek_tok_;
        has_peek_ = false;
        return;
    }
    tok_ = scanner_.next();
}

const Token& Parser::peek() {
    if (!has_peek_) {
        peek_tok_ = scanner_.next();
        has_peek_ = true;
    }
    return peek_tok_;
}

bool Parser::peekIs(Kind k) {
    return peek().kind == k;
}

bool Parser::eat(Kind k) {
    if (!at(k)) return false;
    next();
    return true;
}

void Parser::skipLineEnds() {
    while (eat(Kind::LineEnd)) {
    }
}

void Parser::skipRest() {
    while (!at(Kind::Eof))
        next();
}

void Parser::skipToLineEnd() {
    while (!at(Kind::LineEnd) && !at(Kind::Eof))
        next();
    eat(Kind::LineEnd);
}

NodeId Parser::parseUse() {
    const Pos start = tok_.pos;
    next(); // Use

    if (!at(Kind::ID)) {
        skipToLineEnd();
        return ast_.add(NodeKind::Use, start);
    }

    const Token first = tok_;
    Token last = tok_;
    next();
    while (at(Kind::SymbolDot) && peekIs(Kind::ID)) {
        next(); // Dot
        last = tok_;
        next(); // ID
    }
    if (at(Kind::SymbolDot) && peekIs(Kind::SymbolMul)) {
        next(); // Dot
        last = tok_;
        next(); // *
    }

    const std::string_view value = sliceTokens(first, last);
    if (!at(Kind::LineEnd) && !at(Kind::Eof))
        skipToLineEnd();
    else
        eat(Kind::LineEnd);

    return ast_.add(NodeKind::Use, span(start, last.pos), value);
}

FlatAst Parser::parse() {
    next();
    skipLineEnds();

    std::vector<NodeId> items;
    while (at(Kind::Use)) {
        items.push_back(parseUse());
        skipLineEnds();
    }
    skipRest();

    Pos file_pos;
    file_pos.end = tok_.pos.end;
    const NodeId root = ast_.add(NodeKind::Program, file_pos, {}, items);
    ast_.setRoot(root);
    return ast_;
}

FlatAst parseProgram(std::string_view src) {
    Parser p(src);
    return p.parse();
}

} // namespace rd
