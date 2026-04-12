// Copyright (c) 2026. Yin-Jinlong@github

#include "build_cache.h"
#include <fstream>
#include <iostream>

BuildCache::BuildCache(const string& buildDir) {
    _cachePath = buildDir + "/build.cache";
}

void BuildCache::load() {
    ifstream file(_cachePath);
    if (!file.is_open()) {
        return;
    }
    
    string line1, line2;
    while (getline(file, line1) && getline(file, line2)) {
        if (line1.empty()) continue;
        
        size_t spacePos = line2.find(' ');
        if (spacePos == string::npos) continue;
        
        int64_t timestamp = stoll(line2.substr(0, spacePos));
        uintmax_t size = stoull(line2.substr(spacePos + 1));
        
        _cache[line1] = FileCacheInfo(line1, timestamp, size);
    }
    
    file.close();
}

void BuildCache::save() {
    ofstream file(_cachePath);
    if (!file.is_open()) {
        return;
    }
    
    for (const auto& pair : _cache) {
        file << pair.second.path << "\n";
        file << pair.second.timestamp << " " << pair.second.size << "\n";
    }
    
    file.close();
}

bool BuildCache::needRecompile(const string& filePath) {
    auto it = _cache.find(filePath);
    if (it == _cache.end()) {
        return true;
    }
    
    auto currentInfo = getFileCacheInfo(filePath);
    return currentInfo.timestamp != it->second.timestamp || 
           currentInfo.size != it->second.size;
}

void BuildCache::updateCache(const string& filePath) {
    _cache[filePath] = getFileCacheInfo(filePath);
}

FileCacheInfo BuildCache::getFileCacheInfo(const string& filePath) {
    FileCacheInfo info;
    info.path = filePath;
    
    if (!filesystem::exists(filePath)) {
        return info;
    }
    
    auto ftime = filesystem::last_write_time(filePath);
    auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
        ftime - filesystem::file_time_type::clock::now() + chrono::system_clock::now()
    );
    info.timestamp = chrono::duration_cast<chrono::seconds>(sctp.time_since_epoch()).count();
    info.size = filesystem::file_size(filePath);
    
    return info;
}
