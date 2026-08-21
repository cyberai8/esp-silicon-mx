#pragma once

namespace vocat {
namespace emote {

/**
 * Map cloud / Vocat `llm.emotion` strings to this firmware's 13 pack clips.
 * Unknown names fall back to `sleepy` (standby face).
 */
const char* ResolveClipName(const char* server_emotion);

/** Packer 双点循环 clips should always loop. */
bool ClipShouldRepeat(const char* server_emotion);

/** State-machine / idle fillers — not a real content face from the LLM. */
bool IsStandbyEmotion(const char* emotion);

}  // namespace emote
}  // namespace vocat
