# 喵伴 / VoCat SE 对话数字人（口型 + 表情）设备端交接说明

**给谁用**：下一个要在**另一块设备软件**上实现「说话口型 + 对话表情」的工程师。  
**范围**：设备端分层 2D 脸（底图、上半脸、情绪嘴、Viseme 嘴、过渡嘴、装饰 overlay）+ **服务端下发协议摘要**（第 13 节）。不含 ASR、不含圆环菜单/音乐页图标。  
**画布**：360×360 圆屏，原点在左上角，单位像素。  
**资源版本**：V5 底图与 9 套脸 + V5.1 五套扩展脸 + V5.2 八张口型过渡。固件加载名一律是本目录 `runtime/` 下的**扁平文件名**。

本包路径建议解压后保持：

```text
show11/
  README.md                 ← 本文件（设备资源 + 设备逻辑 + 服务端协议）
  avatar_manifest.json      ← 图层坐标机器可读清单
  runtime/                  ← 63 张，设备运行时按文件名加载（必须用这些名字）
  preview/                  ← 17 张整脸合成预览，只给人眼看，不要打进固件
```

---

## 0. 接手人能直接接入吗？

**结论：资源与规则已齐，可以开工；但要跑通口型，还必须接好服务端 WebSocket 协议 + DAC 播放时钟。**

| 本包已覆盖 | 新设备工程师还需自行实现 |
|------------|--------------------------|
| 63 张运行时 PNG 及命名 | 360×360 分层 UI（LVGL / 自研 Canvas 均可） |
| 图层坐标、尺寸、叠放顺序 | WebSocket/MQTT JSON 解析（见第 14 节） |
| 14 套情绪 + overlay 映射表 | Opus 解码 → PCM → DAC，并暴露 `played_samples` |
| `zh_15` viseme ID 与过渡规则 | `LipSyncController` 等价逻辑（Arm / Load / Anchor / Tick / Reset） |
| 整脸 preview 验收图 | OTA 上报 `board_type=esp-show`（否则服务端不下发 viseme） |
| 现网固件参考路径 `voiceshow/` | 40ms 渲染定时器 + 眨眼 + 能量假嘴 |

**建议阅读顺序**

1. 第 1～9 节：画面长什么样、贴哪张图、坐标多少  
2. 第 14 节：**服务端发什么、什么顺序、设备怎么响应**（接入新板必看）  
3. 第 10～12 节：最小实现步骤与验收  
4. 对照 `voiceshow/main/application.cc`（JSON 入口）、`avatar_compositor.cc`（贴图）、`lip_sync_controller.cc`（时钟）

**不需要从零猜的部分**：图片叫什么、放哪、情绪字符串怎么映射、viseme JSON 字段含义、一句 TTS 的消息顺序——本文已写明。  
**仍需读现网代码的部分**：音频管线、`played_samples` 从哪取、LVGL 对象树、板级 `board_type` 上报字段名。

---

## 1. 最终效果（必须做成这样）

对话页是一张分层脸，不是整脸 GIF：

1. **待机 / 聆听**：显示「当前情绪」的上半脸 + 该情绪的闲置嘴；约 2～3 秒眨一次眼。
2. **说话**：上半脸和 overlay **保持当前情绪**；只换嘴层。嘴由服务端 `type:viseme` 时间轴驱动，时钟绑喇叭已播采样，不是绑字幕、也不是绑收包。
3. **句末 / 打断**：嘴回到 SIL（闭或微闭），然后若已停说，切回该情绪的闲置嘴。
4. **禁止**：用 speaking 整脸 clip、队列压力假嘴、两张完整嘴 50% 透明叠出「双嘴唇」作为最终方案。

绘制顺序固定，不可调换：

```text
base_360.png          360×360 @ (0, 0)      肤色/头发/身体，整脸底
     ↓
upper_*.png           220×105 @ (70, 105)   眉、眼、鼻梁；下沿结束于 Y=209
     ↓
mouth / viseme / transition
                      160×100 @ (100, 210)  嘴；上沿从 Y=210 开始，与 upper 不重叠
     ↓
overlay_*.png         原尺寸 @ 各自坐标     仅 overlay 带 Alpha；blush/泪/汗/问号/星/心
```

**硬规则**

- `upper_*` 与所有嘴图（情绪嘴、viseme、transition）都是**完全不透明** PNG。
- 设备端禁止对这些层做缩放、旋转、recolor、额外透明度。
- 只有 overlay 做透明混合。
- 分界必须贴在 **Y=209 / Y=210**。改坐标就会在鼻下/上唇出现色块或缝。

---

## 2. 设备端模块怎么串起来

当前固件（`voiceshow`）里，对话脸走 PNG 合成，不再用 13 条 emote GIF 驱动口型。

