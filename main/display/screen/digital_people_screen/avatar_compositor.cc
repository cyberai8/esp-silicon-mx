#include "avatar_compositor.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state.h"
#include "screen_util.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "esp_random.h"

namespace {

constexpr const char* TAG = "AvatarCompositor";

constexpr int32_t kCanvas = 360;
constexpr int32_t kUpperX = 70;
constexpr int32_t kUpperY = 105;
constexpr int32_t kMouthX = 100;
constexpr int32_t kMouthY = 210;
constexpr uint32_t kTickMs = 40;
constexpr uint32_t kVisemeSampleRate = 24000;
constexpr int kTransitionTicks = 1;  // ~40ms，接近现网 32ms
// 补偿 I2S DMA/功放延迟：嘴型相对已写入采样略提前，避免“开口偏晚”。
constexpr uint32_t kLipLeadMs = 80;
constexpr size_t kPathMax = 40;
constexpr size_t kMaxVisemes = 256;
// 句首 SIL 也可能几乎无声，不能用峰值门槛挡住锚点。
constexpr uint32_t kPcmAnchorPeakMin = 0;
constexpr size_t kStateNameMax = 16;
constexpr size_t kOverlayNameMax = 24;

struct OverlayGeom {
    const char* name;
    int32_t x;
    int32_t y;
};

constexpr OverlayGeom kOverlays[] = {
    {"overlay_blush", 70, 184},
    {"overlay_tear", 224, 179},
    {"overlay_sweat", 269, 92},
    {"overlay_question", 273, 63},
    {"overlay_sparkle", 267, 139},
    {"overlay_heart", 270, 125},
};

struct EmotionPose {
    const char* emotion;
    const char* state;
    const char* overlay;  // nullptr = 无
    bool hide_overlay_when_speaking;
};

constexpr EmotionPose kEmotionMap[] = {
    {"happy", "happy", nullptr, false},
    {"laughing", "happy", nullptr, false},
    {"funny", "happy", nullptr, false},
    {"paishou", "happy", nullptr, false},
    {"loving", "loving", "overlay_heart", false},
    {"kissy", "loving", "overlay_heart", false},
    {"love", "loving", "overlay_heart", false},
    {"embarrassed", "shy", "overlay_blush", false},
    {"shy", "shy", "overlay_blush", false},
    {"crying", "crying", "overlay_tear", false},
    {"cry", "crying", "overlay_tear", false},
    {"sad", "sad", nullptr, false},
    {"angry", "angry", nullptr, false},
    {"surprised", "surprised", nullptr, false},
    {"shocked", "surprised", nullptr, false},
    {"surprise", "surprised", nullptr, false},
    {"insert", "surprised", nullptr, false},
    {"thinking", "thinking", "overlay_question", true},
    {"think", "thinking", "overlay_question", true},
    {"question", "thinking", "overlay_question", true},
    {"book", "thinking", "overlay_question", true},
    {"confused", "thinking", "overlay_sweat", false},
    {"dizzy", "thinking", "overlay_sweat", false},
    {"nauseated", "thinking", "overlay_sweat", false},
    {"silly", "silly", nullptr, false},
    {"playful", "silly", nullptr, false},
    {"winking", "playful", "overlay_sparkle", false},
    {"wink", "playful", "overlay_sparkle", false},
    {"delicious", "silly", "overlay_sparkle", false},
    {"eat", "silly", "overlay_sparkle", false},
    {"sleepy", "sleepy", nullptr, false},
    {"sleep", "sleepy", nullptr, false},
    {"tired", "sleepy", nullptr, false},
    {"tried", "sleepy", nullptr, false},
    {"cool", "cool", nullptr, false},
    {"confident", "cool", "overlay_sparkle", false},
    {"listening", "focused", nullptr, false},
    {"focused", "focused", nullptr, false},
    {"look_left", "focused", nullptr, false},
    {"look_right", "focused", nullptr, false},
    {"look_around", "focused", nullptr, false},
    {"idle", "neutral", nullptr, false},
    {"relaxed", "neutral", nullptr, false},
    {"neutral", "neutral", nullptr, false},
    {"speaking", "neutral", nullptr, false},
};

const char* kVisemeStems[] = {
    "viseme_00_SIL",
    "viseme_01_PP",
    "viseme_02_FF",
    "viseme_03_TH",
    "viseme_04_DD",
    "viseme_05_kk",
    "viseme_06_CH",
    "viseme_07_SS",
    "viseme_08_nn",
    "viseme_09_RR",
    "viseme_10_aa",
    "viseme_11_E",
    "viseme_12_I",
    "viseme_13_O",
    "viseme_14_U",
};

const char* kEnergyStems[] = {
    "viseme_00_SIL",
    "viseme_10_aa_small",
    "viseme_10_aa",
    "viseme_10_aa_open",
};

struct TransitionEdge {
    uint8_t from;
    uint8_t to;
    const char* stem;
};

constexpr TransitionEdge kTransitions[] = {
    {0, 10, "transition_SIL_to_aa"},
    {10, 0, "transition_aa_to_SIL"},
    {1, 10, "transition_PP_to_aa"},
    {10, 1, "transition_aa_to_PP"},
    {12, 10, "transition_I_to_aa"},
    {10, 12, "transition_aa_to_I"},
    {0, 13, "transition_SIL_to_O"},
    {13, 0, "transition_O_to_SIL"},
};

struct Ui {
    lv_obj_t* canvas = nullptr;
    lv_obj_t* base = nullptr;
    lv_obj_t* upper = nullptr;
    lv_obj_t* mouth = nullptr;
    lv_obj_t* overlay = nullptr;
    lv_timer_t* tick = nullptr;
    char base_path[kPathMax]{};
    char upper_path[kPathMax]{};
    char mouth_path[kPathMax]{};
    char overlay_path[kPathMax]{};
};

Ui s_ui;

char s_state[kStateNameMax] = "neutral";
char s_overlay[kOverlayNameMax] = "";
bool s_hide_overlay_speaking = false;
bool s_speaking = false;

int s_blink_idle = 0;
int s_blink_next = 50;
int s_blink_step = -1;  // -1 = 不在眨眼

char s_mouth_applied[kPathMax] = "";

std::mutex s_lip_mu;
int s_armed_index = 0;
bool s_need_anchor = false;
bool s_anchored = false;
uint64_t s_anchor_samples = 0;
bool s_utterance_anchor_valid = false;
uint64_t s_utterance_anchor_samples = 0;
bool s_await_pcm_anchor = false;
bool s_utterance_armed = false;
uint64_t s_speaking_started_played = 0;
bool s_pending_pcm_valid = false;
uint64_t s_pending_pcm_start = 0;
AvatarCompositor::VisemeEvent s_events[kMaxVisemes];
size_t s_event_count = 0;
int s_current_viseme = -1;
int s_last_logged_viseme = -1;
uint32_t s_last_logged_now_ms = 0;
int s_trans_left = 0;
const char* s_trans_stem = nullptr;
int s_energy_level = 0;

// audio_output 栈很小：NotifyPcmOutput 只能写原子量，禁止 mutex/日志。
std::atomic<bool> s_pcm_want_first{false};
std::atomic<bool> s_pcm_first_ready{false};
std::atomic<uint64_t> s_pcm_first_start{0};
std::atomic<uint32_t> s_pcm_first_peak{0};

uint64_t CurrentPlayedSamples() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    return codec != nullptr ? codec->GetPlayedSamples() : 0;
}

