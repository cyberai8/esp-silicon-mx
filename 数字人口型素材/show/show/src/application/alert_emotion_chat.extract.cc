// EXTRACT from voiceshow/main/application.cc
// Alert：状态条 + 表情 + system 字幕

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    const bool error_emotion =
        emotion != nullptr &&
        (strcmp(emotion, "circle_xmark") == 0 ||
         strcmp(emotion, "triangle_exclamation") == 0 ||
         strcmp(emotion, "cloud_slash") == 0);
    if (error_emotion) {
        // MAIN_EVENT_ERROR transitions to idle in the same event-loop pass. Idle then sends
        // neutral, so keep the error face as an overlay long enough to be perceptible.
        if (auto* vocat_ui = vocat::GetVocatDisplay(display)) {
            vocat_ui->PlayTemporaryHomeEmotion(emotion, 2000);
        } else {
            display->SetEmotion(emotion);
        }
    } else {
        display->SetEmotion(emotion);
    }
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound, 60);
    }
}

