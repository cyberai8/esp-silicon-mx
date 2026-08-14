// 屏幕仿真器：在 PC 上跑真实的屏幕代码，把画面存成 PNG。
//
//   ./sim --screen album --script ../scenarios/album.txt --out shots
//
// 场景脚本里可以点、可以滑、可以截图，所以「改一版 UI 看一眼效果」不用烧板子。
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "sim_assets.h"
#include "sim_display.h"
#include "sim_input.h"
#include "sim_screens.h"

namespace {

std::string g_out_dir = ".";
int g_shot_seq = 0;

uint32_t TickCb() {
    static const auto boot = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - boot)
                                     .count());
}

// 推进 LVGL：按真实时间跑，动画/定时器/后台线程的相对节奏和真机一致。
void Pump(int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
}

std::string OutPath(const std::string& name) {
    std::string file = name;
    if (file.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "shot_%02d.png", g_shot_seq);
        file = buf;
    }
    if (file.size() < 4 || file.substr(file.size() - 4) != ".png") {
        file += ".png";
    }
    ++g_shot_seq;
    return g_out_dir + "/" + file;
}

void Shot(const std::string& name, bool round) {
    const std::string path = OutPath(name);
    if (SimDisplay::Screenshot(path.c_str(), round)) {
        printf("[sim] 截图 -> %s\n", path.c_str());
    }
}

// 连拍若干帧拼成一张「胶片图」：看动画、看页面切换用这个比单张截图直观。
void Strip(const std::string& name, int count, int interval_ms, bool round) {
    if (count < 1) {
        count = 1;
    }
    const int w = SimDisplay::Width();
    const int h = SimDisplay::Height();
    const int cols = count < 6 ? count : 6;
    const int rows = (count + cols - 1) / cols;
    const int gap = 8;
    const int sw = cols * w + (cols + 1) * gap;
    const int sh = rows * h + (rows + 1) * gap;
    std::vector<uint8_t> sheet(static_cast<size_t>(sw) * sh * 4, 0);
    for (size_t i = 3; i < sheet.size(); i += 4) {
        sheet[i] = 255;
    }

    std::vector<uint8_t> frame;
    for (int i = 0; i < count; ++i) {
        Pump(interval_ms);
        SimDisplay::CaptureRgba(&frame, round);
        const int cx = gap + (i % cols) * (w + gap);
        const int cy = gap + (i / cols) * (h + gap);
        for (int y = 0; y < h; ++y) {
            uint8_t* dst = &sheet[((static_cast<size_t>(cy + y) * sw) + cx) * 4];
            memcpy(dst, &frame[static_cast<size_t>(y) * w * 4], static_cast<size_t>(w) * 4);
        }
    }
    const std::string path = OutPath(name);
    if (SimDisplay::WritePng(path.c_str(), sheet.data(), sw, sh)) {
        printf("[sim] 胶片图 %d 帧（每 %dms）-> %s\n", count, interval_ms, path.c_str());
    }
}

void Tap(int x, int y) {
    SimInput::MoveTo(x, y);
    Pump(40);
    SimInput::SetPressed(true);
    Pump(80);
    SimInput::SetPressed(false);
    Pump(120);
}