int CurrentOutputSampleRate() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr && codec->output_sample_rate() > 0) {
        return codec->output_sample_rate();
    }
    return static_cast<int>(kVisemeSampleRate);
}

uint32_t PlayedToMs(uint64_t played, uint64_t anchor, int sample_rate) {
    if (sample_rate <= 0) {
        sample_rate = static_cast<int>(kVisemeSampleRate);
    }
    const uint64_t delta = played > anchor ? played - anchor : 0;
    return static_cast<uint32_t>((delta * 1000ULL) /
                                 static_cast<uint64_t>(sample_rate));
}

void ApplyUtteranceAnchorLocked() {
    if (!s_utterance_anchor_valid || s_event_count == 0) {
        return;
    }
    s_anchor_samples = s_utterance_anchor_samples;
    s_anchored = true;
    s_need_anchor = false;
    s_await_pcm_anchor = false;
    const uint64_t played = CurrentPlayedSamples();
    const int sr = CurrentOutputSampleRate();
    const uint32_t now_ms = PlayedToMs(played, s_anchor_samples, sr);
    ESP_LOGI(TAG,
             "lip anchor pcm=%llu played=%llu now_ms=%u events=%u",
             static_cast<unsigned long long>(s_utterance_anchor_samples),
             static_cast<unsigned long long>(played), now_ms,
             static_cast<unsigned>(s_event_count));
}

