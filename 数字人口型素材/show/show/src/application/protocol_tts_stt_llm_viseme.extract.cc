// EXTRACT from voiceshow/main/application.cc
// AI 对话过程：TTS 字幕 / STT 字幕 / LLM 表情 / Viseme 口型轴
// 原路径: voiceshow/main/application.cc

if (strcmp(ts, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                ESP_LOGI(TAG, "TTS state: start");
                if (IsDictationListenOnlySession()) {
                    // Keep device in listen-only path: no speaking state, no TTS decode (see OnIncomingAudio).
                    Schedule([this]() {
                        aborted_ = false;
                        if (GetDeviceState() == kDeviceStateSpeaking) {
                            SetDeviceState(kDeviceStateListening);
                        }
                    });
                } else {
                    Schedule([this]() {
                        if (IsChatRouteNone()) {
                            return;
                        }
                        aborted_ = false;
                        lip_sync_controller_.Reset();
                        tts_audio_packets_ = 0;
                        last_tts_audio_packet_time_us_ = 0;
                        preserve_tts_audio_queue_ = false;
#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
                        if (listening_mode_ == kListeningModeRealtime &&
                            GetDeviceState() == kDeviceStateListening) {
                            return;
                        }
#endif
                        SetDeviceState(kDeviceStateSpeaking);
                    });
                }
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI(TAG, "TTS state: stop");
                Schedule([this, display]() {
                    if (bridge_awaiting_played_) {
                        EmitNotifyPlayed(bridge_pending_notify_id_, bridge_pending_task_id_,
                                         SystemInfo::GetMacAddress(), 0);
                        ClearBridgePlayedPending();
                    }
                    lip_sync_controller_.Reset();
                    if (IsDictationListenOnlySession()) {
                        if (GetDeviceState() == kDeviceStateSpeaking) {
                            SetDeviceState(kDeviceStateListening);
                        }
                        return;
                    }
                    if (GetDeviceState() != kDeviceStateSpeaking) {
#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
                        if (listening_mode_ == kListeningModeRealtime && tts_audio_packets_ == 0) {
                            ESP_LOGW(TAG, "Empty realtime TTS turn ignored while still listening");
                        }
#endif
                        return;
                    }
                    if (listening_mode_ == kListeningModeManualStop) {
                        StartTtsPlaybackDrainTo(kDeviceStateIdle);
                    } else {
                        StartTtsPlaybackDrainTo(kDeviceStateListening);
                    }
                    // Assistant subtitle should not stay pinned on screen after TTS ends.
                    display->SetChatMessage("assistant", "");
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                const cJSON* index_item = cJSON_GetObjectItem(root, "index");
                const int sentence_index = cJSON_IsNumber(index_item) ? index_item->valueint : -1;
                if (IsDictationListenOnlySession()) {
                    // No assistant subtitle in dictation-only UI.
                } else {
                    auto text = cJSON_GetObjectItem(root, "text");
                    if (cJSON_IsString(text)) {
                        ESP_LOGI(TAG, "<< %s", text->valuestring);
                        Schedule([this, display, message = std::string(text->valuestring),
                                  sentence_index]() {
                            if (IsChatRouteNone()) {
                                return;
                            }
                            // 「演唱中」只是状态字，不是一句 TTS。Arm 会清掉口型轴并干等 DAC，
                            // 歌声已经在播时嘴就会一直闭着。歌词 viseme 自己 LoadTimeline。
                            if (!IsServerStatusStt(message)) {
                                lip_sync_controller_.ArmUtterance(sentence_index);
                            }
#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
                            if (listening_mode_ == kListeningModeRealtime &&
                                !aborted_ &&
                                GetDeviceState() != kDeviceStateSpeaking) {
                                SetDeviceState(kDeviceStateSpeaking);
                            }
#endif
                            display->SetChatMessage("assistant", message.c_str());
                        });
                    } else if (sentence_index >= 0) {
                        Schedule([this, sentence_index]() {
                            lip_sync_controller_.ArmUtterance(sentence_index);
                        });
                    }
                }
            }
        } else if (strcmp(ts, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                const std::string stt_message(text->valuestring);
                bool is_final = false;
                const cJSON* fin = cJSON_GetObjectItem(root, "final");
                if (cJSON_IsBool(fin)) {
                    is_final = cJSON_IsTrue(fin);
                }
                const bool is_tool_status = IsToolStatusStt(stt_message);
                const bool is_server_status = IsServerStatusStt(stt_message);
                const cJSON* role = cJSON_GetObjectItem(root, "role");
                const bool is_assistant_caption =
                    cJSON_IsString(role) && role->valuestring != nullptr &&
                    strcmp(role->valuestring, "assistant") == 0;
                Schedule([this, display, message = stt_message, is_tool_status, is_server_status,
                          is_assistant_caption, is_final]() {
                    if (IsChatRouteNone()) {
                        return;
                    }
#if CONFIG_USE_LVGL_VOCAT_UI
                    if (auto* lvgl = dynamic_cast<VocatLvglDisplay*>(display)) {
                        if (lvgl->IsMeetingPageVisible() && meeting_hold_active_ && !is_tool_status &&
                            !is_server_status && !is_assistant_caption && !message.empty()) {
                            DisplayLockGuard lock(lvgl);
                            lvgl->AppendMeetingCaption(message.c_str(), is_final);
                        }
                    }
#endif
                    if (!IsDictationListenOnlySession()) {
                        if (is_tool_status) {
                            display->ShowNotification(ToolStatusToastText(message), 1500);
                        } else if (is_server_status || is_assistant_caption) {
                            // AI sing status/lyrics — show as assistant, never as user barge-in.
                            display->SetChatMessage("assistant", message.c_str());
                        } else if (!meeting_hold_active_) {
                            display->SetChatMessage("user", message.c_str());
                        }
                    }
#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
                    // Only real final user ASR may abort TTS. Server status STT
                    // ("演唱中") and lyric captions must not ResetDecoder.
                    if (!message.empty() &&
                        !is_server_status &&
                        !is_assistant_caption &&
                        is_final &&
                        listening_mode_ == kListeningModeRealtime &&
                        GetDeviceState() == kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "Realtime STT during TTS: %s", message.c_str());
                        aborted_ = true;
                        DisarmTtsWatchdog();
                        audio_service_.ResetDecoder();
                        SetDeviceState(kDeviceStateListening);
                    }
#endif
                });
                DictationForwardEnqueueStt(stt_message, is_final);
            }
        } else if (strcmp(ts, "llm") == 0) {
            if (!IsDictationListenOnlySession()) {
                auto emotion = cJSON_GetObjectItem(root, "emotion");
                if (cJSON_IsString(emotion)) {
                    Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                        ESP_LOGI(TAG, "LLM emotion -> %s", emotion_str.c_str());
                        display->SetEmotion(emotion_str.c_str());
                    });
                }
            }
        } else if (strcmp(ts, "viseme") == 0) {
            auto index = cJSON_GetObjectItem(root, "index");
            auto visemes = cJSON_GetObjectItem(root, "visemes");
            const int idx = cJSON_IsNumber(index) ? index->valueint : -1;
            if (!cJSON_IsArray(visemes)) {
                ESP_LOGW(TAG, "viseme message missing visemes array");
                return;
            }
            std::vector<VisemeEvent> events;
            const int count = cJSON_GetArraySize(visemes);
            events.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                const cJSON* item = cJSON_GetArrayItem(visemes, i);
                if (!cJSON_IsObject(item)) {
                    continue;
                }
                VisemeEvent ev;
                const cJSON* time_ms = cJSON_GetObjectItem(item, "time_ms");
                const cJSON* duration_ms = cJSON_GetObjectItem(item, "duration_ms");
                const cJSON* id = cJSON_GetObjectItem(item, "id");
                const cJSON* blend_ms = cJSON_GetObjectItem(item, "blend_ms");
                ev.time_ms = cJSON_IsNumber(time_ms) ? time_ms->valueint : 0;
                ev.dur_ms = cJSON_IsNumber(duration_ms) ? duration_ms->valueint : 120;
                ev.id = cJSON_IsNumber(id) ? id->valueint : kVisemeSIL;
                ev.blend_ms = cJSON_IsNumber(blend_ms) ? blend_ms->valueint : 0;
                events.push_back(ev);
            }
            Schedule([this, idx, events = std::move(events)]() {
                lip_sync_controller_.LoadTimeline(idx, std::move(events));
            });
        
