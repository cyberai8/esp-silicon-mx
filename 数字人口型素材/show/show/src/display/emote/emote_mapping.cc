#include "emote_mapping.h"

#include <cstring>
#include <strings.h>

namespace vocat {
namespace emote {
namespace {

struct MapEntry {
    const char* emotion;
    const char* clip;
};

// Device pack (13 clips): angry confused crying delicious happy idle
// listening sad shocked shy sleepy speaking tired
//
// Cloud EmotionEmoji (22) + aliases + local device states. Prefer a distinct
// pack face over collapsing everything into happy/sleepy.

constexpr MapEntry kMap[] = {
    // —— pack identity (cloud or MCP may send the clip name itself) ——
    {"angry", "angry"},
    {"confused", "confused"},
    {"crying", "crying"},
    {"delicious", "delicious"},
    {"happy", "happy"},
    {"idle", "sleepy"},
    {"listening", "listening"},
    {"sad", "sad"},
    {"shocked", "shocked"},
    {"shy", "shy"},
    {"sleepy", "sleepy"},
    {"speaking", "speaking"},
    {"tired", "tired"},

    // —— cloud EmotionEmoji (xiaozhi / Vocat llm.emotion) ——
    {"neutral", "sleepy"},
    {"laughing", "happy"},
    {"funny", "happy"},
    {"loving", "shy"},
    {"embarrassed", "shy"},
    {"surprised", "shocked"},
    {"thinking", "confused"},
    {"winking", "happy"},
    {"cool", "idle"},
    {"relaxed", "sleepy"},
    {"kissy", "shy"},
    {"confident", "idle"},
    {"silly", "confused"},

    // —— cloud aliases / explicit-expression commands ——
    {"wink", "happy"},
    {"eat", "delicious"},
    {"tried", "tired"},
    {"question", "confused"},
    {"book", "listening"},
    {"look_left", "listening"},
    {"look_right", "listening"},
    {"look_around", "listening"},
    {"insert", "idle"},
    {"paishou", "happy"},
    {"afraid", "shocked"},

    // —— device / alert fillers ——
    {"microchip_ai", "sleepy"},
    {"dizzy", "confused"},
    {"nauseated", "confused"},
    {"triangle_exclamation", "sad"},
    {"circle_xmark", "sad"},
    {"cloud_slash", "sad"},
};

bool Eq(const char* a, const char* b)
{
    return a != nullptr && b != nullptr && strcasecmp(a, b) == 0;
}

}  // namespace

const char* ResolveClipName(const char* server_emotion)
{
    const char* key = (server_emotion != nullptr && server_emotion[0] != '\0') ? server_emotion
                                                                               : "neutral";
    for (const auto& e : kMap) {
        if (Eq(key, e.emotion)) {
            return e.clip;
        }
    }
    return "sleepy";
}

bool ClipShouldRepeat(const char* /*clip_or_emotion*/)
{
    // Packer entries use 双点循环; one-shot play freezes on the last frame.
    return true;
}

bool IsStandbyEmotion(const char* emotion)
{
    if (emotion == nullptr || emotion[0] == '\0') {
        return true;
    }
    // Standby/state filler only. "relaxed" is a valid cloud content emotion and must
    // not be treated as "keep previous face" during conversation.
    return Eq(emotion, "neutral") || Eq(emotion, "idle") || Eq(emotion, "microchip_ai");
}

}  // namespace emote
}  // namespace vocat