bool TryAnchorOnPcmLocked(uint64_t pcm_start_played, uint32_t peak) {
    // 说话期间先记下首包 PCM，Arm/Load 若晚到可直接用。
    if (s_speaking && !s_pending_pcm_valid) {
        s_pending_pcm_start = pcm_start_played;
        s_pending_pcm_valid = true;
        ESP_LOGI(TAG, "lip pcm first speak_pcm=%llu peak=%u",
                 static_cast<unsigned long long>(pcm_start_played), peak);
    }

    if (!s_await_pcm_anchor || !s_utterance_armed) {
        return false;
    }
    if (peak < kPcmAnchorPeakMin) {
        return false;
    }
    s_utterance_anchor_samples = pcm_start_played;
    s_utterance_anchor_valid = true;
    s_await_pcm_anchor = false;
    s_anchored = false;
    if (s_event_count > 0) {
        ApplyUtteranceAnchorLocked();
    } else {
        ESP_LOGI(TAG, "lip pcm pending viseme pcm=%llu peak=%u",
                 static_cast<unsigned long long>(pcm_start_played), peak);
    }
    return true;
}

// 在 LVGL tick 里消费 audio_output 丢过来的原子通知。
void DrainPcmNotifyLocked() {
    if (!s_pcm_first_ready.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t pcm_start =
        s_pcm_first_start.load(std::memory_order_relaxed);
    const uint32_t peak = s_pcm_first_peak.load(std::memory_order_relaxed);
    TryAnchorOnPcmLocked(pcm_start, peak);
}

void ArmPcmCaptureWindow() {
    s_pcm_first_ready.store(false, std::memory_order_relaxed);
    s_pcm_want_first.store(true, std::memory_order_release);
}

void ClearPcmCaptureWindow() {
    s_pcm_want_first.store(false, std::memory_order_relaxed);
    s_pcm_first_ready.store(false, std::memory_order_relaxed);
}

// Arm/Load 晚于音频时：用已记下的首包，或当前播放位置追赶。
void TryCatchUpAnchorLocked() {
    if (s_anchored || !s_await_pcm_anchor || !s_utterance_armed) {
        return;
    }
    if (s_pending_pcm_valid) {
        s_utterance_anchor_samples = s_pending_pcm_start;
        s_utterance_anchor_valid = true;
        s_await_pcm_anchor = false;
        if (s_event_count > 0) {
            ApplyUtteranceAnchorLocked();
        }
        return;
    }
    if (!s_speaking) {
        return;
    }
    const uint64_t played = CurrentPlayedSamples();
    if (played <= s_speaking_started_played) {
        return;
    }
    // 没有首包记录时，把“现在”当作 t=0，至少嘴能动起来。
    s_utterance_anchor_samples = played;
    s_utterance_anchor_valid = true;
    s_await_pcm_anchor = false;
    if (s_event_count > 0) {
        ApplyUtteranceAnchorLocked();
        ESP_LOGW(TAG, "lip anchor catch-up played=%llu (arm late)",
                 static_cast<unsigned long long>(played));
    }
}

void FillAssetPath(char* buf, size_t n, const char* stem) {
    std::snprintf(buf, n, "A:%s.spng", stem);
}

void SetImgSrc(lv_obj_t* img, char* buf, size_t n, const char* stem) {
    if (img == nullptr || stem == nullptr || stem[0] == '\0') {
        return;
    }
    FillAssetPath(buf, n, stem);
    lv_image_set_src(img, buf);
}

lv_obj_t* MakeLayer(lv_obj_t* parent, int32_t x, int32_t y) {
    lv_obj_t* img = lv_image_create(parent);
    lv_obj_set_pos(img, x, y);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_DEFAULT);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    screen_make_input_passive(img);
    return img;
}

