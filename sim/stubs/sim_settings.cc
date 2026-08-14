#include "settings.h"

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace {

std::mutex g_mutex;
// 命名空间 -> 键值对。进程内共享，析构时写盘。
std::map<std::string, std::map<std::string, std::string>> g_store;
std::map<std::string, bool> g_loaded;

std::string StoreDir() {
    const char* env = getenv("SIM_NVS_DIR");
    return env != nullptr ? env : std::string(SIM_NVS_DIR);
}

std::string StorePath(const std::string& ns) {
    return StoreDir() + "/" + ns + ".txt";
}

void EnsureLoaded(const std::string& ns) {
    if (g_loaded[ns]) {
        return;
    }
    g_loaded[ns] = true;
    std::ifstream in(StorePath(ns));
    if (!in) {
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        g_store[ns][line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void Flush(const std::string& ns) {
    mkdir(StoreDir().c_str(), 0755);
    std::ofstream out(StorePath(ns), std::ios::trunc);
    if (!out) {
        fprintf(stderr, "[sim] 写不了设置文件：%s\n", StorePath(ns).c_str());
        return;
    }
    for (const auto& kv : g_store[ns]) {
        out << kv.first << '=' << kv.second << '\n';
    }
}

}  // namespace

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    std::lock_guard<std::mutex> lock(g_mutex);
    EnsureLoaded(ns_);
}

Settings::~Settings() {
    if (read_write_ && dirty_) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Flush(ns_);
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto ns_it = g_store.find(ns_);
    if (ns_it == g_store.end()) {
        return default_value;
    }
    auto it = ns_it->second.find(key);
    return it == ns_it->second.end() ? default_value : it->second;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (!read_write_) {
        fprintf(stderr, "[sim] Settings(%s) 只读，SetString(%s) 被忽略（真机上同样写不进去）\n",
                ns_.c_str(), key.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_store[ns_][key] = value;
    dirty_ = true;
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    const std::string v = GetString(key, "");
    if (v.empty()) {
        return default_value;
    }
    return static_cast<int32_t>(strtol(v.c_str(), nullptr, 10));
}

void Settings::SetInt(const std::string& key, int32_t value) {
    SetString(key, std::to_string(value));
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    return GetInt(key, default_value ? 1 : 0) != 0;
}

void Settings::SetBool(const std::string& key, bool value) {
    SetInt(key, value ? 1 : 0);
}

void Settings::EraseKey(const std::string& key) {
    if (!read_write_) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_store[ns_].erase(key);
    dirty_ = true;
}

void Settings::EraseAll() {
    if (!read_write_) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_store[ns_].clear();
    dirty_ = true;
}
