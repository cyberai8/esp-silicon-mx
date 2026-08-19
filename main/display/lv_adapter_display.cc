#include "lv_adapter_display.h"

#include <cstring>
#include <memory>

#include <esp_lcd_panel_io.h>
#include <esp_log.h>

#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "esp_lv_fs.h"
#include "esp_mmap_assets.h"
#include "touch_feed.h"   

#include "mmap_generate_resources.h"

#include "screen/boot_screen/boot_screen.h"
#include "screen/chat_screen/chat_screen.h"
#include "screen/digital_people_screen/digital_people_screen.h"
#include "screen/home_screen/home_screen.h"

#include "application.h"

static const char* TAG = "LVAdapterDisplay";

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height, const esp_lv_adapter_panel_interface_t panel_if,
                                   const SpiTeConfig* spi_te) {
    // VoCat：先保持关屏，等 BootScreen 画完再开，避免未初始化 framebuffer 闪边角。
#if !CONFIG_BOARD_TYPE_ESP_VOCAT
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
#endif

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
#if CONFIG_BOARD_TYPE_ESP_VOCAT || CONFIG_BOARD_TYPE_WAVESHARE_S3_TOUCH_LCD_1_85B || \
    (defined(DISPLAY_WIDTH) && defined(DISPLAY_HEIGHT) && DISPLAY_WIDTH == 360 && DISPLAY_HEIGHT == 360)
    // LVGL 任务栈若在 PSRAM，任务内读 NVS/Flash 会触发
    // esp_task_stack_is_sane_cache_disabled assert（主屏状态栏/主题都会读 NVS）。
    // 360 小屏 LVGL 栈不大，改用内部 RAM。
    adapter_cfg.stack_in_psram = false;
#else
    adapter_cfg.stack_in_psram = true;
#endif
    // 默认优先级 6 在 QSPI+WiFi 时刷屏过猛易堵 SPI；4 仍高于多数业务任务。
    adapter_cfg.task_priority = 4;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg;
    if (panel_if == ESP_LV_ADAPTER_PANEL_IF_RGB) {
        // ESP32-S31 Korvo-1：官方 Brookesia 在 num_fbs=2 时用 DOUBLE_DIRECT。
        // DOUBLE_FULL 会走全屏软件重绘，首页滑动时 LVGL 任务堵死触发 task_wdt。
        // RGB DEFAULT 宏是 TRIPLE_PARTIAL（要 3 缓冲），与板级 2 FB 不匹配。
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
            panel, panel_io, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
            ESP_LV_ADAPTER_ROTATE_0);
        disp_cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
        disp_cfg.profile.use_psram = true;
        disp_cfg.profile.require_double_buffer = true;
        // S31 无 PPA；关掉避免误开
        disp_cfg.profile.enable_ppa_accel = false;
    } else if (panel_if == ESP_LV_ADAPTER_PANEL_IF_OTHER) {
        // SPI/QSPI（ESP-VoCat / Waveshare 360 QSPI）：OTHER 接口只允许 NONE / TE_SYNC。
        // TE_SYNC / 全高 FULL 刷在本板 QSPI 上会 spi transmit (queue) color failed，
        // 随后 LVGL 卡在 wait_for_flushing 触发 task_wdt。必须用 PARTIAL 条带。
        const bool use_te = (spi_te != nullptr && spi_te->te_gpio >= 0);
        if (use_te) {
            disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_TE_DEFAULT_CONFIG(
                panel, panel_io, static_cast<uint16_t>(width),
                static_cast<uint16_t>(height), ESP_LV_ADAPTER_ROTATE_0, spi_te->te_gpio,
                spi_te->bus_freq_hz, spi_te->data_lines, spi_te->bits_per_pixel);
        } else {
            disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
                panel, panel_io, static_cast<uint16_t>(width),
                static_cast<uint16_t>(height), ESP_LV_ADAPTER_ROTATE_0);
            if (height <= 400) {
                // QSPI 360：必须用短条带 PARTIAL。全高/过大条带会 spi queue color failed，
                // 随后 LVGL 卡在 wait_for_flushing。
                // 画缓冲放内部 RAM：WiFi 抢 PSRAM 时 SPI DMA 从 PSRAM 取数易挂死无完成回调。
                disp_cfg.profile.buffer_height = 40;
                disp_cfg.profile.require_double_buffer = true;
                disp_cfg.profile.use_psram = false;
            }
        }
        disp_cfg.profile.enable_ppa_accel = false;
        ESP_LOGI(TAG, "SPI/QSPI display %dx%d te=%s buf_h=%u dbl=%d psram=%d", width, height,
                 use_te ? "on" : "off",
                 static_cast<unsigned>(disp_cfg.profile.buffer_height),
                 disp_cfg.profile.require_double_buffer ? 1 : 0,
                 disp_cfg.profile.use_psram ? 1 : 0);
    } else {
        // 性能调优要点（720x720 MIPI-DSI RGB565 屏，Claw4）：
        //   - enable_ppa_accel: 开启 PPA
        //   - tear_avoid_mode = TRIPLE_FULL：用 panel 三帧缓冲，避开 partial fallback
        disp_cfg = {
            .panel = panel,
            .panel_io = panel_io,
            .profile =
                {
                    .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
                    .hor_res = static_cast<uint16_t>(width),
                    .ver_res = static_cast<uint16_t>(height),
                    .buffer_height = 200,
                    .use_psram = true,
                    .enable_ppa_accel = true,
                    .require_double_buffer = true,
                },
            .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL,
        };
    }

    lv_display_t* disp = esp_lv_adapter_register_display(&disp_cfg);
    if (touch_handle != nullptr) {
        esp_lv_adapter_touch_config_t touch_cfg =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        lv_indev_t* touch_indev = esp_lv_adapter_register_touch(&touch_cfg);
        // 16ms：跟手与负载折中；CST816S 仍由 INT 门控。
        touch_feed_init(touch_handle, 16);
        touch_feed_attach_indev(touch_indev);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    // 图标走 A:*.spng；缓存驻留解码结果，避免滑动重绘时反复解码。
    // 360 圆屏图标页多，给足 1MB；大屏 Claw4：2MB。
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    lv_image_cache_resize(512 * 1024, true);
#elif defined(CONFIG_BOARD_TYPE_ESP_VOCAT) || defined(CONFIG_BOARD_TYPE_WAVESHARE_S3_TOUCH_LCD_1_85B)
    lv_image_cache_resize(1024 * 1024, true);
#else
    lv_image_cache_resize(2 * 1024 * 1024, true);
#endif

    mmap_assets_handle_t assets;
    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "resources",
        .max_files = MMAP_RESOURCES_FILES,
        .checksum = MMAP_RESOURCES_CHECKSUM,
        .flags = {.mmap_enable = true},
    };
    ESP_ERROR_CHECK(mmap_assets_new(&mmap_cfg, &assets));

    esp_lv_fs_handle_t fs_handle;
    const fs_cfg_t fs_cfg = {
        .fs_letter = 'A',
        .fs_nums = MMAP_RESOURCES_FILES,
        .fs_assets = assets,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_fs_mount(&fs_cfg, &fs_handle));

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetupUI();
        esp_lv_adapter_unlock();
    }

    // Application::GetInstance().ForceReturnToIdle();
}

