#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// MusicScreen
//
// 音乐播放器界面：
//   - BOARD_ESP_VOCAT：播放 SD 卡本地音乐（扫描 /sdcard 最多 5 级目录），
//     不使用蓝牙；实现见 music_screen_sd.*。
//   - 外置蓝牙板：进入页面时通过 SimpleUart 把蓝牙模块切到模式三
//     （AT+RX=1 / AT+MODE=3），切换后注册 UART RX 回调用于解析手机
//     回传的 JSON 数据流。
//   - ESP32-S31-Korvo-1：使用片上 Classic BT/A2DP Sink，手机直接连接本机
//     播放音乐，并通过 AVRCP Metadata 显示歌名 / 歌手 / 专辑。
//   - 外置蓝牙模块 JSON：
//       {"type":"song",  "data":"人间共鸣-李健"}   -> 显示成歌名标题
//       {"type":"lyrics","data":"人间共鸣 - 李健"}  -> 显示成当前歌词
//   - 外置蓝牙模块按钮通过 AT 命令控制播放：
//       上一曲 AT+PREV / 下一曲 AT+NEXT / 播放暂停 AT+PP
//       音量加 AT+VOLUP / 音量减 AT+VOLDOWN
//     S31 片上蓝牙按钮通过 AVRCP passthrough 控制手机播放。
// ---------------------------------------------------------------------------
class MusicScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
