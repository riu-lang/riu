// Copyright (c) 2026. Yin-Jinlong@github

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
    auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
        ftime - filesystem::file_time_type::clock::now() + chrono::system_clock::now()
    );
    return chrono::duration_cast<chrono::seconds>(sctp.time_since_epoch()).count();
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
        std::cout << "Waiting for another SDK compilation to complete..." << std::endl;
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
        ReleaseMutex((HANDLE)_handle);
        CloseHandle((HANDLE)_handle);
        _handle = nullptr;
        _locked = false;
    }
}