void LVAdapterDisplay::SetupUI() {
    lv_obj_t* boot_scr = BootScreen::Create();
    lv_screen_load(boot_scr);
}

void LVAdapterDisplay::WaitForBootAnimation() {
    BootScreen::WaitUntilAnimationFinished();
}

void LVAdapterDisplay::ShowHomeScreen() {
    if (home_shown_) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home_scr = HomeScreen::Create();
    lv_screen_load(home_scr);
    if (old_scr != nullptr && old_scr != home_scr) {
        lv_obj_delete(old_scr);
    }
    home_shown_ = true;
    esp_lv_adapter_unlock();
}

LVAdapterDisplay::~LVAdapterDisplay() = default;

void LVAdapterDisplay::SetEmotion(const char* const emotion) {
    const char* name = (emotion != nullptr && emotion[0] != '\0') ? emotion
                                                                    : "neutral";
    ESP_LOGI(TAG, "SetEmotion: %s", name);

    // 数字人：show11 完整 emotion 映射；聊天：仍用服务器原始名读 SD chat/*.eaf
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    DigitalPeopleScreen::SetEmotion(name);
    ChatScreen::SetEmotion(name);
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetChatMessage(const char* const role, const char* const content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') {
        return;
    }

    // role 归一化：
    //   user             -> 用户发言
    //   assistant/system -> 设备 / AI 回应
    const bool is_user = (std::strcmp(role, "user") == 0);
    const bool is_bot  = (std::strcmp(role, "assistant") == 0 ||
                          std::strcmp(role, "system") == 0);
    if (!is_user && !is_bot) {
        return;
    }

    // 路由策略：
    //   1) 聊天屏在前台 -> 历史滚动气泡（双侧）。
    //   2) 数字人屏在前台 -> user 走底部气泡，bot 走 gif 左上方气泡。
    //   3) 其它屏 -> 直接丢弃，避免在后台无界堆积。
    const bool chat_active = ChatScreen::IsActive();
    const bool dp_active   = DigitalPeopleScreen::IsActive();
    if (!chat_active && !dp_active) {
        return;
    }

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (chat_active) {
        ChatScreen::AddMessage(content,
                               is_user ? ChatMsgDir::Right : ChatMsgDir::Left);
    } else {
        if (is_user) {
            DigitalPeopleScreen::ShowUserMessage(content);
        } else {
            DigitalPeopleScreen::ShowSystemMessage(content);
        }
    }
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetStatus(const char* const status) {}

void LVAdapterDisplay::ShowNotification(const char* notification, int duration_ms) {}

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {}

void LVAdapterDisplay::SetPreviewImage(const void* image) {}

void LVAdapterDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool LVAdapterDisplay::Lock(const int timeout_ms) { return true; }

void LVAdapterDisplay::Unlock() {}