| 步骤 | 谁 | 做什么 |
|------|----|--------|
| 1 | MQTT/WS JSON `type=llm` 的 `emotion` | `Application` → `Display::SetEmotion` → `AvatarCompositor::SetEmotion` |
| 2 | JSON `type=tts` `state=sentence_start` | `LipSyncController::ArmUtterance(index)` |
| 3 | JSON `type=viseme` | `LipSyncController::LoadTimeline(index, events)` |
| 4 | 本句第一包 PCM 写入 DAC 前 | `MarkAnchorIfArmed(played_samples)` |
| 5 | 渲染约 40ms 一拍 | `ExpressionController::TickMouthFromPlayback` → `LipSyncController::Tick(played_samples)` → `AvatarCompositor::SetVisemeId` |
| 6 | `tts.stop` / 用户打断 | `LipSyncController::Reset()`，嘴回 SIL / 闲置情绪嘴 |

**完整消息格式、时序、板型门控见第 13 节。**

关键源码（仓库 `voiceshow/`）：

| 文件 | 职责 |
|------|------|
| `main/display/avatar/avatar_compositor.cc` | 加载 63 张 PNG、叠层、情绪 Pose、眨眼、viseme/过渡/能量假嘴 |
| `main/display/lipsync/lip_sync_controller.cc` | viseme 时间轴 + DAC `played_samples` 时钟 |
| `main/display/lipsync/viseme_types.h` | `zh_15` ID 0～14 |
| `main/display/vocat_lvgl/components/expression_controller.cc` | 说话时每拍取 viseme；轴是 SIL 但喇叭还在响则走能量档 |
| `main/application.cc` | 解析 `llm` / `tts` / `viseme` JSON |

固件从 **assets 分区**按**文件名**取图，例如 `base_360.png`。本包 `runtime/` 里的名字必须与代码 `LOAD(base_360, …)` 一致。

---

## 3. 图层坐标与尺寸（全部）

画布 360×360。安装坐标与 `avatar_manifest.json` / `AvatarCompositor` 常量一致。

| 图层 | 运行时文件 | 像素 | 贴图位置 (x, y) | 不透明度 |
|------|------------|------|-----------------|----------|
| 底图 | `runtime/base_360.png` | 360×360 | (0, 0) | 不透明 |
| 上半脸 | `runtime/upper_<状态>.png` | 220×105 | (70, 105) | 不透明 |
| 眨眼 35% | `runtime/upper_blink_35.png` | 220×105 | (70, 105) | 不透明 |
| 眨眼 80% | `runtime/upper_blink_80.png` | 220×105 | (70, 105) | 不透明 |
| 闭眼 | `runtime/upper_closed.png` | 220×105 | (70, 105) | 不透明 |
| 情绪闲置嘴 | `runtime/mouth_<状态>.png` | 160×100 | (100, 210) | 不透明 |
| Viseme 嘴 | `runtime/viseme_*.png` | 160×100 | (100, 210) | 不透明 |
| 过渡嘴 | `runtime/transition_*.png` | 160×100 | (100, 210) | 不透明 |
| 脸红 | `runtime/overlay_blush.png` | 220×70 | (70, 184) | Alpha |
| 泪 | `runtime/overlay_tear.png` | 32×52 | (224, 179) | Alpha |
| 汗 | `runtime/overlay_sweat.png` | 42×52 | (269, 92) | Alpha |
| 问号 | `runtime/overlay_question.png` | 46×64 | (273, 63) | Alpha |
| 星光 | `runtime/overlay_sparkle.png` | 58×58 | (267, 139) | Alpha |
| 爱心 | `runtime/overlay_heart.png` | 64×58 | (270, 125) | Alpha |

伪代码：

```cpp
SetImg(base,     "base_360.png",           0,   0);
SetImg(upper,    "upper_happy.png",       70, 105);   // 说话时仍用情绪 upper
SetImg(mouth,    "viseme_10_aa.png",     100, 210);   // 只换这一张
SetImg(overlay,  "overlay_heart.png",    270, 125);   // 可隐藏
```

`preview/` 里是已经叠好的 360×360 整脸，用来对照你合成对不对，**不要**当运行时贴图。

---

## 4. 14 套对话表情（上半脸 + 闲置嘴）

每套必须成对：`upper_<id>.png` + `mouth_<id>.png`。说话时 **upper 不变**，嘴改 viseme。

