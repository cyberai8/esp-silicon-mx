# 数字人对口型素材包（由 gif人物 生成）

源目录：`数字人口型素材/gif人物`（360×360 整脸 GIF）

## 目录说明

| 目录 | 用途 |
|------|------|
| `viseme_gif/` | 口型循环 GIF：a/e/i/o/v/er |
| `viseme_png/` | 各口型开口最大代表帧 PNG |
| `mouth_levels/` | **无云端能量驱动**用的 4 档整脸关键帧 |
| `emotion_gif/` | 表情映射到现有数字人 6 大类 |
| `frames/{name}/` | 逐帧 PNG（便于检查/再加工） |
| `preview_sheet/` | 预览拼图 |

## 无云端方案（推荐先用）

播放 TTS PCM 能量 → 切换：

- `mouth_0.png` 闭口（idle）
- `mouth_1.png` 微张（i）
- `mouth_2.png` 半开（a）
- `mouth_3.png` 圆唇大开（o）

实现上可以：
1. **整脸切换**：speaking 时按档切换上述 PNG/短 EAF（最简单）
2. 或把代表帧做成短循环 EAF，能量高时播开口大的循环

## 中文音素近似映射（以后有时间戳再用）

- 静音 / b p m → idle
- a ai ao → a
- e ei → e
- i ü j q x → i
- o u ou → o
- f → v
- er → er

## 转 EAF

1. 打开 https://esp32-gif.espressif.com/
2. 上传 `viseme_gif/*.gif` 与 `emotion_gif/*.gif`
3. 导出放到 SD：`/sdcard/system/emotion/`

## 与现有 DigitalPeopleScreen

现有需要：crying/happy/loving/neutral/surprised/thinking

本包已映射：
- idle → neutral
- happy → happy
- sad → crying
- think → thinking
- angry → surprised（近似，建议日后补正 surprise 素材）
- loving → 暂缺，可先复用 happy
