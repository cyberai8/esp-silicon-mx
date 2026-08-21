# Show — 2D 数字人最终方案（目标冻结）

> **文档名**：`show`  
> **版本**：**1.0 FINAL**（目标冻结 — 实现可分批推进，**不缩小本方案定义的最终效果**）  
> **日期**：2026-08-12  
> **维护原则**：本文档只描述 **我们要做成什么样**。后续修订仅允许：补细节、修错、增验收项；**不允许**再拆「V1/V2/V3 子目标」或降低口型/表情/过渡要求。

---

## 0. 最终目标（一句话）

在喵伴 **360×360** 圆屏上，实现 **分层 2D 数字人**：  
**口型**随真实喇叭播放时钟与 Viseme 时间轴精确切换，含完备过渡、自然收嘴；  
**表情**随 22 种云端情绪独立展现（眉/眼/嘴/overlay 可组合），说话时保留情绪脸、仅嘴层随发音变化；  
**动作**含待机眨眼与微呼吸；  
**禁止** 用 speaking 整脸 clip 或队列压力假嘴作为最终口型方案。

---

## 1. 现状 vs 最终目标

| 维度 | 现状（As-Is） | **最终目标（To-Be，冻结）** |
|------|---------------|---------------------------|
| 口型 | 3 档假嘴 / speaking 整脸 clip | **15 Viseme + 8 过渡帧 + α 叠化**；绑 `played_samples_` |
| 口型同步 | 队列压力，与发音无关 | 相对 I2S：**典型 < 50 ms，P95 < 80 ms** |
| 表情 | 22 emotion → 13 clip 多对一 | **22 emotion 各有一套可辨 Pose**（眉/眼/嘴_base/overlay） |
| 说话时脸 | 整脸 speaking 动画 | **情绪 Pose 保留 + 仅 Viseme 嘴层叠加** |
| 过渡 | 无 | **大跨度 Viseme 必有过渡**（sprite 或 40–80 ms blend） |
| 句末 | 不定 | 强制 **sil 80–150 ms** 自然收嘴 |
| 待机 | clip 循环 | **眨眼 + 微呼吸**（本地 timer，5–8 s 随机） |
| 素材 | 13×360 全帧动画 | **静态底图 + 分层 sprite**（见 §5） |
| 字幕 | 句级，绑收包 | 句级维持；口型 **不绑字幕** |

---

## 2. 最终形态：分层 2D 数字人

### 2.1 渲染栈（冻结）

```text
┌─────────────────────────────────────────────┐
│  Layer 5  Overlay（汗、泪、问号、blush）       │  ← emotion / MCP
├─────────────────────────────────────────────┤
│  Layer 4  Eye + Brow（睁闭、视线、挑眉）       │  ← emotion + idle 眨眼
├─────────────────────────────────────────────┤
│  Layer 3  Mouth Viseme（15 类 + 过渡 + blend）│  ← viseme 时间轴 + 播放时钟
├─────────────────────────────────────────────┤
│  Layer 2  Face base（肤色、脸型、耳朵）         │  ← 静态底图，嘴区透明
├─────────────────────────────────────────────┤
│  Layer 1  Body / Hair / 背景（可选 2–3 帧呼吸）│  ← 待机慢动画
└─────────────────────────────────────────────┘
  合成 → PSRAM RGB565 360×360
  脏区刷新：嘴 180×100、眼 80×60、overlay 按需
  刷新率：≥ 25 FPS（对话中 ≥ 20 FPS 全页）
  单写者：LVGL canvas + AvatarCompositor
```

### 2.2 实现形态（冻结选型）

**采用：静态底图 + 分层 Face Rig 局部贴图**（Live2D 仅作美术参考，设备跑自研 blit，不用 Cubism runtime）。

| 方案 | 是否采用 |
|------|----------|
| 静态底图 + 分层 Viseme/表情 sprite | **✓ 最终方案** |
| 13 条整脸 emote clip 驱动口型 | **✗ 禁止**（仅可作降级或无 avatar 分区时的 body 占位） |
| 端侧 AI 口型（Wav2Lip 等） | **✗ 不做** |
| 云端下发整脸视频流 | **✗ 不做**（带宽/延迟） |
| 固定 MP3 话术预渲染视频 | 可选例外路径，非对话主路径 |

