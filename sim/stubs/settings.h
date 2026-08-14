#ifndef SETTINGS_H
#define SETTINGS_H

// Settings 的 PC 替身：接口和 main/settings.h 一致，存储从 NVS 换成
// sim/.nvs/<命名空间>.txt，所以闹钟、语言、上次播放这些设置在仿真里也能记住。
#include <string>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);
    void EraseKey(const std::string& key);
    void EraseAll();

private:
    std::string ns_;
    bool read_write_ = false;
    bool dirty_ = false;
};

#endif
