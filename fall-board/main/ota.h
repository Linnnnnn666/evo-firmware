#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
void ota_init(void);
esp_err_t ota_check_now(void);
#ifdef __cplusplus
}
#endif
