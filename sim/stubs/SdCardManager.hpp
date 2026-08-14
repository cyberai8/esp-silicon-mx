#pragma once

// SD 卡管理器的 PC 替身：挂载点指向本地目录（CMake 里的 SIM_SDCARD_DIR，
// 默认 sim/sdcard）。往那个目录里丢照片/音乐，屏幕代码看到的就是一张 SD 卡。
#include <sys/stat.h>

#include <cstdio>

class SdCardManager {
public:
    static constexpr const char* kMountPoint = SIM_SDCARD_DIR;

    static SdCardManager& GetInstance() {
        static SdCardManager inst;
        return inst;
    }

    bool Mount() {
        struct stat st = {};
        mounted_ = stat(kMountPoint, &st) == 0 && S_ISDIR(st.st_mode);
        if (!mounted_) {
            fprintf(stderr, "[sim] SD 目录不存在：%s\n", kMountPoint);
        }
        return mounted_;
    }

    void Unmount() {
        mounted_ = false;
    }

    bool IsMounted() const {
        return mounted_;
    }

    bool HasCard() const {
        return true;
    }

    const char* GetMountPoint() const {
        return kMountPoint;
    }

private:
    SdCardManager() = default;
    bool mounted_ = false;
};
