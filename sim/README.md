# 屏幕仿真器（PC 上看屏幕效果，不用烧板子）

在电脑上跑**仓库里真实的屏幕代码**（`main/display/screen/...`），把画面存成 PNG。
改一版 UI，几秒钟就能看到效果，不用每次刷机。

- LVGL 用的是 `managed_components/lvgl__lvgl`，**和固件同一个版本（9.3.0）**；
  `sim/lv_conf.h` 里的关键项也是照 `sdkconfig.esp-show` 抄的，
  所以字体、抗锯齿、图片缓存这些渲染行为和真机一致。
- ESP-IDF 那部分（日志、堆、FreeRTOS、esp_timer、NVS、SD 卡、JPEG 解码）由
  `sim/stubs/` 顶替，屏幕代码本身**一行都不用改**。
- 圆屏按内切圆遮罩输出，圆外画成机身黑 —— 截图里看到的就是真机上看得见的范围。
- **零系统依赖**：只要 `cmake` + `gcc`，不需要 SDL、libjpeg、libpng。

## 快速开始

```bash
cd sim
./run.sh                      # 编译 + 跑相册的默认场景 + 出截图
./run.sh clock                # 换个屏
./run.sh settings             # 设置页（亮度/待机/音量/语言/充电）
./run.sh --list               # 看有哪些屏
```

截图落在 `sim/shots/`。也可以直接用可执行文件：

```bash
./build/sim --screen album --out shots --hold 3000        # 静态截一张
./build/sim --screen album --script scenarios/album.txt   # 按脚本点点滑滑
./build/sim --screen clock --size 466                     # 换个尺寸看看排版
./build/sim --screen clock --square                       # 方屏（不做圆遮罩）
./build/sim --help
```

## 场景脚本

纯文本，一行一条命令（`#` 是注释）。看 `scenarios/album.txt`。

| 命令 | 作用 |
| --- | --- |
| `wait <ms>` | 让界面自己跑一会儿（动画、定时器、后台解码都在真实推进） |
| `shot <名字.png>` | 截一张 |
| `strip <名字.png> <帧数> <每帧ms>` | 连拍多帧拼成一张「胶片图」，专门用来看动画和页面切换 |
| `tap <x> <y>` | 点一下 |
| `swipe <x1> <y1> <x2> <y2> [ms]` | 滑动（走真实的手势判定，能测左右滑切换、右滑返回） |
| `press <x> <y>` / `release` | 手动按下 / 松开，测长按 |
| `log <文本>` | 往输出里打一行，方便对照截图 |

坐标是屏上的绝对像素，和代码里的 `lv_obj_align` 一个坐标系。

## 素材放哪

| 仿真里的东西 | 目录 | 说明 |
| --- | --- | --- |
| SD 卡 | `sim/sdcard/` | 照片、音乐丢进去，屏幕代码看到的就是一张 SD 卡（对应真机 `/sdcard`） |
| 图片资源（`A:` 盘） | `main/xingzhi-assets/` | 直接读原始 `.png`；代码里写 `A:ic_app_back.spng`，仿真会自动换成 `.png` 去找 |
| 设置（NVS） | `sim/.nvs/` | 一个命名空间一个文本文件，所以闹钟、语言这些设置在仿真里也能记住 |

这三个路径都能在 cmake 时改：`-DSIM_SDCARD_DIR=...`、`-DSIM_ASSETS_DIR=...`、`-DSIM_NVS_DIR=...`。

## 加一个新屏

三步：

1. `sim/CMakeLists.txt` 的 `SCREEN_SOURCES` 里加上那个屏的 `.cc`。
2. 编一次，看缺什么头文件。缺 ESP-IDF 的就往 `sim/stubs/` 补一个同名头
   （`stubs` 排在 include 路径最前面，会盖掉仓库里的设备版）。
3. `sim/src/sim_screens.cc` 里加一行注册。

补 stub 的原则是**只留 UI 会用到的接口**，别把整条硬件链拉进来。已经有的：

| stub | 顶替 | 行为 |
| --- | --- | --- |
| `esp_log.h` | ESP_LOG* | 打到 stderr，格式接近 idf monitor |
| `esp_heap_caps.h` | heap_caps_* | malloc；`largest_free_block` 返回 7MB（装成有 8MB PSRAM），另外统计峰值 |
| `freertos/*` | 任务、信号量、事件组 | 任务 = pthread，tick = 毫秒；`vTaskDelete(NULL)` 是空操作（任务函数返回即结束） |
| `esp_timer.h` | 周期/单次定时器 | 独立线程回调，和真机「不在 LVGL 线程」一致 |
| `settings.h` | NVS | 落盘到 `sim/.nvs/` |
| `SdCardManager.hpp` | SD 卡 | 挂载点指向 `sim/sdcard/` |
| `esp_jpeg_dec.h` | esp_new_jpeg | 用 LVGL 自带的 TJpgDec 实现（详见下面「已知差异」） |
| `application.h` | Application 单例 | 只留 `PlaySound`，改成打日志 |
| `config.h` | 板级配置 | 只留 `DISPLAY_*` 和板子开关 |
| `home_screen/home_screen.h` 的实现 | 主界面 | `src/sim_home_stub.cc` 里的占位屏，让「返回」有地方去 |

`home_screen.cc` 本身没接进来：它编译期就依赖 27 个子屏 + 网络 + 电量 + 电源键策略，
要仿真整个主界面得先把那几条链也 stub 掉。

## 已知差异（判断效果时要留意的）

1. **JPEG 解码**：真机是 `esp_new_jpeg`（预编译的 xtensa 静态库，PC 上链不了），
   仿真用 LVGL 自带的 TJpgDec。LVGL 那份编译时关掉了整档降采样
   （`JD_USE_SCALE 0`，而改 `managed_components` 会被下次拉依赖冲掉），
   所以仿真是**整图解码后自己做盒式降采样**：尺寸、构图、内存申请路径都和真机一致，
   画质略好一点，PC 上多花几毫秒。
2. **时间是真实时间**，不是虚拟时钟。`wait 500` 就是真等 500ms，动画和后台线程的
   相对节奏和真机接近；但 PC 解码远快于 S3，所以「缩略图多久出来」这类**性能**问题
   仿真看不出来，只能看排版和交互。
3. **`.eaf` 动画和 `.spng`/`.sjpg` 的打包格式**没实现：`A:` 盘是直接读原始 png/jpg。
   用到 `lv_eaf_*` 的屏（比如音乐）接进来前得先补这块。
4. **没有声音、没有网络、没有真实电量**，相关调用都是打日志。
5. `--gui` 实时窗口模式需要 `sudo apt install libsdl2-dev` 再重新 cmake
   （cmake 会自己探测并提示）。**这条路没在本机验证过** —— 装 SDL2 的机器上第一次
   用要是报错，问题大概在 `sim/CMakeLists.txt` 的 SDL 链接那几行。

## 换屏幕参数（以后配新屏用）

- 分辨率：`--size <边长>`（默认取 `sim/stubs/config.h` 里的 `DISPLAY_WIDTH`）。
  屏幕代码里的布局常量多半是照 360 写死的，换尺寸主要用来**验证排版会不会崩**。
- 圆 / 方：默认圆屏遮罩，`--square` 出完整矩形。
- 颜色深度、图片缓存、抗锯齿这些跟着 `sim/lv_conf.h`；**改了板子的 Kconfig 记得同步这里**，
  不然仿真和真机会不一样。
