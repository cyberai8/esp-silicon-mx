#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

// ESP-Show（硬件基于 Waveshare ESP32-S3-Touch-LCD-1.85B）
// 文档: https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B/
// 360x360 ST77916 QSPI + CST816S + ES8311/ES7210 + BQ27220 + SDMMC 4-bit

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
// ES7210 TDM 双麦；本板未启用设备端 AFE 处理器，开参考通道会让唤醒词走 AEC，
// 在当前内存/esp-sr 组合下会于 esp_aec3_dlfft_process 空指针崩溃，故关闭。
#define AUDIO_INPUT_REFERENCE    false

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_2
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_38
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_48
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_39
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_47

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_9
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_11
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_10
#define I2C_SDA_PIN              AUDIO_CODEC_I2C_SDA_PIN
#define I2C_SCL_PIN              AUDIO_CODEC_I2C_SCL_PIN
#define AUDIO_CODEC_ES8311_ADDR  0x30
#define AUDIO_CODEC_ES7210_ADDR  0x80

#define BOOT_BUTTON_GPIO GPIO_NUM_0

#define DISPLAY_WIDTH    360
#define DISPLAY_HEIGHT   360
// 整屏逆时针旋转 90°（相对面板默认方向）：swap + mirror_y。
// 触摸 flags 与面板保持一致，否则触点会错位。
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY  true
// QMI8658 重力方向自动旋转（90° 步进）；0 关闭。
#define DISPLAY_AUTO_ROTATION 1
// IMU 判定方向相对真实屏幕的固定补偿（顺时针 90/180/270）。
#define DISPLAY_ORIENT_OFFSET_DEG 90
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define QSPI_LCD_H_RES         360
#define QSPI_LCD_V_RES         360
#define QSPI_LCD_BIT_PER_PIXEL 16
#define QSPI_LCD_HOST          SPI2_HOST
#define QSPI_PIN_NUM_LCD_PCLK  GPIO_NUM_40
#define QSPI_PIN_NUM_LCD_CS    GPIO_NUM_21
#define QSPI_PIN_NUM_LCD_DATA0 GPIO_NUM_46
#define QSPI_PIN_NUM_LCD_DATA1 GPIO_NUM_45
#define QSPI_PIN_NUM_LCD_DATA2 GPIO_NUM_42
#define QSPI_PIN_NUM_LCD_DATA3 GPIO_NUM_41
#define QSPI_PIN_NUM_LCD_RST   GPIO_NUM_3
#define QSPI_PIN_NUM_LCD_TE    GPIO_NUM_NC
#define QSPI_PIN_NUM_LCD_BL    GPIO_NUM_5

#define DISPLAY_BACKLIGHT_PIN           QSPI_PIN_NUM_LCD_BL
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define TP_PORT        I2C_NUM_0
#define TP_PIN_NUM_RST GPIO_NUM_1
#define TP_PIN_NUM_INT GPIO_NUM_4

#define SDMMC_CLK_PIN   GPIO_NUM_15
#define SDMMC_CMD_PIN   GPIO_NUM_14
#define SDMMC_D0_PIN    GPIO_NUM_16
#define SDMMC_D1_PIN    GPIO_NUM_17
#define SDMMC_D2_PIN    GPIO_NUM_12
#define SDMMC_D3_PIN    GPIO_NUM_13
#define SDMMC_BUS_WIDTH 4
#define SDMMC_MAX_FREQ_KHZ SDMMC_FREQ_DEFAULT
#define MOUNT_POINT "/sdcard"

#define UART1_TX GPIO_NUM_43
#define UART1_RX GPIO_NUM_44

#define HEAD_TOUCH_GPIO         GPIO_NUM_NC
#define HEAD_TOUCH_ACTIVE_LEVEL 0
#define SPEAKING_LED_GPIO         GPIO_NUM_NC
#define SPEAKING_LED_ACTIVE_LEVEL 0
#define VIBRATE_MOTOR_GPIO GPIO_NUM_NC

#define BOARD_HAS_EXTERNAL_BT 0
#define BOARD_HAS_NATIVE_BT   0
#define BOARD_HAS_DUAL_SIM    0
#define BT_AUDIO_TX_PIN       GPIO_NUM_NC
#define BT_AUDIO_RX_PIN       GPIO_NUM_NC

// 与 VoCat 同为 360 圆屏；复用圆屏 UI 适配分支
#define BOARD_ESP_VOCAT 1
#define BOARD_ESP_SHOW 1

#endif  // _BOARD_CONFIG_H_
