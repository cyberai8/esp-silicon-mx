#include "cx25601n.h"

esp_err_t cx25601n_init(i2c_master_bus_handle_t /*bus*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

bool cx25601n_is_ready(void) {
    return false;
}

esp_err_t cx25601n_enable_charge(bool /*enable*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_is_charge_enabled(bool* /*enabled*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_set_ichg_ma(uint32_t /*ma*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_get_ichg_ma(uint32_t* /*ma*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_set_iindpm_ma(uint32_t /*ma*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_get_iindpm_ma(uint32_t* /*ma*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_set_vreg_mv(uint32_t /*mv*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_get_vreg_mv(uint32_t* /*mv*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_get_chrg_stat(uint8_t* /*stat*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_get_vbus_stat(uint8_t* /*stat*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t cx25601n_read_reg(uint8_t /*reg*/, uint8_t* /*val*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

const char* cx25601n_chrg_stat_str(uint8_t /*stat*/) {
    return "-";
}

const char* cx25601n_vbus_stat_str(uint8_t /*stat*/) {
    return "-";
}