### 2.3 像真人的验收哲学（冻结）

1. **说话时**：观众感到「嘴在发这个音」，不是「视频在播」。  
2. **听时**：聆听 Pose（专注眼/眉/闭唇），非 frozen clip。  
3. **换情绪时**：眉眼嘴 overlay 协同变化，22 种可辨。  
4. **过渡时**：PP→大开口无「弹嘴」；快速连读「妈妈米」可验收。  
5. **待机时**：有呼吸/眨眼，非 dead loop。

---

## 3. 口型：Viseme、动态机制与过渡（完整规范）

### 3.1 Viseme 是什么

**Viseme** = 某个发音对应的 **标准嘴形**。设备在正确时刻显示对应静态嘴图；**连续说话感**来自 **快速切换 + 过渡**，不是 GIF。

### 3.2 Viseme 集（冻结：`zh_15`）

| ID | 名称 | 嘴形要点 | 典型音/字 |
|----|------|----------|-----------|
| 0 | SIL | 闭唇或微闭 | 句末、停顿 |
| 1 | PP | 双唇完全闭合 | 妈、波、摸 |
| 2 | FF | 上齿咬下唇 | 发、飞 |
| 3 | TH | 微张口，齿缝 | 思、四 |
| 4 | DD | 微开，舌尖位 | 的、地、那 |
| 5 | kk | 中等开口偏后 | 哥、可、哈 |
| 6 | CH | 扁唇前伸擦音 | 机、七、知 |
| 7 | SS | 扁唇微开 | 丝 |
| 8 | nn | 微开鼻音 | 嗯、恩 |
| 9 | RR | 圆唇卷舌 | 儿、日 |
| 10 | aa | **最大开口** | 啊、大、好 |
| 11 | E | 扁开 | 耶、别 |
| 12 | I | 扁开嘴角拉宽 | 一、你、米 |
| 13 | O | 圆唇开 | 哦、多 |
| 14 | U | 圆唇小开 | 乌、出 |

**强度档（额外 sprite，同 ID 不同图）**：

| 扩展 | 说明 |
|------|------|
| `aa_open` / `aa_small` | 同一 `aa`，大开 vs 小开；由 TTS 能量或元音时长选取 |
| 锚点 | 所有嘴图 **下唇中心对齐 (180, 196)**，画布 140×90，透明底 |

### 3.3 嘴部「动」的三层机制（缺一不可）

```text
① 时间表（Server）   每音素：何时哪 Viseme、持续多久
② 保持规则（Device） 最短 hold、句首/句末/打断
③ 过渡规则（Device + 设计）  相邻 Viseme 如何衔接
```

#### ① 时间表（服务端生成）

每句 TTS 一条 `type: viseme`，示例「妈妈你好」：

```json
{
  "type": "viseme",
  "viseme_set_id": "zh_15",
  "index": 3,
  "utterance_id": "r3_i3",
  "visemes": [
    { "time_ms": 0,   "duration_ms": 60,  "id": 0,  "blend_ms": 0 },
    { "time_ms": 60,  "duration_ms": 140, "id": 1,  "blend_ms": 40, "via": "sil_PP_mid" },
    { "time_ms": 200, "duration_ms": 120, "id": 1,  "blend_ms": 0 },
    { "time_ms": 320, "duration_ms": 160, "id": 10, "blend_ms": 60, "via": "PP_aa_mid" },
    { "time_ms": 560, "duration_ms": 120, "id": 12, "blend_ms": 50, "via": "aa_I_mid" },
    { "time_ms": 920, "duration_ms": 120, "id": 0,  "blend_ms": 40, "via": "aa_PP_mid" }
  ]
}
```

