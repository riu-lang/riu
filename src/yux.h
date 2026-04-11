// Copyright (c) 2026. Yin-Jinlong@github

#ifndef YUX_LANG_YUX_H
#define YUX_LANG_YUX_H

#include "node/file_node.h"

class Yux {
    vector<p<FileNode>> _files;
    p<FileNode> _sdkFile;

public:
    Yux();
    ~Yux();
    
    void addFile(const p<FileNode>& file);
    p<FileNode> createFile(const string& moduleName);
    p<FileNode> createSdkFile();
    
    p<FileNode> sdkFile() const { return _sdkFile; }
    const vector<p<FileNode>>& files() const { return _files; }
};

#endif //YUX_LANG_YUX_H