| 状态 id | 上半脸文件 | 闲置嘴文件 | 观感 |
|---------|------------|------------|------|
| `neutral` | `upper_neutral.png` | `mouth_neutral.png` | 默认脸、放松、说话默认底 |
| `happy` | `upper_happy.png` | `mouth_happy.png` | 高兴、大笑、滑稽 |
| `sad` | `upper_sad.png` | `mouth_sad.png` | 难过 |
| `angry` | `upper_angry.png` | `mouth_angry.png` | 生气 |
| `surprised` | `upper_surprised.png` | `mouth_surprised.png` | 惊讶、震惊 |
| `sleepy` | `upper_sleepy.png` | `mouth_sleepy.png` | 困、累 |
| `thinking` | `upper_thinking.png` | `mouth_thinking.png` | 思考、疑惑 |
| `focused` | `upper_focused.png` | `mouth_focused.png` | 专注、侧目/聆听 |
| `playful` | `upper_playful.png` | `mouth_playful.png` | 眨眼俏皮底 |
| `shy` | `upper_shy.png` | `mouth_shy.png` | 害羞（常加 blush） |
| `crying` | `upper_crying.png` | `mouth_crying.png` | 哭（常加泪） |
| `silly` | `upper_silly.png` | `mouth_silly.png` | 调皮、吃东西 |
| `loving` | `upper_loving.png` | `mouth_loving.png` | 亲近（常加爱心） |
| `cool` | `upper_cool.png` | `mouth_cool.png` | 酷、自信 |

眨眼三张**不属于**某一种情绪，所有脸上共用：

| 文件 | 用途 |
|------|------|
| `upper_blink_35.png` | 眨眼序列第 1、5 拍 |
| `upper_blink_80.png` | 第 2、4 拍 |
| `upper_closed.png` | 第 3 拍全闭 |

现网眨眼：40ms timer；空闲约 50+ 拍（约 2.0～3.2s）后走 `35 → 80 → closed → 80 → 35 → 当前情绪 upper`。说话时也可以眨，只换 upper，不换 viseme 嘴。

### 4.1 云端 `llm.emotion` 字符串 → 哪套脸 + 哪个 overlay

服务端下发例如：`{"type":"llm","emotion":"laughing"}`。设备 `AvatarCompositor::PoseForEmotion` 映射如下（大小写按固件 `strcmp`，请原样匹配）。

| 云端 emotion（及别名） | 使用的 upper/mouth 状态 | overlay 文件 | 说话时是否藏 overlay |
|------------------------|-------------------------|--------------|----------------------|
| `happy` `laughing` `funny` `paishou` | happy | 无 | — |
| `loving` `kissy` `love` | loving | `overlay_heart.png` @ (270,125) | 否 |
| `embarrassed` `shy` | shy | `overlay_blush.png` @ (70,184) | 否 |
| `crying` `cry` | crying | `overlay_tear.png` @ (224,179) | 否 |
| `sad` | sad | 无 | — |
| `angry` | angry | 无 | — |
| `surprised` `shocked` `surprise` `insert` | surprised | 无 | — |
| `thinking` `think` `question` `book` | thinking | `overlay_question.png` @ (273,63) | **是**（说话藏问号） |
| `confused` `dizzy` `nauseated` | thinking | `overlay_sweat.png` @ (269,92) | 否 |
| `silly` `playful` | silly | 无 | — |
| `winking` `wink` | playful | `overlay_sparkle.png` @ (267,139) | 否 |
| `delicious` `eat` | silly | `overlay_sparkle.png` | 否 |
| `sleepy` `sleep` `tired` `tried` | sleepy | 无 | — |
| `cool` | cool | 无 | — |
| `confident` | cool | `overlay_sparkle.png` | 否 |
| `listening` `focused` `look_left` `look_right` `look_around` | focused | 无 | — |
| `idle` `relaxed` `neutral` `speaking` 及其它未知 | **neutral** | 无 | — |

未列出的字符串一律走 **neutral**。  
`preview/` 文件名与状态对应：`happy_preview.png` = 底图+happy 上半脸+happy 闲置嘴的整脸效果，依此类推。

---

## 5. 口型 Viseme（说话时真正换的嘴）

集合名 **`zh_15`**，ID 整数 **0～14**。运行时文件必须叫下面这些名字（不要带 `_160x100` 后缀）。

| ID | 符号 | 运行时文件 | 嘴形 | 典型音 |
|----|------|------------|------|--------|
| 0 | SIL | `viseme_00_SIL.png` | 闭或微闭 | 停顿、句末 |
| 1 | PP | `viseme_01_PP.png` | 双唇闭合 | 妈、波、摸 |
| 2 | FF | `viseme_02_FF.png` | 上齿咬下唇 | 发、飞 |
| 3 | TH | `viseme_03_TH.png` | 微张、齿缝 | 思、四 |
| 4 | DD | `viseme_04_DD.png` | 微开 | 的、地 |
| 5 | kk | `viseme_05_kk.png` | 中等开口 | 哥、可 |
| 6 | CH | `viseme_06_CH.png` | 扁唇前伸 | 机、七、知 |
| 7 | SS | `viseme_07_SS.png` | 扁唇微开 | 丝 |
| 8 | nn | `viseme_08_nn.png` | 微开鼻音 | 嗯 |
| 9 | RR | `viseme_09_RR.png` | 圆唇卷舌 | 儿、日 |
| 10 | aa | `viseme_10_aa.png` | 标准大开「啊」 | 啊、大 |
| 11 | E | `viseme_11_E.png` | 扁开 | 耶、别 |
| 12 | I | `viseme_12_I.png` | 嘴角拉宽 | 一、你、米 |
| 13 | O | `viseme_13_O.png` | 圆唇开 | 哦、多 |
| 14 | U | `viseme_14_U.png` | 圆唇小开 | 乌、出 |

