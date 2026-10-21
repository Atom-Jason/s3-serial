#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

esp_err_t network_service_init(void);
bool network_service_password_matches(const char *candidate);
cJSON *network_service_status(void);
const char *network_service_configure(const cJSON *payload);
esp_err_t network_service_change_password(const char *password);