| 字段 | 说明 |
|------|------|
| `time_ms` | 相对 **该句第一个 PCM 采样写入 DAC 的时刻** |
| `duration_ms` | 建议 hold；设备 clamp **最短 40 ms** |
| `id` | Viseme ID（0–14） |
| `blend_ms` | 进入本 Viseme 前 **与上一 Viseme 叠化时长**（40–80 ms） |
| `via` | 可选：指定过渡 sprite 名；无则纯 α lerp |
| `strength` | 0–255，可选；选 `aa_open` vs `aa_small` |

**映射链路**：

```text
doubao2s PCM + enable_timestamp
  → 解析字/音素时间
  → 拼音/音素 → VisemeId（viseme_zh.go）
  → 相邻大跨度自动插入 blend_ms / via
  → 下发 JSON + Opus
```

#### ② 保持规则（设备执行）

| 规则 | 参数 |
|------|------|
| 最短 hold | ≥ **40 ms**（1 帧 @25FPS） |
| 句首 | `sil` 或 `PP`，0–60 ms |
| 句末 | 强制 `SIL`，**80–150 ms** |
| 打断 | **≤ 200 ms** 切 SIL；`speechGeneration++` 作废旧轴 |
| 长元音 | 允许单 Viseme hold 300 ms+ |

#### ③ 过渡规则（设备 + 设计，最终方案必需）

**禁止** 大跨度 Viseme 仅 hard cut。必须满足以下 **至少一条**：

- **α 叠化**：`blend_ms` 40–80 ms，`smoothstep` 缓动  
- **过渡 sprite**：下表 8 张，显示 40–60 ms  

**Blend 算法（设备，每 40 ms tick）**：

```text
progress = clamp((now_ms - t_ms) / blend_ms, 0, 1)
progress = smoothstep(progress)   // 非线性，防机械感
pixel = lerp(sprite_prev, sprite_curr, progress)
```

**过渡帧配对表（设计必须交付 8 张）**：

| 从 → 到 | 文件名 | 视觉 | 时长 |
|---------|--------|------|------|
| PP → aa | `PP_aa_mid` | 双唇刚分，开口约 30% | 40–60 ms |
| aa → PP | `aa_PP_mid` | 口收小，将闭 | 40–60 ms |
| aa → I | `aa_I_mid` | 大开收扁 | 40–60 ms |
| I → aa | `I_aa_mid` | 扁→圆开 | 40–60 ms |
| sil → PP | `sil_PP_mid` | 微张备发 m/b/p | 30–40 ms |
| PP → I | `PP_I_mid` | 闭唇→扁开 | 50–70 ms |
| aa → O | `aa_O_mid` | 大开→圆唇 | 40–60 ms |
| CH → aa | `CH_aa_mid` | 擦音→元音 | 40–60 ms |

### 3.4 设备每帧逻辑（25 FPS）

```text
1. now_ms = GetPlayedSamples() * 1000 / 24000
2. 查当前 utterance viseme 轴 → current, next, blend_progress, via_sprite
3. 若 via_sprite 有效 → 显示过渡帧
    elif blend_ms > 0 → Lerp(prev, curr, progress)
   else → curr
4. Blit 到嘴区矩形 (x≈90, y≈156, w=180, h=100)
5. MarkDirtyRect(嘴区) → QSPI flush
```

### 3.5 口型 vs 假嘴（心智模型）

```text
假嘴（现状，最终要淘汰）:
  有声音 → 嘴变大 → 与发什么音无关

最终方案:
  t=60ms PP(妈) → blend → t=320ms aa(好) → blend → t=920ms SIL(收嘴)
  与音节对齐 + 过渡 + 句末收嘴
```

---

## 4. 表情系统（完整规范）

### 4.1 原则

- **22 种云端 emotion 各有一套可辨识 Pose**，禁止多 emotion 共用一张「shy/happy」脸。  
- **说话时**：更新 `mouth_viseme` 层，**不覆盖** emotion 眉眼。  
- **TTS 结束**：嘴回 `mouth_base`，情绪 Pose 保留至下一句 emotion。  
- **禁止** speaking 整脸 clip 覆盖嘴层。

### 4.2 EmotionPose 结构（设备）