const EmotionPose* PoseForEmotion(const char* emotion) {
    if (emotion == nullptr || emotion[0] == '\0') {
        return nullptr;
    }
    for (const auto& e : kEmotionMap) {
        if (std::strcmp(e.emotion, emotion) == 0) {
            return &e;
        }
    }
    return nullptr;
}

const OverlayGeom* OverlayGeomByName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    for (const auto& o : kOverlays) {
        if (std::strcmp(o.name, name) == 0) {
            return &o;
        }
    }
    return nullptr;
}

const char* VisemeStem(int id) {
    if (id < 0 || id > 14) {
        return kVisemeStems[0];
    }
    return kVisemeStems[id];
}

const char* TransitionStem(int from, int to) {
    for (const auto& t : kTransitions) {
        if (t.from == from && t.to == to) {
            return t.stem;
        }
    }
    return nullptr;
}

void ApplyUpper(const char* stem) {
    char name[32];
    std::snprintf(name, sizeof(name), "upper_%s", stem);
    SetImgSrc(s_ui.upper, s_ui.upper_path, sizeof(s_ui.upper_path), name);
}

void ApplyMouthStem(const char* stem) {
    if (stem == nullptr || s_ui.mouth == nullptr) {
        return;
    }
    if (std::strcmp(s_mouth_applied, stem) == 0) {
        return;
    }
    ESP_LOGD(TAG, "mouth -> %s (speaking=%d events=%u)", stem,
             s_speaking ? 1 : 0, static_cast<unsigned>(s_event_count));
    std::strncpy(s_mouth_applied, stem, sizeof(s_mouth_applied) - 1);
    s_mouth_applied[sizeof(s_mouth_applied) - 1] = '\0';
    SetImgSrc(s_ui.mouth, s_ui.mouth_path, sizeof(s_ui.mouth_path), stem);
}

void ApplyIdleMouth() {
    char name[32];
    std::snprintf(name, sizeof(name), "mouth_%s", s_state);
    ApplyMouthStem(name);
}

