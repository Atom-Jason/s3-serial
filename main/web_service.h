#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

esp_err_t web_service_init(void);
void web_service_serial_event(uint32_t owner, const char *type,
                              const char *request_id, const cJSON *payload);
