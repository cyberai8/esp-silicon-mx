# ESP-VoCat 板级移植执行文档

目标：把 `/home/qmx/esp-project-sb/vocat-3/main/boards/esp-vocat` 的硬件板级移植到当前工程，同时继续使用当前工程的应用、对话、网络和 LVGL UI 体系。

当前硬件是 ESP32-S3 + 360x360 QSPI 圆屏。移植要分阶段完成：先硬件闭环，再打开功能，最后做 360 圆屏 UI 裁剪。不要直接整包搬旧工程 UI，否则会引入大量 Brookesia/Vocat 专用依赖，风险高。

## 0. 总体原则

- 新板级目录使用 `main/boards/esp-vocat`。
- 目标芯片使用 `esp32s3`。
- 显示分辨率固定为 `360x360`，作为圆屏小屏布局档。
- 核心功能使用当前工程现有模块：`Application`、`AudioService`、`HomeScreen`、`NetworkScreen`、`MusicScreen` 等。
- 旧工程只作为硬件引脚、LCD 初始化、触摸、电池 profile、SD/ML307 连接方式的来源。
- 首版不启用相机、磁场、震动、蓝牙音乐和电话，避免硬件不确定或资源冲突。

## 1. 硬件能力表

| 功能 | 支持状态 | 方案 |
|---|---|---|
| 语音对话 | 支持 | ES8311 + ES7210，复用当前 `BoxAudioCodec` |
| 离线唤醒 | 支持 | ESP32-S3 + AFE/WakeWord |
| Wi-Fi | 支持 | ESP32-S3 原生 Wi-Fi |
| 4G 网络 | 二阶段 | ML307，后续复用当前 `DualNetworkBoard` |
| LCD | 已接入 | ST77916 QSPI，已迁入旧工程厂商 init 表，并增加 TE 同步刷新 |
| 触摸 | 已接入 | CST816S，已挂到 `SpiLcdDisplay` |
| 背光 | 支持 | GPIO3 PWM |
| 电池 | 二阶段 | BQ27220，复用当前 `Bq27220Gauge`，移植 profile |
| SD 卡 | 待上板验证 | 1-bit SDMMC |
| 电源键/软关机 | 部分接入 | PG1 读键，PG2 保持电源；软关机策略后续补齐 |
| 头部触摸 | 支持 | GPIO8 |
| 说话灯 | 支持 | GPIO18，低电平有效 |
| 姿态/摇一摇 | 待验证 | QMI8658A，需要轻量驱动 |
| 相机 | 暂不支持 | 没有看到摄像头硬件定义 |
| 磁场 | 暂不支持 | 没有看到磁力计 |
| 震动 | 暂不支持 | 没有看到马达引脚 |
| 蓝牙音乐 | 暂缓 | ESP32-S3 可做，但首版先避开 Wi-Fi/AFE/内存冲突 |
| 电话 | 暂缓 | ML307 音频链路未确认 |

## 2. 文件清单

新增：

```text
main/boards/esp-vocat/config.h
main/boards/esp-vocat/config.json
main/boards/esp-vocat/esp_vocat.cc
main/boards/esp-vocat/README.md
main/boards/esp-vocat/bq27220_profile.h
main/boards/esp-vocat/bq27220_profile.cc
```

可能需要从源工程迁入或作为组件添加：

```text
esp_lcd_st77916.h/.c
esp_lcd_touch_cst816s.h/.c
QMI8658A 轻量驱动
```

优先复用当前工程：

```text
main/audio/codecs/box_audio_codec.*
main/boards/common/dual_network_board.*
main/boards/common/ml307_board.*
main/boards/common/bq27220_gauge.*
main/boards/common/backlight.*
main/display/lv_adapter_display.*
main/display/screen/*
```

## 3. 板级引脚定义

`main/boards/esp-vocat/config.h` 使用以下核心定义：