同一 ID=10 还有两档**强度**（能量假嘴 / 演唱用，不另占 ID）：

| 文件 | 何时用 |
|------|--------|
| `viseme_10_aa_small.png` | 能量档 1：小声、轴还是 SIL 但喇叭已有声 |
| `viseme_10_aa.png` | 能量档 2，或时间轴 id=10 |
| `viseme_10_aa_open.png` | 能量档 3：最大开口 |

现网 `SetVisemeId(10)` 用标准 `viseme_10_aa.png`。`aa_small` / `aa_open` 走 `SetFallbackMouthLevel(1/3)`。

非法 id 或缺图：回退 `viseme_00_SIL.png`。

### 5.1 服务端 JSON 字段（设备解析子集）

与 `tts.sentence_start` 的 `index` 对齐。`time_ms` 相对**这一句第一个 PCM 写入 DAC 的时刻**，不是相对 JSON 到达时刻。

**完整包体、下发顺序、`source` 含义见第 13.5 节。** 最小示例如下：

```json
{
  "type": "viseme",
  "viseme_set_id": "zh_15",
  "index": 1,
  "visemes": [
    { "time_ms": 0,   "duration_ms": 80,  "id": 0,  "blend_ms": 0 },
    { "time_ms": 80,  "duration_ms": 140, "id": 1,  "blend_ms": 40 },
    { "time_ms": 220, "duration_ms": 180, "id": 10, "blend_ms": 60 },
    { "time_ms": 400, "duration_ms": 120, "id": 12, "blend_ms": 40 },
    { "time_ms": 700, "duration_ms": 100, "id": 0,  "blend_ms": 40 }
  ]
}
```

| 字段 | 含义 |
|------|------|
| `index` | 与本句 `tts.sentence_start` 的 index 相同；旧 index 的包要丢弃 |
| `visemes[].time_ms` | 相对本句 DAC 锚点 |
| `visemes[].duration_ms` | 建议保持时长；设备可 clamp 最短约 40ms |
| `visemes[].id` | 0～14 |
| `visemes[].blend_ms` | 进入该 viseme 前与上一张叠化；现网大跨度改用过渡 sprite，约显示 32ms |

采样率 **24000 Hz**。现网时钟：

```text
now_ms = (played_samples - anchor_samples) * 1000 / 24000
当前 id = 时间轴上 time_ms <= now_ms < time_ms+duration_ms 的那一项
```

`tts.sentence_start` → `ArmUtterance`；第一包 PCM 下 DAC 前 → `MarkAnchorIfArmed`。时间轴若比音频晚到，允许在已有 events 时自动锚一次，避免嘴一直闭着。

AI 演唱：歌词 viseme 是另一条轴；没有轴时用播放包络三档嘴（`aa_small` / `aa` / `aa_open`）。普通 TTS 以 `type:viseme` 为主。

---

## 6. 八张口型过渡（大跨度才用）

两张完整嘴做 50% Alpha 会在 RGB565 上出现双唇线。这 8 张是**已画好的中间嘴**，尺寸、坐标与 viseme 完全相同：`160×100 @ (100,210)`。

现网只对下列 **from_id → to_id** 查表，其它边直接切目标 viseme。过渡帧显示约 **32ms** 再切到目标。音素极短时可跳过过渡。新 viseme 在过渡未结束时到达：取消过渡，立刻切新目标，不要卡在中间帧。

| from → to | ID | 运行时文件 | 中间形态参考 |
|-----------|----|------------|--------------|
| SIL → aa | 0→10 | `transition_SIL_to_aa.png` | 小开口，接近 aa_small |
| aa → SIL | 10→0 | `transition_aa_to_SIL.png` | 同上方向收回 |
| PP → aa | 1→10 | `transition_PP_to_aa.png` | 闭唇到啊 |
| aa → PP | 10→1 | `transition_aa_to_PP.png` | 啊收回闭唇 |
| I → aa | 12→10 | `transition_I_to_aa.png` | 扁嘴到啊，接近 E |
| aa → I | 10→12 | `transition_aa_to_I.png` | 啊回到扁嘴 |
| SIL → O | 0→13 | `transition_SIL_to_O.png` | 闭到圆唇，接近 U |
| O → SIL | 13→0 | `transition_O_to_SIL.png` | 圆唇收回 |

