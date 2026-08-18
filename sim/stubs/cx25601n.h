#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool cx25601n_is_ready(void);
esp_err_t cx25601n_set_ichg_ma(uint32_t ma);

#ifdef __cplusplus
}
#endif