```cpp
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE    true

#define PG1_POWER_KEY_GPIO          GPIO_NUM_16
#define PG1_POWER_KEY_ACTIVE_LEVEL  0
#define PG2_HOLD_GPIO               GPIO_NUM_7
#define PG2_HOLD_ACTIVE_LEVEL       1

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_42
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_39
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_40
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_38
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_41

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_4
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_2
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_1

#define DISPLAY_WIDTH  360
#define DISPLAY_HEIGHT 360

#define QSPI_PIN_NUM_LCD_PCLK  GPIO_NUM_14
#define QSPI_PIN_NUM_LCD_CS    GPIO_NUM_10
#define QSPI_PIN_NUM_LCD_DATA0 GPIO_NUM_13
#define QSPI_PIN_NUM_LCD_DATA1 GPIO_NUM_9
#define QSPI_PIN_NUM_LCD_DATA2 GPIO_NUM_12
#define QSPI_PIN_NUM_LCD_DATA3 GPIO_NUM_46
#define QSPI_PIN_NUM_LCD_RST   GPIO_NUM_21
#define QSPI_PIN_NUM_LCD_TE    GPIO_NUM_17
#define QSPI_PIN_NUM_LCD_BL    GPIO_NUM_3

#define TP_PIN_NUM_INT GPIO_NUM_11

#define BSP_SD_CLK GPIO_NUM_48
#define BSP_SD_CMD GPIO_NUM_45
#define BSP_SD_D0  GPIO_NUM_47

#define ML307_RX_PIN   GPIO_NUM_5
#define ML307_TX_PIN   GPIO_NUM_6
#define ML307_EN_PIN   GPIO_NUM_15
#define ML307_UART_NUM UART_NUM_2

#define HEAD_TOUCH_GPIO         GPIO_NUM_8
#define HEAD_TOUCH_ACTIVE_LEVEL 1

#define SPEAKING_LED_GPIO         GPIO_NUM_18
#define SPEAKING_LED_ACTIVE_LEVEL 0
```

## 4. 配置策略

`config.json` 首版建议：

```json
{
    "target": "esp32s3",
    "builds": [
        {
            "name": "esp-vocat",
            "sdkconfig_append": [
                "CONFIG_MMAP_FILE_NAME_LENGTH=32",
                "CONFIG_FLASH_EXPRESSION_ASSETS=y",
                "CONFIG_CODEC_ES8311_SUPPORT=y",
                "CONFIG_CODEC_ES7210_SUPPORT=y",
                "CONFIG_BT_ENABLED=n",
                "CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE=n"
            ]
        }
    ]
}
```

如果首轮内部 SRAM 紧张，继续关闭：

```text
CONFIG_USE_AUDIO_PROCESSOR=n
CONFIG_USE_AFE_WAKE_WORD=n
```

如果唤醒和对话稳定，再恢复 AFE。

## 5. 初始化顺序

`EspVocat` 构造函数按下面顺序实现：

1. `InitializePowerLatch()`：PG2 拉高，保证开机不掉电。
2. `InitializePowerKey()`：PG1 输入，上拉，后续支持关机。
3. `InitializeButtons()`：BOOT 键。
4. `InitializeI2c()`：I2C0，GPIO2/GPIO1，400k。
5. `InitializeBatteryGauge()`：BQ27220。
6. `InitializeSpi()`：SPI2 QSPI bus。
7. `InitializeDisplay()`：ST77916 + `LVAdapterDisplay` 或小屏 Display。
8. `InitializeBacklight()`：GPIO3 PWM。
9. `StartupBatteryGate()`：低电压保护。
10. `GetAudioCodec()->Start()`：ES8311/ES7210。
11. `InitializeTouch()`：CST816S 注册到 LVGL indev。
12. `InitializeSdCard()`：1-bit SDMMC，失败不致命。
13. `InitializeHeadTouch()`：GPIO8。
14. `InitializeSpeakingLed()`：GPIO18。
15. `InitializeMotion()`：QMI8658A，第二阶段再做。

## 6. 首版 App 裁剪

360 圆屏首版保留：