文件名大小写必须一致：`SIL`、`PP`、`aa`、`I`、`O`（`aa` 小写）。

---

## 7. 能量假嘴（没有 viseme 轴，或轴是 SIL 但喇叭还在响）

`ExpressionController` 读播放包络：

- 有时间轴且当前 id≠SIL：按 viseme 贴嘴。
- 有时间轴但 id=SIL，且包络 ≥ 80：不要一直闭嘴，改用档位 1/2/3。
- 完全没有时间轴（例如演唱轴未到）：同样走档位。

| 档位 | 图 |
|------|----|
| 0 | `viseme_00_SIL.png` |
| 1 | `viseme_10_aa_small.png` |
| 2 | `viseme_10_aa.png` |
| 3 | `viseme_10_aa_open.png` |

档位要限速升降，避免每 40ms 乱跳。

---

## 8. `runtime/` 全文件清单（63 张，缺一不可）

加载时按**文件名**取，不要改名。源素材在仓库 `asset/avatar_layers_v5/` 等目录里带 `_160x100` / `_full` 后缀；打进固件前已改成下表名字。

### 8.1 底图（1）

| 运行时路径 | 尺寸 | 坐标 | 说明 |
|------------|------|------|------|
| `runtime/base_360.png` | 360×360 | (0,0) | 唯一整脸底。头发、身体、脸型肤色。upper/嘴贴在这张上面。 |

### 8.2 上半脸（17）

| 运行时路径 | 尺寸 | 坐标 | 说明 |
|------------|------|------|------|
| `runtime/upper_neutral.png` | 220×105 | (70,105) | 默认 / relaxed / idle |
| `runtime/upper_happy.png` | 220×105 | (70,105) | 高兴 |
| `runtime/upper_sad.png` | 220×105 | (70,105) | 难过 |
| `runtime/upper_angry.png` | 220×105 | (70,105) | 生气 |
| `runtime/upper_surprised.png` | 220×105 | (70,105) | 惊讶 |
| `runtime/upper_sleepy.png` | 220×105 | (70,105) | 困 |
| `runtime/upper_thinking.png` | 220×105 | (70,105) | 思考/疑惑 |
| `runtime/upper_focused.png` | 220×105 | (70,105) | 专注聆听 |
| `runtime/upper_playful.png` | 220×105 | (70,105) | 俏皮眨眼底 |
| `runtime/upper_shy.png` | 220×105 | (70,105) | 害羞 |
| `runtime/upper_crying.png` | 220×105 | (70,105) | 哭 |
| `runtime/upper_silly.png` | 220×105 | (70,105) | 调皮 |
| `runtime/upper_loving.png` | 220×105 | (70,105) | 亲近 |
| `runtime/upper_cool.png` | 220×105 | (70,105) | 酷 |
| `runtime/upper_blink_35.png` | 220×105 | (70,105) | 眨眼 35% |
| `runtime/upper_blink_80.png` | 220×105 | (70,105) | 眨眼 80% |
| `runtime/upper_closed.png` | 220×105 | (70,105) | 全闭眼 |

源文件对照（仅归档，不要用源文件名去 `fopen`）：

- V5：`asset/avatar_layers_v5/<state>_upper_full.png` → `upper_<state>.png`
- 眨眼：`blink_35_upper_full.png` → `upper_blink_35.png`，`closed_upper_full.png` → `upper_closed.png`
- V5.1：`asset/avatar_emotion_extension_v5_1/<state>_upper_full.png`

### 8.3 情绪闲置嘴（14）

未说话时显示，与当前 upper 成对。坐标一律 (100,210)。

| 运行时路径 | 对应状态 |
|------------|----------|
| `runtime/mouth_neutral.png` | neutral |
| `runtime/mouth_happy.png` | happy |
| `runtime/mouth_sad.png` | sad |
| `runtime/mouth_angry.png` | angry |
| `runtime/mouth_surprised.png` | surprised |
| `runtime/mouth_sleepy.png` | sleepy |
| `runtime/mouth_thinking.png` | thinking |
| `runtime/mouth_focused.png` | focused |
| `runtime/mouth_playful.png` | playful |
| `runtime/mouth_shy.png` | shy |
| `runtime/mouth_crying.png` | crying |
| `runtime/mouth_silly.png` | silly |
| `runtime/mouth_loving.png` | loving |
| `runtime/mouth_cool.png` | cool |

源：`*_mouth_full.png` → `mouth_*.png`。

### 8.4 Viseme 嘴（17）

坐标一律 (100,210)。见第 5 节。源：`asset/avatar_layers_v5/visemes/viseme_XX_*_160x100.png` → 去掉 `_160x100`。

