// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/4.
//

#ifndef YUX_LANG_FILE_NODE_H
#define YUX_LANG_FILE_NODE_H

#include "fn_node.h"
#include "node.h"

class FileNode : public ScopeNode {
    vector<p<FnNode>> _functions;
    set<string> _innerFnNames;

public:
    FileNode();
    
    void addFunction(const p<FnNode>& function);

    const vector<p<FnNode>>& getFunctions() const;
    
    bool isInnerFn(const string& name) const { return _innerFnNames.contains(name); }
};

#endif //YUX_LANG_FILE_NODE_H