void ApplyOverlay() {
    if (s_ui.overlay == nullptr) {
        return;
    }
    const bool hide_for_speech =
        s_speaking && s_hide_overlay_speaking && s_overlay[0] != '\0';
    const OverlayGeom* geom = OverlayGeomByName(s_overlay);
    if (geom == nullptr || hide_for_speech) {
        lv_obj_add_flag(s_ui.overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_pos(s_ui.overlay, geom->x, geom->y);
    SetImgSrc(s_ui.overlay, s_ui.overlay_path, sizeof(s_ui.overlay_path),
              geom->name);
    lv_obj_remove_flag(s_ui.overlay, LV_OBJ_FLAG_HIDDEN);
}

int EnergyLevelFromPeak(uint32_t peak) {
    if (peak < 800) {
        return 0;
    }
    if (peak < 3000) {
        return 1;
    }
    if (peak < 8000) {
        return 2;
    }
    return 3;
}

void ScheduleNextBlink() {
    s_blink_next = static_cast<int>(50 + (esp_random() % 31));
    s_blink_idle = 0;
}

void TickBlink() {
    static const char* kBlinkSeq[] = {
        "blink_35", "blink_80", "closed", "blink_80", "blink_35",
    };
    if (s_blink_step >= 0) {
        if (s_blink_step < 5) {
            ApplyUpper(kBlinkSeq[s_blink_step]);
            s_blink_step++;
        } else {
            ApplyUpper(s_state);
            s_blink_step = -1;
            ScheduleNextBlink();
        }
        return;
    }
    s_blink_idle++;
    if (s_blink_idle >= s_blink_next) {
        s_blink_step = 0;
    }
}

int TickVisemeId(uint64_t played, int sample_rate) {
    std::lock_guard<std::mutex> lock(s_lip_mu);
    if (s_event_count == 0 || !s_anchored) {
        return -1;
    }
    if (sample_rate <= 0) {
        sample_rate = static_cast<int>(kVisemeSampleRate);
    }
    const uint32_t now_ms =
        PlayedToMs(played, s_anchor_samples, sample_rate) + kLipLeadMs;
    int id = 0;
    for (size_t i = 0; i < s_event_count; ++i) {
        const auto& e = s_events[i];
        const uint32_t end = static_cast<uint32_t>(e.time_ms) + e.duration_ms;
        if (now_ms >= e.time_ms && now_ms < end) {
            id = e.id;
            break;
        }
        if (now_ms >= e.time_ms) {
            id = e.id;
        }
    }
    if (id != s_last_logged_viseme ||
        (now_ms > s_last_logged_now_ms + 200)) {
        s_last_logged_viseme = id;
        s_last_logged_now_ms = now_ms;
        ESP_LOGI(TAG, "lip tick now_ms=%u id=%d played=%llu anchor=%llu",
                 now_ms, id, static_cast<unsigned long long>(played),
                 static_cast<unsigned long long>(s_anchor_samples));
    }
    return id;
}

void TickMouth() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    const uint64_t played = codec != nullptr ? codec->GetPlayedSamples() : 0;
    const int sample_rate =
        codec != nullptr && codec->output_sample_rate() > 0
            ? codec->output_sample_rate()
            : static_cast<int>(kVisemeSampleRate);

    {
        std::lock_guard<std::mutex> lock(s_lip_mu);
        DrainPcmNotifyLocked();
        if (!s_anchored) {
            TryCatchUpAnchorLocked();
        }
    }

    if (!s_speaking) {
        s_energy_level = 0;
        s_current_viseme = -1;
        s_trans_left = 0;
        s_trans_stem = nullptr;
        ApplyIdleMouth();
        return;
    }

    // 已 arm 但首包 PCM 未到：保持情绪嘴，不提前张嘴。
    if (!s_anchored || s_event_count == 0) {
        s_energy_level = 0;
        s_current_viseme = -1;
        s_trans_left = 0;
        s_trans_stem = nullptr;
        ApplyIdleMouth();
        return;
    }

    const int viseme_id = TickVisemeId(played, sample_rate);
    if (viseme_id < 0) {
        ApplyIdleMouth();
        return;
    }
    if (viseme_id >= 0) {
        if (s_trans_left > 0 && s_trans_stem != nullptr) {
            ApplyMouthStem(s_trans_stem);
            s_trans_left--;
            return;
        }
        if (s_current_viseme >= 0 && s_current_viseme != viseme_id) {
            const char* trans = TransitionStem(s_current_viseme, viseme_id);
            if (trans != nullptr) {
                s_trans_stem = trans;
                s_trans_left = kTransitionTicks;
                s_current_viseme = viseme_id;
                ApplyMouthStem(trans);
                return;
            }
        }
        s_current_viseme = viseme_id;
        s_energy_level = 0;
        ApplyMouthStem(VisemeStem(viseme_id));
        return;
    }
}

void OnTick(lv_timer_t* /*t*/) {
    if (s_ui.canvas == nullptr) {
        return;
    }
    TickBlink();
    TickMouth();
}

}  // namespace

