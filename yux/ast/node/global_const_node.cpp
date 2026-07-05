// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "global_const_node.h"

p<TypeNode> GlobalConstNode::typeNode() const {
    return _type;
}

p<ExprNode> GlobalConstNode::value() const {
    return _value;
}

bool GlobalConstNode::isPrivate() const {
    return _isPrivate;
}

TypeInfo GlobalConstNode::getType() const {
    return _type ? _type->getType() : _value->getType();
}
