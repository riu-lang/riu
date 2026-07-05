// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "global_var_node.h"

TypeInfo GlobalVarNode::getType() const {
    return _type ? _type->getType() : _value->getType();
}