void AvatarCompositor::Create(lv_obj_t* parent) {
    if (parent == nullptr) {
        return;
    }
    Destroy();

    s_ui.canvas = lv_obj_create(parent);
    screen_strip_obj_chrome(s_ui.canvas);
    lv_obj_set_size(s_ui.canvas, kCanvas, kCanvas);
    lv_obj_center(s_ui.canvas);
    lv_obj_set_style_bg_opa(s_ui.canvas, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.canvas, LV_OBJ_FLAG_CLICKABLE);
    screen_make_input_passive(s_ui.canvas);

    s_ui.base = MakeLayer(s_ui.canvas, 0, 0);
    s_ui.upper = MakeLayer(s_ui.canvas, kUpperX, kUpperY);
    s_ui.mouth = MakeLayer(s_ui.canvas, kMouthX, kMouthY);
    s_ui.overlay = MakeLayer(s_ui.canvas, 0, 0);

    SetImgSrc(s_ui.base, s_ui.base_path, sizeof(s_ui.base_path), "base_360");
    s_mouth_applied[0] = '\0';
    ApplyUpper(s_state);
    ApplyIdleMouth();
    ApplyOverlay();
    ScheduleNextBlink();

    s_ui.tick = lv_timer_create(OnTick, kTickMs, nullptr);
    ESP_LOGI(TAG, "created layered avatar state=%s", s_state);
}

void AvatarCompositor::Destroy() {
    if (s_ui.tick != nullptr) {
        lv_timer_delete(s_ui.tick);
        s_ui.tick = nullptr;
    }
    s_ui = Ui{};
    s_blink_step = -1;
    s_mouth_applied[0] = '\0';
}

bool AvatarCompositor::IsCreated() {
    return s_ui.canvas != nullptr;
}

void AvatarCompositor::SetEmotion(const char* emotion) {
    const EmotionPose* pose = PoseForEmotion(emotion);
    const char* state = pose != nullptr ? pose->state : "neutral";
    const char* overlay = pose != nullptr ? pose->overlay : nullptr;
    s_hide_overlay_speaking = pose != nullptr && pose->hide_overlay_when_speaking;

    std::strncpy(s_state, state, sizeof(s_state) - 1);
    s_state[sizeof(s_state) - 1] = '\0';
    if (overlay != nullptr) {
        std::strncpy(s_overlay, overlay, sizeof(s_overlay) - 1);
        s_overlay[sizeof(s_overlay) - 1] = '\0';
    } else {
        s_overlay[0] = '\0';
    }

    ESP_LOGI(TAG, "emotion %s -> state=%s overlay=%s",
             emotion != nullptr ? emotion : "<null>", s_state,
             s_overlay[0] != '\0' ? s_overlay : "-");

    if (!IsCreated()) {
        return;
    }
    if (s_blink_step < 0) {
        ApplyUpper(s_state);
    }
    if (!s_speaking) {
        ApplyIdleMouth();
    }
    ApplyOverlay();
}

void AvatarCompositor::SetSpeaking(bool speaking) {
    if (s_speaking == speaking) {
        return;
    }
    ESP_LOGI(TAG, "speaking %d -> %d (events=%u anchored=%d)",
             s_speaking ? 1 : 0, speaking ? 1 : 0,
             static_cast<unsigned>(s_event_count), s_anchored ? 1 : 0);
    s_speaking = speaking;
    if (speaking) {
        s_speaking_started_played = CurrentPlayedSamples();
        s_pending_pcm_valid = false;
        s_pending_pcm_start = 0;
        ArmPcmCaptureWindow();
    } else {
        ResetLipSync();
        if (IsCreated()) {
            ApplyIdleMouth();
        }
    }
    if (IsCreated()) {
        ApplyOverlay();
    }
}

