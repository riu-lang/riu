// Copyright (c) 2026. Yin-Jinlong@github

#ifndef YUX_LANG_BUILD_CACHE_H
#define YUX_LANG_BUILD_CACHE_H

#include <string>
#include <map>
#include <filesystem>

using namespace std;

struct FileCacheInfo {
    string path;
    int64_t timestamp;
    uintmax_t size;
    
    FileCacheInfo() : timestamp(0), size(0) {}
    FileCacheInfo(const string& p, int64_t ts, uintmax_t sz) 
        : path(p), timestamp(ts), size(sz) {}
};

class BuildCache {
    string _cachePath;
    map<string, FileCacheInfo> _cache;
    
public:
    BuildCache(const string& buildDir);
    
    void load();
    void save();
    
    bool needRecompile(const string& filePath);
    void updateCache(const string& filePath);
    
private:
    FileCacheInfo getFileCacheInfo(const string& filePath);
};

#endif
