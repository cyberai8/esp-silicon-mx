# show — voiceshow「跟 AI 对话」显示切换代码整理

从 `voiceshow/` 抽出：**小智 / Home 对话页**上，表情、字幕、口型、状态条在对话过程中如何切换。  
**不是**可编译工程；原路径写在下方，改固件请回 `voiceshow/main/...`。

配套：

| 包 | 内容 |
|----|------|
| 本目录 `show/` | 设备端对话显示相关源码副本 + 协议入口摘录 |
| `show11/` | 分层脸 PNG 资源 + 接入说明 |
| `DESIGN.md` | 2D 数字人最终方案（原根目录 `show.md`） |

---

## 1. 对话时画面怎么变（一条链）

```text
服务端 WS/MQTT JSON
  ├─ type=llm   emotion="laughing"     → SetEmotion → AvatarCompositor 换眉眼/闲置嘴/overlay
  ├─ type=stt   text=用户说话           → SetChatMessage("user", …)     → Home 字幕条
  ├─ type=tts   sentence_start + text  → ArmUtterance + SetChatMessage("assistant", …)
  ├─ type=viseme visemes=[…]           → LipSyncController::LoadTimeline
  └─ type=tts   stop                   → LipSync Reset + 清空 assistant 字幕

设备状态机（application SetDeviceState）
  idle / connecting / listening / speaking
    → SetStatus + SetEmotion("neutral") 等

音频 DAC
  本句首包 PCM 写入前 → MarkAnchorIfArmed(played_samples)
  渲染 ~40ms → ExpressionController::TickMouthFromPlayback
              → LipSyncController::Tick(played_samples)
              → AvatarCompositor::SetVisemeId（只换嘴，表情 upper 保留）
```

---

## 2. 目录结构（相对本包）

```text
show/
  README.md                          ← 本说明
  DESIGN.md                          ← 方案冻结文档
  src/
    application/
      protocol_tts_stt_llm_viseme.extract.cc   ← TTS/STT/LLM/Viseme 入口摘录
      device_state_display_switch.extract.cc   ← 状态→状态条/表情摘录
      alert_emotion_chat.extract.cc            ← Alert 表情+字幕摘录
    audio/
      played_samples_lip_sync_anchor.extract.cc ← DAC 时钟锚点摘录
    display/
      display.h                      ← SetEmotion / SetChatMessage 基类 API
      avatar/                        ← 分层 PNG 合成（表情+口型）
      lipsync/                       ← viseme 时间轴 + played_samples
      emote/emote_mapping.*          ← 云端 emotion → 旧 13 clip（降级/临时脸）
      vocat/vocat_display.h          ← IVocatDisplay（含 Home / 临时表情）
      vocat_lvgl/
        vocat_lvgl_display.h         ← LVGL 实现声明
        screens/screen_home.cc       ← Home 脸 UI、字幕条、ApplyHomeEmotion
        components/expression_*      ← 表情控制器 + 几何脸降级
        components/chat_widgets.h
        components/listen_indicator.*
        extracts/vocat_lvgl_display_chat_methods.extract.cc
    ring/ring_page_home.cc           ← 圆环进 Home 对话路由
```

---

## 3. 文件 ↔ 职责 ↔ 原路径

| 本包路径 | 职责 | voiceshow 原路径 |
|----------|------|------------------|
| `src/display/avatar/*` | 底图/上半脸/嘴/overlay；`SetEmotion` / `SetVisemeId` | `main/display/avatar/` |
| `src/display/lipsync/*` | Arm / LoadTimeline / Tick / Reset | `main/display/lipsync/` |
| `src/display/vocat_lvgl/components/expression_controller.*` | 说话每拍取 viseme；无轴时能量假嘴 | `main/display/vocat_lvgl/components/` |
| `src/display/vocat_lvgl/components/expression_view.*` | 无 Avatar 时的几何表情降级 | 同上 |
| `src/display/vocat_lvgl/screens/screen_home.cc` | 建 Home 脸、字幕带、ApplyHomeEmotion | `.../screens/screen_home.cc` |
| `src/display/vocat_lvgl/extracts/*` | `SetStatus`/`SetChatMessage`/`SetEmotion` 实现摘录 | `.../vocat_lvgl_display.cc` |
| `src/display/emote/emote_mapping.*` | emotion 字符串 → clip 名 | `main/display/emote/` |
| `src/application/protocol_*.extract.cc` | JSON：`tts`/`stt`/`llm`/`viseme` | `main/application.cc` |
| `src/application/device_state_*.extract.cc` | idle/connecting/listening/speaking UI | `main/application.cc` |
| `src/audio/played_samples_*.extract.cc` | 口型 DAC 时钟 | `main/audio/audio_service.{h,cc}` |
| `src/ring/ring_page_home.cc` | 进 xiaozhi 对话页 | `main/display/ring_page_home.cc` |

资源图见 **`../show11/runtime/`**（`base_360.png`、`upper_*`、`mouth_*`、`viseme_*` 等）。

---

## 4. 对话中「显示切换」对照表

| 时机 | 显示变化 | 代码入口 |
|------|----------|----------|
| 进 Home / 待机 | 状态「待命」、表情 neutral、清字幕 | `device_state_display_switch` → idle |
| 开麦连接 | 状态「连接中」、neutral | → connecting |
| 聆听 | 状态「聆听中」、neutral；ListenIndicator | → listening |
| 用户说话识别 | 字幕 role=user | `type=stt` → `SetChatMessage("user")` |
| LLM 情绪 | 换上半脸+闲置嘴+overlay | `type=llm` → `SetEmotion` |
| AI 开始一句 | Arm 口型；字幕 role=assistant | `tts.sentence_start` |
| Viseme 轴到达 | 载入时间轴 | `type=viseme` → `LoadTimeline` |
| 喇叭出声 | 嘴跟 Viseme（或能量档） | `MarkAnchor` + `TickMouthFromPlayback` |
| TTS 结束 | Reset 口型；清 assistant 字幕 | `tts.stop` |
| Alert / 错误 | 临时表情 + system 文案 | `Alert` 摘录 |

---

## 5. 建议阅读顺序

1. `src/application/protocol_tts_stt_llm_viseme.extract.cc` — 服务端驱动什么  
2. `src/display/vocat_lvgl/extracts/vocat_lvgl_display_chat_methods.extract.cc` — UI 入口  
3. `src/display/vocat_lvgl/screens/screen_home.cc` — Home 脸与字幕  
4. `src/display/vocat_lvgl/components/expression_controller.cc` — 表情 vs 口型每拍  
5. `src/display/avatar/avatar_compositor.cc` — 真正贴哪张图  
6. `src/display/lipsync/lip_sync_controller.cc` + `src/audio/played_samples_*.extract.cc` — 时钟  
7. `DESIGN.md` / `../show11/README.md` — 目标效果与资源命名  

---

## 6. 刻意未收录（非「AI 对话脸」主路径）

圆环其它功能页（音乐/闹钟/设置/足球等）、整份 `vocat_lvgl_display.cc`（仅摘对话相关方法）、配网二维码/OTA 进度、OpenClaw/听写页字幕旁路。  
需要完整编译请直接打开 `voiceshow/`。