```cpp
struct EmotionPose {
    uint8_t brow;
    uint8_t eye_l, eye_r;
    uint8_t mouth_base;      // 非说话时嘴型
    uint8_t overlay;         // blush / tear / sweat / question
    uint16_t transition_ms;  // 从上一 Pose 渐变，默认 200–400 ms
};
```

### 4.3 22 emotion → Pose 映射（冻结）

| 云端 emotion | brow | eye | mouth_base | overlay |
|--------------|------|-----|------------|---------|
| happy, laughing, funny | up | smile | smile | — |
| loving | soft | heart | kiss | blush |
| kissy | soft | heart | kiss | blush |
| embarrassed | — | down | wavy | blush |
| sad | down | sad | flat | — |
| crying | down | tear | open | tear |
| angry | angry | narrow | frown | — |
| surprised, shocked | up | wide | O | — |
| thinking, confused, silly | one_up | lookup | flat | question |
| delicious | — | sparkle | lick | — |
| sleepy, neutral, relaxed | — | half | tiny | — |
| tired | tired | half | flat | — |
| listening | — | focused | closed | — |
| speaking | *不变* | *保持当前 emotion* | *由 Viseme 接管* | — |

**服务端**：继续发 `llm.emotion`；设备查 **EmotionPose 表**，不再查 `emote_mapping → clip`。

### 4.4 说话时叠加规则（冻结）

```text
llm.emotion     → SetEmotionPose（眉/眼/mouth_base/overlay）
type:viseme     → SetViseme（仅 Layer 3，覆盖 mouth_base）
tts.stop        → Viseme=SIL；mouth 回 mouth_base；Pose 保留
abort           → 200ms 内 SIL；Pose 可选回 listening
```

### 4.5 Idle 生命感（冻结）

| 动作 | 规格 |
|------|------|
| 眨眼 | 随机 **5–8 s** 一次；140 ms 闭眼帧；左右眼可错开 20 ms |
| 微呼吸 | body/base **2–3 帧** 慢循环，周期 **3–4 s**，幅度 ≤ 3 px |
| 聆听 | listening Pose；偶尔 mouth 微张 idle |

---

## 5. 素材与 Flash（设计交付 — 完整清单）

### 5.1 锚点与画布（冻结）

| 项 | 值 |
|----|-----|
| 逻辑画布 | 360 × 360 |
| 嘴部动态区 | 180 × 100，中心锚点 **(180, 196)** |
| 嘴 sprite 画布 | 140 × 90；下唇中心在 sprite **(70, 75)** |
| 眼区 | 64 × 56；眼心 (100,172) / (260,172) 与 ExpressionView 一致 |
| 格式 | 设计 PSD/AI/SVG → 导出 **RGB565 PNG** → 打包 `avatar.bin` |
| 底图 | **嘴部必须透明** |

### 5.2 必须交付的文件

| # | 内容 | 数量 | 说明 |
|---|------|------|------|
| 1 | `base_360.png` | 1 | 静态底图，嘴透明 |
| 2 | `body_breathe_*.png` | 2–3 | 可选微呼吸 |
| 3 | `viseme_00_SIL` … `viseme_14_U` | **15** | 主 Viseme |
| 4 | `viseme_10_aa_open` / `aa_small` | 2 | 强度档 |
| 5 | `transition_*.png` | **8** | §3.3 配对表 |
| 6 | `brow_*.png` | **8** | 眉形 |
| 7 | `eye_*.png` | **10** | 含开/半/闭/ sad/wide… |
| 8 | `overlay_*.png` | **6** | blush/tear/sweat/question/sparkle/heart |
| 9 | `emotion_board/` | 22×2 | 每 emotion 静默帧 + 说话帧预览 |
| 10 | `avatar_manifest.json` | 1 | id、文件名、锚点、尺寸 |
| 11 | PSD 分层源 | 1套 | 全部图层可编辑 |

**禁止交付**：speaking 整脸循环动画作为口型方案。

### 5.3 Flash 分区（冻结要求）