### 8.5 过渡嘴（8）

坐标一律 (100,210)。见第 6 节。源：`asset/avatar_transitions_v5_2/transitions/transition_*_160x100.png`。

### 8.6 Overlay（6）

| 运行时路径 | 尺寸 | 坐标 | 典型情绪 |
|------------|------|------|----------|
| `runtime/overlay_blush.png` | 220×70 | (70,184) | shy / embarrassed |
| `runtime/overlay_tear.png` | 32×52 | (224,179) | crying |
| `runtime/overlay_sweat.png` | 42×52 | (269,92) | confused |
| `runtime/overlay_question.png` | 46×64 | (273,63) | thinking（说话时藏） |
| `runtime/overlay_sparkle.png` | 58×58 | (267,139) | wink / delicious / confident |
| `runtime/overlay_heart.png` | 64×58 | (270,125) | loving / kissy |

V5.1 源包里的 `overlay_shy_blush.png` 等与 V5 `overlay_blush.png` 等同图；固件只加载上表 6 个短名。

---

## 9. `preview/`（17 张，不进固件）

整脸 360×360，用于验收合成。文件名：

| 文件 | 对应 runtime 组合 |
|------|-------------------|
| `preview/neutral_preview.png` | base + upper_neutral + mouth_neutral |
| `preview/happy_preview.png` | base + upper_happy + mouth_happy |
| `preview/sad_preview.png` | … |
| `preview/angry_preview.png` | … |
| `preview/surprised_preview.png` | … |
| `preview/sleepy_preview.png` | … |
| `preview/thinking_preview.png` | … |
| `preview/focused_preview.png` | … |
| `preview/playful_preview.png` | … |
| `preview/shy_preview.png` | V5.1 shy |
| `preview/crying_preview.png` | V5.1 crying |
| `preview/silly_preview.png` | V5.1 silly |
| `preview/loving_preview.png` | V5.1 loving |
| `preview/cool_preview.png` | V5.1 cool |
| `preview/blink_35_preview.png` | 眨眼 35% 整脸 |
| `preview/blink_80_preview.png` | 眨眼 80% |
| `preview/closed_preview.png` | 闭眼整脸 |

你的合成应与对应 preview 在鼻下接缝、肤色上一致。

---

## 10. 新设备最小实现步骤

1. 建 360×360 画布，四层 Image：base / upper / mouth / overlay。
2. 把 `runtime/` 63 张按**同名**放进资源系统。
3. 启动：`base_360` + `upper_neutral` + `mouth_neutral`，overlay 隐藏。
4. 收到 `emotion`：查第 4.1 节表，换 upper+闲置嘴+overlay。
5. 进入 speaking：upper/overlay 保持；嘴先 `viseme_00_SIL`。
6. 解析 `type:viseme`，用 DAC 已播采样算 `now_ms`，查 id，贴对应 `viseme_XX_*.png`；若在第 6 节 8 条边上，先贴 transition 约 32ms。
7. 停说：闲置嘴回到当前情绪的 `mouth_*.png`。
8. 本地 40ms 定时器做眨眼，只替换 upper。
9. 不要缩放 mouth/upper。

---

## 11. 验收（给下一块板）

1. 待机 30 秒：额头、眼周、鼻下、嘴周无色块。
2. 完整眨眼：无闪白、灰边。
3. 手动连切 15 个 viseme + 8 个 transition：下唇完整，Y=210 无缝。
4. 14 种情绪各切一次：upper 与 mouth 成对，接缝干净。
5. 播一句「妈妈你好」：嘴跟喇叭走，不要跟字幕；句末收到 SIL。
6. 高兴时说话：眼睛仍是 happy，只有嘴在变。
7. RGB565 上不要用两张嘴 Alpha 混合。

---

## 12. 不要做的事

- 不要用 13 条 emote GIF（angry/happy/speaking…）当最终口型。那是旧降级路径。
- 不要把源目录里的 `viseme_10_aa_160x100.png` 原名直接写进加载代码；运行时名是 `viseme_10_aa.png`。
- 不要把 `preview/` 打进固件。
- 不要移动 (70,105) / (100,210)；不要让 upper 盖住嘴或嘴盖住眼。
- 设备只负责按协议贴图和跟 DAC 时钟；viseme 时间轴由服务端生成（见第 14 节）。

---

## 13. 服务端下发协议（设备接入必读）

设备与云端通过 **WebSocket（或 MQTT 信令 + UDP 音频）** 通信。与「脸」相关的 JSON 都走 **WebSocket 文本帧**；TTS 音频是 **Opus 二进制帧**（或 UDP），与 JSON 分开。

### 13.1 板型门控（不下发就没有口型轴）