```text
聊天
网络配置
音乐
设置
天气
电台
录音
SD卡
信息
主题
日历
计算器
OpenClaw
翻译
AI生图
2048
```

首版隐藏：

```text
电话
相机
地图/GPS
水平仪
磁场
震动
引脚测试
测试
ESPClaw
```

隐藏方式：在 `HomeScreen` 的 `kApps` 表中根据 `BOARD_ESP_VOCAT` 或 `DISPLAY_WIDTH == 360` 使用小屏 App 列表。

## 7. 360 圆屏 UI 适配规则

新增通用判断：

```cpp
constexpr bool kLayoutRoundSmall =
    DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360;
```

主屏：

- 720 方屏：保持 3x3。
- 800x480 横屏：保持当前横屏布局。
- 360 圆屏：改为 2x2，每页 4 个 App。
- 图标尺寸：72~84 px。
- App cell：约 120x120。
- 状态栏：28~32 px。
- 圆屏安全区：内容尽量限制在中心 320x320 内。

应用页：

- 禁止写死 `720` 或 `800/480`。
- 页面宽高使用 `DISPLAY_WIDTH` / `DISPLAY_HEIGHT`。
- 弹窗最大宽度 300，高度不超过 260。
- 返回按钮 44~52 px，位置偏内。
- 列表每屏显示 3~4 项，依赖滚动。
- 文本尽量 1~2 行，长文本滚动或省略。

## 8. 核心页面适配顺序

按优先级逐步完成：

1. `HomeScreen`：2x2 App、状态栏、分页。
2. `ChatScreen`：最近 1~2 行文本 + 表情/状态。
3. `SettingsScreen`：列表压缩。
4. `NetworkScreen`：Wi-Fi/4G 切换，小屏列表和密码弹窗。
5. `MusicScreen`：小圆盘 + 两行歌词 + 三个控制按钮。
6. `WeatherScreen`：当前天气为主，预报做横向/纵向滚动。
7. `RadioScreen`：列表每屏 3 项。
8. `RecordingScreen`：录音按钮居中，列表压缩。
9. `SdCardScreen`：状态 + 文件列表。
10. `StandbyScreen`：时钟常显，圆屏居中。

## 9. 编译和验证命令

ESP-VoCat 独立构建：

```bash
source /home/qmx/esp/esp-idf/export.sh
idf.py -B build-esp-vocat -DSDKCONFIG=sdkconfig.esp-vocat build
```

烧录：

```bash
idf.py -B build-esp-vocat -DSDKCONFIG=sdkconfig.esp-vocat -p /dev/ttyUSB0 flash monitor
```

等价 `esptool.py` 烧录地址：

```bash
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 build-esp-vocat/bootloader/bootloader.bin \
  0x9000 build-esp-vocat/partition_table/partition-table.bin \
  0x5a000 build-esp-vocat/ota_data_initial.bin \
  0x5d000 build-esp-vocat/srmodels/srmodels.bin \
  0x160000 build-esp-vocat/xiaozhi.bin \
  0xa60000 build-esp-vocat/mmap_build/xingzhi-assets/resources/resources.bin \
  0xe60000 build-esp-vocat/factory_test.bin
```

首轮验收日志：

```text
ESP-VoCat: PG2 latch ready
ESP-VoCat: I2C ready
ESP-VoCat: ST77916 LCD ready 360x360
ESP-VoCat: CST816S touch ready
BoxAudioCodec: BoxAudioDevice initialized
AudioCodec: Set input enable to true
EspWakeWord: Wake word(...)
WifiStation: Got IP
```

## 10. 分阶段检查表

### 阶段 A：文档和骨架

- [x] 新增本移植文档
- [x] 新增 `main/boards/esp-vocat/config.h`
- [x] 新增 `main/boards/esp-vocat/config.json`
- [x] 新增 `main/boards/esp-vocat/README.md`
- [x] 新增最小 `esp_vocat.cc`

### 阶段 B：基础硬件