现有 `emote_gen` 4.75 MB 已满。**必须** 新增 `avatar` 分区 **2–4 MB**（改 `partitions/v2/16m.csv` + OTA 策略）。  
13 clip emote pack **不再扩容**；Show 主路径改用 `avatar.bin`。

估算：`avatar.bin` ≈ 1.5–2.5 MB（15+2+8 嘴 + 眉眼 overlay + base）。

---

## 6. 音画同步（冻结）

### 6.1 唯一主时钟

**`played_samples_`**：24 kHz mono，在 `AudioService::OutputData` 成功后累加。  
口型、Viseme 查找 **只认此时钟**。

### 6.2 端到端链路

```text
服务端
  TTS → PCM + timestamp
  → VisemeEvent[] + Emotion
  → 首 Opus 前：llm.emotion → viseme → sentence_start
  → Binary Opus × N

设备
  viseme → LipSyncController 缓存（绑 index + speechGeneration）
  首帧 OutputData → 锚点 played_samples_=0
  每 40ms tick → Lookup(now_ms) → Compositor blit
  abort/gen++ → Reset → SIL
```

### 6.3 三条铁律

1. **口型不认** 网络到达、字幕显示、队列深度。  
2. **一句一轴**：`index` + `speechGeneration` + `utterance_id` 绑定。  
3. **sentence_start 不能当 t=0**（会比喇叭早 60–200 ms）。

### 6.4 同步指标（验收）

| 指标 | 目标 |
|------|------|
| Viseme 切换典型误差 | **< 50 ms** |
| Viseme 切换 P95 | **< 80 ms** |
| 打断停嘴 | **≤ 200 ms** |
| 句末收嘴 | **80–150 ms SIL** |
| 字幕句首 | 可早于声音 ≤ 250 ms（口型不受影响） |

---

## 7. 算力分工（预置 vs 云端 vs 设备）

| 层级 | 负责什么 | 不负责什么 |
|------|----------|------------|
| **预置（Flash）** | 嘴/眼/眉/底图/过渡帧/overlay 长什么样 | 切换时机 |
| **云端** | TTS、timestamp 解析、Viseme 轴、emotion、下发 JSON | 渲染 |
| **设备** | 解码、I2S、`played_samples_`、Viseme 调度、blit、blend、眨眼 | AI 口型、音素识别 |

**结论**：真实感 = **预置素材** + **云端时间轴** + **设备对齐调度**；不是端侧 AI，也不是「素材做好自动同步」。

---

## 8. 协议设计

### 8.1 消息类型

| 消息 | 作用 |
|------|------|
| `llm.emotion` | 设 EmotionPose |
| `type: viseme` | 设本句 Viseme 时间轴（见 §3.3） |
| `tts.sentence_start` | 字幕 + utterance 上下文 |
| `tts.sentence_end` | 可选，标记句结束 |
| Binary Opus | 音频 |
| `tts.stop` / abort | 清空 viseme + SIL |

### 8.2 `type: viseme` 完整示例

```json
{
  "type": "viseme",
  "protocol_version": 1,
  "viseme_set_id": "zh_15",
  "session_id": "...",
  "utterance_id": "r3_i1",
  "index": 1,
  "round": 3,
  "audio": { "sample_rate": 24000, "channels": 1, "duration_ms": 3280 },
  "visemes": [
    { "time_ms": 0, "duration_ms": 60, "id": 0, "blend_ms": 0 },
    { "time_ms": 60, "duration_ms": 140, "id": 1, "blend_ms": 40, "via": "sil_PP_mid" }
  ]
}
```

### 8.3 下发时序（冻结）

```text
sendAudioChunk 首次有声：
  1. sendEmotionMessage
  2. sendVisemeMessage
  3. sendTTSMessage sentence_start
  4. sendAudioFramesStream
```

### 8.4 降级（例外路径，非目标）

| 条件 | 行为 |
|------|------|
| 无 viseme / 解析失败 | 临时回退假嘴；**日志告警**；不阻塞 TTS |
| MP3 缓存 / AI 演唱 | 不发 viseme；假嘴或 SIL |
| 时间轴漂移 > 500 ms | 软 resync 到最近 keyframe |

