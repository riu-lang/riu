// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_BUILD_CACHE_H
#define YUX_LANG_BUILD_CACHE_H

#include <string>
#include <filesystem>

using namespace std;

class BuildCache {
public:
    static bool needRecompile(const string& objPath, const string& srcPath);
    static void updateCache(const string& objPath, const string& srcPath);
    
private:
    static string getCachePath(const string& objPath);
    static int64_t getFileTimestamp(const string& filePath);
    static uintmax_t getFileSize(const string& filePath);
};

class SdkLock {
    void* _handle;
    bool _locked;
    
public:
    SdkLock();
    ~SdkLock();
    
    bool tryLock();
    void unlock();
};

#endif
