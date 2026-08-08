# Waveshare ESP32-S3-Touch-LCD-1.85B

硬件文档: https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B/

## 规格摘要

- ESP32-S3R8，8MB PSRAM，16MB Flash
- 1.85" 360×360 ST77916 QSPI 圆屏 + CST816S 触摸
- ES8311 播放 + ES7210 双麦回声消除
- BQ27220 电量、QMI8658 IMU、MicroSD（SDMMC 4-bit）
- 无 4G 模组 → 本板继承 `WifiBoard`

## 与 esp-vocat 的关系

分辨率与 UI 栈相同（`LVAdapterDisplay` + 圆屏适配），但 **GPIO 完全不同**，且无 PG1/PG2、无 ML307。

## 构建

```bash
idf.py -B build-waveshare-1.85b -DSDKCONFIG=sdkconfig.waveshare-s3-touch-lcd-1.85b set-target esp32s3
idf.py -B build-waveshare-1.85b -DSDKCONFIG=sdkconfig.waveshare-s3-touch-lcd-1.85b build
```