---

## 9. 三仓职责

```text
/Users/apple/cyberservice/
├── voiceshow/                 ← 主改：Compositor、LipSync、播放时钟、渲染
├── anime-ai-chat-server/      ← 主改：timestamp、Viseme 轴、协议
├── anime-ai-chat-web-xiaozhi/ ← 辅改：调试页、样本导出
└── show.md                    ← 本文档
```

| 仓库 | 职责 |
|------|------|
| **voiceshow** | `AvatarCompositor`、`LipSyncController`、`GetPlayedSamples()`、素材加载、`avatar` 分区 |
| **anime-ai-chat-server** | doubao2s 解析、`viseme_zh.go`、`sendVisemeMessage` |
| **anime-ai-chat-web-xiaozhi** | Viseme 时间轴预览、TTS 样本导出 |
| **esp-vocat-ui** | Show 期间不改；稳定后 cherry-pick |

### 9.1 esp-show 板型门控（已实现）

| 项 | 说明 |
|----|------|
| 设备板型 | `voiceshow` 编译 `BOARD_TYPE=esp-show`（OTA body `board.type`） |
| 服务端识别 | `checkDeviceInfo()` → `device.BoardType` → `supportsVisemeDownlink()` |
| 下发条件 | **仅** `boardType == esp-show` 的连接，在 TTS 首包前 `sendVisemeMessage` |
| 其他板型 | 不发 `type:viseme`，行为与现网一致 |
| 注意 | 刷 voiceshow 后需 **至少一次 OTA**，服务端 `devices.board_type` 才会更新为 `esp-show` |

---

## 10. 设备端模块（voiceshow）

```text
main/display/avatar/
├── avatar_compositor.h/cc     # 分层合成、EmotionPose、idle
├── lip_sync_controller.h/cc   # viseme 轴、played_samples 查询、打断
├── viseme_types.h             # zh_15 枚举、manifest
├── mouth_renderer.h/cc        # blit、blend、via 过渡
└── avatar_assets.h/cc         # avatar.bin mmap 加载

main/audio/audio_service.cc    # played_samples_ 累加/暴露
main/application.cc            # 解析 viseme/emotion
main/display/vocat_lvgl/screens/screen_home.cc  # 接入 Compositor，禁用 clip 口型
```

**必改约束**：

- flush 回调内禁止 `Schedule` LVGL  
- 嘴/眼缓冲放 PSRAM  
- Viseme 启用时 **关闭** `TickMouthFromPlayback` 假嘴  
- emote 13 clip **不得** 绘制嘴部（透明或不用 speaking clip）

---

## 11. 服务端模块（anime-ai-chat-server）

| 模块 | 改动 |
|------|------|
| `doubao2s/doubao2s.go` | 解析 timestamp JSON |
| `utils/viseme_zh.go`（新） | 音素 → VisemeId；大跨度插入 blend |
| `connection_sendmsg.go` | `sendVisemeMessage()` |
| `connection.go` | timeline 绑 `AudioChunk` |
| `docs/` | 协议补充 viseme + emotion_pose |

**阻塞项**：≥10 组 TTS 样本（文本 + Opus + timestamp 原始 JSON）。

---

## 12. 验收标准（完整 — 达到即视为 Show 完成）

### 12.1 口型

| # | 用例 | 通过条件 |
|---|------|----------|
| L1 | 「妈妈」 | PP 可辨；句末 SIL |
| L2 | 「你好呀」 | Viseme 完整；过渡无弹嘴 |
| L3 | 「妈妈米」快速 | PP→I 无肉眼弹嘴 |
| L4 | 长句 60 s | FPS ≥ 20；嘴型不断档 |
| L5 | 连续 10 轮 | 无旧 viseme 残留 |
| L6 | 播报打断 | ≤ 200 ms SIL |
| L7 | 10 句验收表 | 通过率 ≥ **90%** |

### 12.2 表情

