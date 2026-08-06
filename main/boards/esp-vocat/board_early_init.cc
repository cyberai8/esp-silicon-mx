#include "board_early_init.h"

#include "config.h"
#include "pwr_key_handler.h"

#include <driver/gpio.h>

#include "esp_log.h"

namespace {

constexpr const char* TAG = "VocatEarly";

}  // namespace

void VocatEarlyPowerInit() {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1ULL << PG2_HOLD_GPIO;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(PG2_HOLD_GPIO, PG2_HOLD_ACTIVE_LEVEL);
    ESP_LOGI(TAG, "PG2 latch GPIO%d=%d (before app init)",
             static_cast<int>(PG2_HOLD_GPIO), PG2_HOLD_ACTIVE_LEVEL);

    PwrKey_InitGpio(PG1_POWER_KEY_GPIO, PG1_POWER_KEY_ACTIVE_LEVEL != 0);
    ESP_LOGI(TAG, "PG1 power key poll started (before app init)");
}
