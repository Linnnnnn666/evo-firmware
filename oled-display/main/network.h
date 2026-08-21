#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool network_init(void);
bool network_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