服务端 `supportsVisemeDownlink()` 仅在设备 OTA 上报的 `board_type == "esp-show"` 时下发 `type:viseme`。

| board_type | viseme 轴 | 说明 |
|------------|-----------|------|
| `esp-show` | **下发** | 当前 VoCat SE / voiceshow 固件用的 SKU |
| `esp-vocat-se-v1_2` 及其它 | **不下发** | 仍可有 `llm.emotion` 换表情，嘴只能靠能量假嘴 |

新设备若要口型同步，须在 OTA/握手时把板型报成 `esp-show`，或在服务端把你的新型号加入白名单（改 `connection_sendmsg.go` 的 `supportsVisemeDownlink`）。

### 13.2 一句 TTS 的标准时序（最重要）

服务端 `sendAudioChunk` 对**每个文本分段**（`index` 从 1 递增）保证顺序：

```text
① type:llm          emotion（可选，每段最多一次）
② type:tts          state=sentence_start, index, text
③ type:viseme       index 对齐, visemes[]
④ Opus/PCM 音频帧   多包，直到该段 EOF
⑤ type:tts          state=sentence_end, index, text   （该段最后一包音频时）
⑥ … 下一段重复 ①～⑤ …
整轮结束：
⑦ type:tts          state=stop
```

一轮对话开始还会有：

```text
type:tts  state=start    （整轮 TTS 开始，设备进 Speaking）
```

**设备必须遵守的响应**

| 收到 | 设备动作 |
|------|----------|
| `tts.start` | 进入播报态；可 `lip_sync.Reset()` |
| `llm.emotion` | `SetEmotion(emotion)` → 换 upper + 闲置嘴 + overlay |
| `tts.sentence_start` | `ArmUtterance(index)`；**同 index 重复包要忽略**（服务端已去重，设备仍应防重） |
| `type:viseme` | `LoadTimeline(index, events)`；`index` 小于当前句的丢弃 |
| 首包 PCM 写 DAC **之前** | `MarkAnchorIfArmed(played_samples)` |
| 渲染定时器 ~40ms | `Tick(played_samples)` → `SetVisemeId(id)` |
| `tts.sentence_end` | 可选：句末字幕处理；嘴由轴内 SIL 事件收口 |
| `tts.stop` | `lip_sync.Reset()`；嘴回当前情绪闲置嘴 |
| 用户实时打断 | `ResetDecoder` + `lip_sync.Reset()`（VoCat SE realtime 模式） |

**时钟铁律**：`visemes[].time_ms` 相对 **本句第一个 PCM 样本写入 DAC 的时刻**，不是相对 JSON 到达、也不是相对 `sentence_start`。

```text
now_ms = (played_samples - anchor_samples) * 1000 / 24000
```

采样率固定 **24000 Hz、单声道**（`viseme.audio.sample_rate` 字段同值）。

### 13.3 `type: llm` — 表情

每段播报前最多发一次（与 `sentence_start` 同 index）。也用于用户口令「换个开心表情」等。

```json
{
  "type": "llm",
  "session_id": "…",
  "emotion": "happy",
  "text": "😊"
}
```

| 字段 | 设备用法 |
|------|----------|
| `emotion` | **必用**。查本文第 4.1 节映射表 → `SetEmotion` |
| `text` | emoji 字符串，可忽略或用于调试；**换脸只看 `emotion`** |

**服务端 emotion 从哪来**（便于联调，设备可不实现）：

1. LLM 回复正文里的 emoji → `FindEmotionsInText`  
2. 正文无 emoji → `InferEmotionFromText` 按关键词推断（每段都会算，避免整轮 stuck 在一个脸上）  
3. 用户显式口令 → `FindExplicitExpressionCommand`，并**锁定到本轮结束**  
4. 第一段可合并用户问题 + 助手首句一起推断  

设备侧只需实现第 4.1 节映射；字符串列表见服务端 `anime-ai-chat-server/src/core/utils/emotion.go` 的 `EmotionEmoji`。

### 13.4 `type: tts` — 播报状态机

```json
{
  "type": "tts",
  "state": "sentence_start",
  "session_id": "…",
  "text": "现在几点了？",
  "index": 1,
  "audio_codec": "opus"
}
```

| state | 何时发 | 设备要点 |
|-------|--------|----------|
| `start` | 本轮开始合成/播放 | 进 Speaking；新轮可 Reset 口型 |
| `sentence_start` | **每段**首包音频前 | `ArmUtterance(index)` + 助手字幕 |
| `sentence_end` | 该段音频 EOF | 段结束标记 |
| `stop` | 整轮播完或被打断 | `Reset()` 口型；回 Listening/Idle |

**注意**

