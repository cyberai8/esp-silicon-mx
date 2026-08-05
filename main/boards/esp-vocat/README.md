# ESP-VoCat

ESP-VoCat 喵伴板级移植自 `/home/qmx/esp-project-sb/vocat-3/main/boards/esp-vocat`，目标是在当前工程中使用现有对话、网络和 LVGL App 体系。

## 硬件

- ESP32-S3-WROOM-1
- 360x360 ST77916 QSPI 圆形 LCD
- CST816S 触摸
- ES8311 DAC/功放输出 + ES7210 多麦输入
- BQ27220 电量计
- ML307 4G 模块
- 1-bit SDMMC
- PG1/PG2 软电源保持

## 当前移植状态

首版 Wi-Fi-only 构建已通过：

- Kconfig/CMake 已加入 `BOARD_TYPE_ESP_VOCAT`
- `config.h/config.json` 已加入
- PG2 电源保持、PG1 电源键、BOOT 键已接入
- I2C0 已接入，兼容现有 `metalio_claw_4_get_i2c_bus()` 调用
- ST77916 QSPI LCD 已接入
- CST816S 触摸已接入到 `SpiLcdDisplay`
- GPIO3 PWM 背光已接入
- ES8311 + ES7210 通过 `BoxAudioCodec` 接入
- GPIO18 说话灯已接入
- HomeScreen 已按 360 圆屏使用 2x2 分页，并隐藏首版不支持硬件入口

暂缓项：

- ML307 4G/Wi-Fi 切换
- BQ27220 profile 和低电压保护
- SD 卡上板挂载验证
- QMI8658A 姿态
- 蓝牙音乐