void Swipe(int x1, int y1, int x2, int y2, int ms) {
    const int steps = ms / 20 > 4 ? ms / 20 : 4;
    SimInput::MoveTo(x1, y1);
    Pump(40);
    SimInput::SetPressed(true);
    for (int i = 1; i <= steps; ++i) {
        SimInput::MoveTo(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
        Pump(ms / steps);
    }
    SimInput::SetPressed(false);
    Pump(150);
}

void PrintUsage(const char* argv0) {
    printf("用法: %s [选项]\n", argv0);
    printf("  --screen <名字>   要打开的屏幕（默认 album）\n");
    printf("  --list            列出可仿真的屏幕\n");
    printf("  --script <文件>   场景脚本，见 sim/scenarios/\n");
    printf("  --out <目录>      截图输出目录（默认当前目录）\n");
    printf("  --size <像素>     方屏边长（默认 %d）\n", DISPLAY_WIDTH);
    printf("  --square          不做圆屏遮罩，输出完整矩形\n");
    printf("  --hold <毫秒>     没有脚本时先跑这么久再截一张（默认 1000）\n");
    printf("  --gui             实时窗口（鼠标当手指），需要 libsdl2-dev\n");
    printf("\n场景脚本命令：\n");
    printf("  wait <ms> | shot <名字.png> | tap <x> <y> |\n");
    printf("  swipe <x1> <y1> <x2> <y2> [ms] | press <x> <y> | release | log <文本> |\n");
    printf("  strip <名字.png> <帧数> <每帧ms>   连拍多帧拼一张，用来看动画\n");
}

bool RunScript(const std::string& path, bool round) {
    std::ifstream in(path);
    if (!in) {
        fprintf(stderr, "[sim] 打不开脚本：%s\n", path.c_str());
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd) || cmd.empty() || cmd[0] == '#') {
            continue;
        }
        if (cmd == "wait") {
            int ms = 0;
            ss >> ms;
            Pump(ms);
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            Shot(name, round);
        } else if (cmd == "strip") {
            std::string name;
            int count = 6;
            int interval = 33;
            ss >> name >> count >> interval;
            Strip(name, count, interval, round);
        } else if (cmd == "tap") {
            int x = 0;
            int y = 0;
            ss >> x >> y;
            Tap(x, y);
        } else if (cmd == "swipe") {
            int x1 = 0;
            int y1 = 0;
            int x2 = 0;
            int y2 = 0;
            int ms = 250;
            ss >> x1 >> y1 >> x2 >> y2;
            ss >> ms;
            Swipe(x1, y1, x2, y2, ms);
        } else if (cmd == "press") {
            int x = 0;
            int y = 0;
            ss >> x >> y;
            SimInput::MoveTo(x, y);
            SimInput::SetPressed(true);
            Pump(60);
        } else if (cmd == "release") {
            SimInput::SetPressed(false);
            Pump(120);
        } else if (cmd == "log") {
            std::string rest;
            std::getline(ss, rest);
            printf("[脚本]%s\n", rest.c_str());
        } else {
            fprintf(stderr, "[sim] 第 %d 行不认识的命令: %s\n", lineno, cmd.c_str());
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string screen_name = "album";
    std::string script;
    int size = DISPLAY_WIDTH;
    bool round = true;
    int hold_ms = 1000;
    bool gui = false;
    (void)gui;  // 没装 SDL2 时用不到

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "[sim] %s 缺参数\n", what);
                exit(2);
            }
            return argv[++i];
        };
        if (strcmp(a, "--screen") == 0) {
            screen_name = next(a);
        } else if (strcmp(a, "--script") == 0) {
            script = next(a);
        } else if (strcmp(a, "--out") == 0) {
            g_out_dir = next(a);
        } else if (strcmp(a, "--size") == 0) {
            size = atoi(next(a));
        } else if (strcmp(a, "--hold") == 0) {
            hold_ms = atoi(next(a));
        } else if (strcmp(a, "--square") == 0) {
            round = false;
        } else if (strcmp(a, "--gui") == 0) {
#if LV_USE_SDL
            gui = true;
#else
            fprintf(stderr, "[sim] 这份没编 SDL2，用不了 --gui。装 libsdl2-dev 后重新 cmake。\n");
            return 2;
#endif
        } else if (strcmp(a, "--list") == 0) {
            printf("可仿真的屏幕：\n");
            for (int k = 0; k < SimScreenCount(); ++k) {
                printf("  %s\n", SimScreens()[k].name);
            }
            return 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "[sim] 不认识的参数: %s\n", a);
            PrintUsage(argv[0]);
            return 2;
        }
    }

    const SimScreenEntry* entry = SimFindScreen(screen_name.c_str());
    if (entry == nullptr) {
        fprintf(stderr, "[sim] 没有这个屏幕: %s（用 --list 看清单）\n", screen_name.c_str());
        return 2;
    }
    mkdir(g_out_dir.c_str(), 0755);

    lv_init();
    lv_tick_set_cb(TickCb);
#if LV_USE_SDL
    if (gui) {
        lv_sdl_window_create(size, size);
        lv_sdl_mouse_create();
    } else {
        SimDisplay::Create(size, size);
        SimInput::Init();
    }
#else
    SimDisplay::Create(size, size);
    SimInput::Init();
#endif
    SimAssets::Mount(SIM_ASSETS_DIR);
    // 和设备一致：lv_adapter_display.cc 里把图片缓存设成 512KB。
    lv_image_cache_resize(512 * 1024, true);

    printf("[sim] 屏幕=%s 尺寸=%dx%d 资源=%s SD=%s\n", entry->name, size, size, SIM_ASSETS_DIR,
           SIM_SDCARD_DIR);

    lv_obj_t* scr = entry->create();
    if (scr == nullptr) {
        fprintf(stderr, "[sim] %s 建屏失败\n", entry->name);
        return 1;
    }
    if (entry->lifecycle != nullptr) {
        screen_attach_lifecycle(scr, entry->lifecycle);
    }
    lv_screen_load(scr);

#if LV_USE_SDL
    if (gui) {
        printf("[sim] 实时窗口已打开：鼠标当手指，关窗口退出\n");
        while (true) {
            lv_timer_handler();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
#endif

    if (!script.empty()) {
        if (!RunScript(script, round)) {
            return 1;
        }
    } else {
        Pump(hold_ms);
        Shot("screen.png", round);
    }

    printf("[sim] 堆峰值 %.1f KB\n", static_cast<double>(sim_heap_peak()) / 1024.0);
    return 0;
}
