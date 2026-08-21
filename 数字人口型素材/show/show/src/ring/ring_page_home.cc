#include "ring_pages.h"

#include <esp_log.h>

#include "application.h"
#include "dictation_forward.h"
#include "display/vocat/vocat_display.h"

#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
#include "boards/esp-vocat-se/esp_vocat_settings.h"
#endif

void ShowHomePage(vocat::IVocatDisplay* display)
{
    ESP_LOGI("RingPageHome", "open home page (xiaozhi chat)");
    auto& app = Application::GetInstance();

    app.EndDictationHold(false);
    DictationForwardSetEnabled(false);

    if (display != nullptr) {
        display->EnterHomeChatFromRing(false);
    }

    app.SetChatRouteMode("xiaozhi");
    app.SendChatRouteUpdate();
#if CONFIG_BOARD_TYPE_ESP_VOCAT_SE || CONFIG_BOARD_TYPE_ESP_VOCAT_SE_V1_2
    if (EspVocatRealtimeChatEnabled()) {
        app.ToggleChatState();
        return;
    }
#endif
    app.SwitchToIdle();
    app.ArmHomeChatIdle();
}
