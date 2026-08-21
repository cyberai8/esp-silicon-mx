// EXTRACT from voiceshow/main/application.cc
// 设备状态切换时：SetStatus / SetEmotion / ClearChatMessages（待机/连接/聆听/说话）
// 原路径: voiceshow/main/application.cc

switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            DisarmTtsWatchdog();
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            audio_service_.EnableVoiceProcessing(false);
#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
            EnsureVocatAudioForIdle();
#endif
            if (old_state == kDeviceStateActivating) {
                // 激活完成默认进入语音 AI 聊天页；底部上滑仍可打开圆环 UI 菜单。
                if (CanEnterHome()) {
                    if (auto* emote = vocat::GetVocatDisplay(display)) {
                        emote->EnterHomeChatFromRing();
                    }
                    SetChatRouteMode("xiaozhi");
                    SendChatRouteUpdate();
                } else {
                    ESP_LOGW(TAG, "Skip EnterHomeChatFromRing: home gate closed");
                }
                display->SetEmotion("neutral");
                audio_service_.EnableWakeWordDetection(ShouldEnableWakeWordOnIdle(display));
            } else {
                display->SetEmotion("neutral");
                audio_service_.EnableWakeWordDetection(ShouldEnableWakeWordOnIdle(display));
            }
            if (start_listening_pending_) {
                // Recover from race: listening request may arrive while state is transient.
                xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
            }

            // Conversation ended — do not prefetch pet metrics (fetch on intimacy page enter).
            if (old_state == kDeviceStateSpeaking || old_state == kDeviceStateListening) {
                Board::GetInstance().OnEnterIdleFromConversation();
            }

            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            // 不要用空 system 消息：会清空默认/提示条，且历史上 SetStatus 未处理「连接中」时整屏会看起来没反应
            break;
        case kDeviceStateListening: {
            DisarmTtsWatchdog();
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Stop wake word before starting voice processor to avoid two AFE instances
            // competing for the same mic feed and racing on Deinitialize().
#ifndef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            audio_service_.EnableWakeWordDetection(false);
#endif

#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
            // Apply capture profile / AEC before arming uplink (Reconfigure must not run after Start).
            EnsureVocatAudioForListening();
#endif

            // Make sure the audio processor is running
            const bool dictation_mic_rearm =
                dictation_hold_active_ && IsTelnetDictationPageOnScreen(display);
            const bool entering_listen =
                old_state != kDeviceStateListening;
            const bool realtime_continue_after_tts =
                listening_mode_ == kListeningModeRealtime &&
                old_state == kDeviceStateSpeaking &&
                audio_service_.IsAudioProcessorRunning();
            if ((!realtime_continue_after_tts && entering_listen) || play_popup_on_listening_ ||
                !audio_service_.IsAudioProcessorRunning() || dictation_mic_rearm) {
                if (old_state == kDeviceStateSpeaking) {
                    if (!audio_service_.WaitForPlaybackQueueEmpty(1000)) {
                        ESP_LOGW(TAG, "Playback queue not empty before listening; continue to keep main loop responsive");
                    }
                }

                SendChatRouteUpdate();
                // Send the start listening command
                const bool route_ext = chat_route_mode_ != "xiaozhi";
                protocol_->SendStartListening(GetListeningModeForCurrentRoute(),
                                             IsDictationListenOnlySession(),
                                             route_ext ? AsrStrategyForCurrentRoute().c_str() : nullptr);
                if (dictation_hold_active_ && IsTelnetDictationPageOnScreen(display)) {
                    dictation_listen_start_us_ = esp_timer_get_time();
                }
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP());
            }
            break;
        }
        case kDeviceStateSpeaking:
            speaking_entered_time_us_ = esp_timer_get_time();
#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
            realtime_barge_candidate_us_ = 0;
#endif
            ArmTtsWatchdog(TtsWatchdogTimeoutUs());
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            if (preserve_tts_audio_queue_) {
                preserve_tts_audio_queue_ = false;
            } else {
                audio_service_.ResetDecoder();
            }
            break;
        
