#include "sim_assets.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "lvgl.h"

namespace SimAssets {
namespace {

lv_fs_drv_t s_drv;
std::string s_dir;

// 设备上 spiffs_create_partition_assets 会把 png 打包成 spng、jpg 打包成 sjpg。
// 仿真里没有这层打包，把后缀换回来去找源文件即可；LVGL 的 lodepng 解码器是按
// 文件头的魔数认图的，不看后缀，所以「叫 .spng 的 png」它也能解。
std::string MapName(const char* name) {
    std::string path = s_dir;
    path += '/';
    path += name;
    const size_t dot = path.rfind('.');
    if (dot != std::string::npos) {
        const std::string ext = path.substr(dot);
        if (ext == ".spng") {
            path.replace(dot, ext.size(), ".png");
        } else if (ext == ".sjpg") {
            path.replace(dot, ext.size(), ".jpg");
        }
    }
    return path;
}

void* OpenCb(lv_fs_drv_t* /*drv*/, const char* path, lv_fs_mode_t mode) {
    if (mode != LV_FS_MODE_RD) {
        return nullptr;  // 资源盘只读
    }
    const std::string real = MapName(path);
    FILE* fp = fopen(real.c_str(), "rb");
    if (fp == nullptr) {
        LV_LOG_WARN("sim assets: missing %s", real.c_str());
    }
    return fp;
}

lv_fs_res_t CloseCb(lv_fs_drv_t* /*drv*/, void* file_p) {
    fclose(static_cast<FILE*>(file_p));
    return LV_FS_RES_OK;
}

lv_fs_res_t ReadCb(lv_fs_drv_t* /*drv*/, void* file_p, void* buf, uint32_t btr, uint32_t* br) {
    *br = static_cast<uint32_t>(fread(buf, 1, btr, static_cast<FILE*>(file_p)));
    return LV_FS_RES_OK;
}

lv_fs_res_t SeekCb(lv_fs_drv_t* /*drv*/, void* file_p, uint32_t pos, lv_fs_whence_t whence) {
    int origin = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) {
        origin = SEEK_CUR;
    } else if (whence == LV_FS_SEEK_END) {
        origin = SEEK_END;
    }
    return fseek(static_cast<FILE*>(file_p), static_cast<long>(pos), origin) == 0
               ? LV_FS_RES_OK
               : LV_FS_RES_UNKNOWN;
}

lv_fs_res_t TellCb(lv_fs_drv_t* /*drv*/, void* file_p, uint32_t* pos_p) {
    *pos_p = static_cast<uint32_t>(ftell(static_cast<FILE*>(file_p)));
    return LV_FS_RES_OK;
}

}  // namespace

void Mount(const char* dir) {
    s_dir = dir;
    lv_fs_drv_init(&s_drv);
    s_drv.letter = 'A';
    s_drv.cache_size = 0;
    s_drv.open_cb = OpenCb;
    s_drv.close_cb = CloseCb;
    s_drv.read_cb = ReadCb;
    s_drv.seek_cb = SeekCb;
    s_drv.tell_cb = TellCb;
    lv_fs_drv_register(&s_drv);
}

}  // namespace SimAssets