- [x] 电源保持 PG2
- [x] 电源键 PG1
- [x] I2C0
- [x] QSPI SPI2
- [x] ST77916 LCD
- [x] GPIO3 背光
- [x] CST816S 触摸

### 阶段 C：音频和网络

- [x] ES8311/ES7210 `BoxAudioCodec`
- [x] 唤醒词初始化
- [x] Wi-Fi-only 首版构建
- [ ] ML307 4G 联网
- [ ] Wi-Fi/4G 切换

### 阶段 D：电源和存储

- [ ] BQ27220 电量 profile 移植
- [ ] 低电压开机保护
- [ ] SD 卡挂载
- [ ] 待机时钟
- [ ] 软关机

### 阶段 E：360 UI

- [x] HomeScreen 2x2
- [x] App 列表裁剪
- [ ] ChatScreen 小屏
- [ ] NetworkScreen 小屏
- [ ] MusicScreen 小屏
- [ ] SettingsScreen 小屏
- [ ] Weather/Radio/Recording/SD 小屏

### 阶段 F：可选增强

- [ ] QMI8658A 姿态/摇一摇
- [ ] 水平仪适配 QMI8658A
- [ ] OpenClaw 小屏体验
- [ ] AI 生图小屏预览
- [ ] 翻译小屏体验
- [ ] 蓝牙音乐可行性验证

## 11. 当前执行状态

2026-08-04 执行记录：

- 已新增 ESP-VoCat 板级骨架并接入 Kconfig/CMake。
- 已接入 ST77916 QSPI LCD、CST816S 触摸、GPIO3 背光、PG2 电源保持、PG1 电源键、BOOT 键。
- 已迁入旧工程 `vendor_specific_init_yysj` 初始化表；LCD QSPI 刷新降到 `40MHz`，并通过 `QSPI_PIN_NUM_LCD_TE`/GPIO17 做大块刷新前 TE 同步，处理“能显示文字但屏幕闪烁/撕裂”的问题。
- 已接入 ES8311 + ES7210 的 `BoxAudioCodec`。
- 首版选择 Wi-Fi-only，`CONFIG_BT_ENABLED=n`，ML307/4G 放到第二阶段。
- 已把无马达板级的震动页/震动测试改为 `VIBRATE_MOTOR_GPIO` 能力开关，ESP-VoCat 标记为 `GPIO_NUM_NC`。
- 已修复 BT 关闭时 `native_bluetooth_audio.cc` 仍引用 Bluedroid 地址类型的问题。
- 已修复 ESP-IDF 5.5 C++ 下 ST77916/CST816S 配置宏的 designated initializer 顺序问题。
- 已给 ESP-VoCat 提供 `metalio_claw_4_get_i2c_bus()` 兼容入口，供现有传感器/测试页复用 I2C0。
- 构建已通过：`build-esp-vocat/xiaozhi.bin`。
- 基础板级构建大小 `0x738a40`，最小 app 分区 `0x900000`，剩余约 20%。
- HomeScreen 2x2 + ESP-VoCat App 裁剪后大小 `0x70b490`，最小 app 分区 `0x900000`，剩余约 22%。
- ESP-VoCat 主屏每页 4 个 App，已隐藏电话、相机、地图/GPS、水平仪、磁场、震动、引脚测试、测试、ESPClaw。

下一步按顺序做：

1. 上板烧录，确认 PG2 保持、电源键、背光、LCD 方向/颜色、触摸坐标。
2. 如果 LCD 仍闪烁，下一步优先验证 TE GPIO17 是否真实输出；若 TE 信号异常，临时关闭 0x35 TE 命令并继续降低 QSPI pclk 到 `26MHz` 或 `20MHz`。
3. 移植 BQ27220 profile 并启用电量/低电压保护。
4. 验证 SD 1-bit 挂载。
5. 做 `HomeScreen` 360 圆屏 2x2 和 App 裁剪。
6. 逐个压缩 Chat/Network/Music/Settings/Weather/Radio/Recording/SD 页面。
7. 第二阶段再接 ML307 4G、QMI8658A、蓝牙音乐可行性。