| # | 用例 | 通过条件 |
|---|------|----------|
| E1 | 22 emotion 各测 1 句 | 每种可辨识，互不混淆 |
| E2 | happy 句内说话 | 保持 happy 眉眼，嘴随 Viseme |
| E3 | thinking → happy 两句 | Pose 切换 200–400 ms 渐变 |
| E4 | listening 待机 | 专注眼；5–8 s 有眨眼 |

### 12.3 性能

| 指标 | 目标 |
|------|------|
| 对话 FPS | ≥ 20 |
| 嘴部 tick | 25 FPS |
| PSRAM 增量 | < 400 KB（不含 avatar 分区本体） |
| 内部 RAM 增量 | < 8 KB |
| 24 h 连续对话 | 无泄漏、无崩溃 |

---

## 13. 风险与依赖

| 风险 | 缓解 |
|------|------|
| timestamp 粒度不足 | 拼音 MFA 对齐 fallback |
| Flash | **必须** avatar 分区 2–4 MB |
| 内部 RAM 25 KB | 缓冲放 PSRAM |
| 设计延期 | 占位 sprite 联调时钟与协议；**不降低最终 Viseme/表情数量** |

**外部依赖（阻塞）**：

1. 设计交付 §5.2 全套素材  
2. 服务端 10 组 timestamp 样本  
3. 分区表变更评审  

---

## 14. 源码索引

| 模块 | 路径 |
|------|------|
| 音频 | `voiceshow/main/audio/audio_service.cc` |
| 协议 | `voiceshow/main/application.cc` |
| 主页 | `voiceshow/main/display/vocat_lvgl/screens/screen_home.cc` |
| 假嘴（最终移除） | `voiceshow/main/display/vocat_lvgl/components/expression_controller.cc` |
| TTS | `anime-ai-chat-server/src/core/providers/tts/doubao2s/doubao2s.go` |
| 下发 | `anime-ai-chat-server/src/core/connection_sendmsg.go` |
| 表情协议 | `anime-ai-chat-server/docs/设备主页表情云端对照表.md` |
| 硬件基线 | `esp-vocat-ui/docs/喵伴ESP32-S3数字人口型项目——前置信息收集表-已填写.md` |

---

## 15. 术语表

| 术语 | 含义 |
|------|------|
| Viseme | 视觉音素；一种发音对应的标准嘴形 |
| EmotionPose | 一种情绪的眉/眼/mouth_base/overlay 组合 |
| played_samples_ | 已写入 DAC 的 PCM 采样数（24 kHz） |
| blend_ms | 两 Viseme 之间 α 叠化时长 |
| via | 指定过渡 sprite 名 |
| utterance | 一句 TTS，对应 `index` |
| speechGeneration | 打断代次 |

---

## 附录 A. 建议工作顺序（仅排期，不改变 §0–§12 目标）

实现时可按下列顺序推进，**每步均向最终方案靠拢**，不做「临时简化版规格」：

| 顺序 | 工作项 | 说明 |
|------|--------|------|
| 1 | 分区 + `avatar.bin` 加载骨架 | 先通路，可用占位图 |
| 2 | `GetPlayedSamples()` + LipSyncController | 时钟是根基 |
| 3 | 服务端 viseme 下发 + 样本验证 | 时间轴来源 |
| 4 | Compositor 嘴层 blit + blend | 15 Viseme + 8 过渡 |
| 5 | EmotionPose 22 表 + 眉眼层 | 表情完整 |
| 6 | idle 眨眼/呼吸 | 生命感 |
| 7 | 全量验收 §12 | 通过即完成 |

**注意**：联调早期可用占位 sprite，但 **协议、时钟、分层架构、Viseme 数量、过渡机制** 按最终方案一次定好，避免后面改协议。

---

## 附录 B. 文档修订记录

| 版本 | 日期 | 说明 |
|------|------|------|
| 0.x | 2026-08-12 | 探索稿（含 V1/V2/V3 分阶段，已废止） |
| **1.0 FINAL** | 2026-08-12 | **目标冻结**：单一最终规格；口型 15+过渡+blend；表情 22 Pose；分层数字人 |
