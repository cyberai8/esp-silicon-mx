#pragma once

#include "lvgl.h"

#include <cstddef>
#include <cstdint>

// show11 分层脸：底图 / 上半脸 / 嘴 / overlay。
// 资源来自固件 mmap 分区（A:*.spng），不读 SD 卡。
class AvatarCompositor {
public:
    struct VisemeEvent {
        uint16_t time_ms = 0;
        uint16_t duration_ms = 0;
        uint8_t id = 0;
    };

    static void Create(lv_obj_t* parent);
    static void Destroy();
    static bool IsCreated();

    // 云端 llm.emotion 原字符串（不是 6 大类）。
    static void SetEmotion(const char* emotion);
    static void SetSpeaking(bool speaking);

    static void ArmUtterance(int index);
    static void LoadVisemeTimeline(int index, const VisemeEvent* events, size_t count);
    static void ResetLipSync();

private:
    AvatarCompositor() = delete;
};