void AvatarCompositor::ArmUtterance(int index) {
    std::lock_guard<std::mutex> lock(s_lip_mu);
    const bool same_index_reload =
        s_event_count > 0 && index == s_armed_index && s_anchored;
    if (same_index_reload) {
        return;
    }
    const bool index_changed = (index != s_armed_index);
    const bool keep_events = s_event_count > 0 && index == s_armed_index;
    // 换句才清 pending；首句保留 speaking 期间已记下的首包。
    if (index_changed && s_utterance_armed) {
        s_pending_pcm_valid = false;
        s_pending_pcm_start = 0;
        ArmPcmCaptureWindow();
    }
    s_armed_index = index;
    s_utterance_armed = true;
    s_await_pcm_anchor = true;
    s_utterance_anchor_valid = false;
    s_anchored = false;
    s_need_anchor = true;
    if (!keep_events) {
        s_event_count = 0;
        s_current_viseme = -1;
        s_last_logged_viseme = -1;
        s_last_logged_now_ms = 0;
        s_trans_left = 0;
        s_trans_stem = nullptr;
    }
    ESP_LOGI(TAG, "arm utterance index=%d keep_events=%d pending_pcm=%d",
             index, keep_events ? 1 : 0, s_pending_pcm_valid ? 1 : 0);
    TryCatchUpAnchorLocked();
}

void AvatarCompositor::NotifyPcmOutput(uint64_t pcm_start_played, uint32_t peak) {
    // 仅原子写入；禁止在 audio_output 任务里拿锁/打日志（栈只有 2~4KB）。
    if (!s_pcm_want_first.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    s_pcm_first_start.store(pcm_start_played, std::memory_order_relaxed);
    s_pcm_first_peak.store(peak, std::memory_order_relaxed);
    s_pcm_first_ready.store(true, std::memory_order_release);
}

void AvatarCompositor::LoadVisemeTimeline(int index, const VisemeEvent* events,
                                          size_t count) {
    std::lock_guard<std::mutex> lock(s_lip_mu);
    if (index < s_armed_index && s_armed_index != 0) {
        ESP_LOGW(TAG, "drop stale viseme index=%d armed=%d", index, s_armed_index);
        return;
    }
    if (events == nullptr || count == 0) {
        s_event_count = 0;
        return;
    }
    if (index == s_armed_index && count <= s_event_count && s_event_count > 0 &&
        s_anchored) {
        ESP_LOGI(TAG, "skip duplicate viseme index=%d events=%u (have %u)",
                 index, static_cast<unsigned>(count),
                 static_cast<unsigned>(s_event_count));
        return;
    }
    const size_t incoming = count;
    if (count > kMaxVisemes) {
        ESP_LOGW(TAG, "viseme trunc %u -> %u", static_cast<unsigned>(count),
                 static_cast<unsigned>(kMaxVisemes));
        count = kMaxVisemes;
    }
    std::memcpy(s_events, events, count * sizeof(VisemeEvent));
    s_event_count = count;
    s_armed_index = index;
    s_utterance_armed = true;
    s_current_viseme = -1;
    s_last_logged_viseme = -1;
    s_last_logged_now_ms = 0;
    s_trans_left = 0;
    s_trans_stem = nullptr;
    if (!s_anchored) {
        s_await_pcm_anchor = true;
        s_need_anchor = true;
    }
    if (s_utterance_anchor_valid) {
        ApplyUtteranceAnchorLocked();
    } else {
        TryCatchUpAnchorLocked();
    }
    ESP_LOGI(TAG, "viseme timeline index=%d events=%u/%u anchored=%d await=%d",
             index, static_cast<unsigned>(count), static_cast<unsigned>(incoming),
             s_anchored ? 1 : 0, s_await_pcm_anchor ? 1 : 0);
}

void AvatarCompositor::ResetLipSync() {
    std::lock_guard<std::mutex> lock(s_lip_mu);
    s_armed_index = 0;
    s_need_anchor = false;
    s_anchored = false;
    s_anchor_samples = 0;
    s_utterance_anchor_valid = false;
    s_utterance_anchor_samples = 0;
    s_await_pcm_anchor = false;
    s_utterance_armed = false;
    s_pending_pcm_valid = false;
    s_pending_pcm_start = 0;
    ClearPcmCaptureWindow();
    s_event_count = 0;
    s_current_viseme = -1;
    s_last_logged_viseme = -1;
    s_last_logged_now_ms = 0;
    s_trans_left = 0;
    s_trans_stem = nullptr;
    s_energy_level = 0;
}