- `sentence_start` 的 `text` 是**助手字幕**，不是用户 STT。  
- 状态字如「演唱中」也会走 `sentence_start`，但**不应** `ArmUtterance`（现网 `IsServerStatusStt` 过滤），否则清掉正在唱的歌词 viseme 轴。  
- 同一句 `sentence_start` 服务端只发一次；重复 Arm 会把 DAC 锚点整体后移，嘴型滞后。

### 13.5 `type: viseme` — 口型时间轴

仅 `board_type=esp-show`。完整字段：

```json
{
  "type": "viseme",
  "protocol_version": 1,
  "viseme_set_id": "zh_15",
  "session_id": "…",
  "utterance_id": "r3_i1",
  "index": 1,
  "round": 3,
  "source": "native_tts_words",
  "audio": { "sample_rate": 24000, "channels": 1 },
  "visemes": [
    { "time_ms": 0,   "duration_ms": 80,  "id": 0,  "blend_ms": 0 },
    { "time_ms": 80,  "duration_ms": 140, "id": 1,  "blend_ms": 40 }
  ]
}
```

| 字段 | 含义 |
|------|------|
| `viseme_set_id` | 固定 `zh_15`；设备可校验，未知集合回退 SIL |
| `index` | 与 `tts.sentence_start.index` **必须相同** |
| `round` | 对话轮次，用于日志；设备可选校验 |
| `utterance_id` | 字符串 `"r{round}_i{index}"`，调试用 |
| `source` | 轴来源（见下表）；设备可打日志，不影响贴图 |
| `visemes[].time_ms` | 相对本句 DAC 锚点（毫秒） |
| `visemes[].duration_ms` | 建议保持时长；设备可 clamp 最短 ~40ms |
| `visemes[].id` | 0～14，见第 5 节 |
| `visemes[].blend_ms` | 进入该 viseme 的叠化建议；大跨度时设备优先用第 6 节过渡 sprite |

**`source` 常见值**（服务端生成方式）

| source | 说明 |
|--------|------|
| `native_tts_words` | 豆包 TTS 字级时间戳 + 拼音→viseme（**最优**） |
| `forced_alignment` | 强制对齐补轴 |
| `pinyin_scaled` | 无字轴时按文本拼音均匀拉伸到整段音频时长（流式句 EOF 补发） |
| `lyric_pinyin_scaled` | AI 演唱歌词估轴 |

**下发策略**

- 有字轴：在**首包音频前**与 `sentence_start` 同批下发（避免先出声后动嘴）。  
- 流式句一直无字轴：在该段 **EOF** 用音频时长补 `pinyin_scaled`。  
- 演唱歌词：走 `sendSingLyricViseme`，**不发** `sentence_start`（避免 Arm 清轴）；设备在已有 events 时可 Auto-anchor。  
- `SuppressViseme` 场景（如纯状态字「演唱中」）：不发 viseme。

### 13.6 其它可能收到的消息（与脸弱相关）

| type | 用途 | 与脸关系 |
|------|------|----------|
| `stt` | 用户/助手字幕 | `role=assistant` 只更新字幕，不触发打断 |
| `listen` | `state`+`mode` 拾音提示 | 实时打断时麦克风保持上行；不改变脸 |
| `alert` / `system` | 系统提示 | 一般不换脸 |

### 13.7 联调检查清单

1. OTA 上报 `board_type=esp-show`，抓包能看到 `type:viseme`。  
2. 每段顺序：`emotion?` → `sentence_start` → `viseme` → 音频。  
3. `viseme.index` 与 `sentence_start.index` 一致。  
4. `MarkAnchorIfArmed` 在首包 PCM **写 DAC 前**调用。  
5. 说「妈妈你好」：嘴跟喇叭，不跟字幕滚动。  
6. 高兴情绪下说话：upper 仍是 happy，只有 mouth 层在变。  
7. 打断后 `tts.stop`：嘴立即回闲置情绪嘴，不卡在 viseme 中间帧。

服务端参考实现：`anime-ai-chat-server/src/core/connection_sendmsg.go`（`sendEmotionMessage` / `sendVisemeMessage` / `sendAudioChunk`）。  
设备参考实现：`voiceshow/main/application.cc`（`type` 分支约 1700～1940 行）。

---

## 14. 统计

| 类别 | 张数 | 目录 |
|------|------|------|
| 底图 | 1 | `runtime/base_360.png` |
| 上半脸（含眨眼） | 17 | `runtime/upper_*.png` |
| 情绪嘴 | 14 | `runtime/mouth_*.png` |
| Viseme + aa 分档 | 17 | `runtime/viseme_*.png` |
| 过渡 | 8 | `runtime/transition_*.png` |
| Overlay | 6 | `runtime/overlay_*.png` |
| **运行时合计** | **63** | `runtime/` |
| 整脸预览 | 17 | `preview/` |

设备对话脸用到的图片就是这 63 张。把 `show11/` 整个目录打包即可交给下一位工程师。
