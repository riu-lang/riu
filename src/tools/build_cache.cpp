// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "build_cache.h"
#include <fstream>
#include <iostream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

string BuildCache::getCachePath(const string& objPath) {
    return objPath + ".cache";
}

int64_t BuildCache::getFileTimestamp(const string& filePath) {
    if (!filesystem::exists(filePath)) {
        return 0;
    }
    auto ftime = filesystem::last_write_time(filePath);
    // 直接用文件时间的 epoch 计数做确定性比较，不走跨时钟换算
    // (ftime - file_clock::now() + system_clock::now()) 两次 now() 之间细微间隔
    // 会导致同文件两次 getFileTimestamp 返回不同值，缓存反复失效。
    return chrono::duration_cast<chrono::seconds>(ftime.time_since_epoch()).count();
}

uintmax_t BuildCache::getFileSize(const string& filePath) {
    if (!filesystem::exists(filePath)) {
        return 0;
    }
    return filesystem::file_size(filePath);
}

bool BuildCache::needRecompile(const string& objPath, const string& srcPath) {
    if (!filesystem::exists(objPath)) {
        return true;
    }
    
    string cachePath = getCachePath(objPath);
    if (!filesystem::exists(cachePath)) {
        return true;
    }
    
    ifstream file(cachePath);
    if (!file.is_open()) {
        return true;
    }
    
    int64_t cachedTimestamp;
    uintmax_t cachedSize;
    file >> cachedTimestamp >> cachedSize;
    file.close();
    
    int64_t currentTimestamp = getFileTimestamp(srcPath);
    uintmax_t currentSize = getFileSize(srcPath);
    
    return cachedTimestamp != currentTimestamp || cachedSize != currentSize;
}

void BuildCache::updateCache(const string& objPath, const string& srcPath) {
    string cachePath = getCachePath(objPath);
    
    ofstream file(cachePath);
    if (!file.is_open()) {
        return;
    }
    
    file << getFileTimestamp(srcPath) << " " << getFileSize(srcPath);
    file.close();
}

SdkLock::SdkLock() : _handle(nullptr), _locked(false) {
}

SdkLock::~SdkLock() {
    unlock();
}

bool SdkLock::tryLock() {
    if (_locked) {
        return true;
    }
    
    HANDLE mutex = CreateMutexA(nullptr, FALSE, "Global\\YuxSdkCompileMutex");
    if (mutex == nullptr) {
        return false;
    }
    
    DWORD result = WaitForSingleObject(mutex, 0);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        _handle = mutex;
        _locked = true;
        return true;
    }
    
    if (result == WAIT_TIMEOUT) {
        std::cout << "Waiting for another SDK compilation to complete..." << '\n';
        std::cout.flush();
        
        result = WaitForSingleObject(mutex, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            _handle = mutex;
            _locked = true;
            return true;
        }
    }
    
    CloseHandle(mutex);
    return false;
}

void SdkLock::unlock() {
    if (_locked && _handle) {
        ReleaseMutex(static_cast<HANDLE>(_handle));
        CloseHandle(static_cast<HANDLE>(_handle));
        _handle = nullptr;
        _locked = false;
    }
}
