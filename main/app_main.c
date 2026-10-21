#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "network_service.h"
#include "serial_service.h"
#include "web_service.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE("app", "NVS needs maintenance (%s). Back up settings before erasing NVS.", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(network_service_init());
    ESP_ERROR_CHECK(serial_service_init(web_service_serial_event));
    ESP_ERROR_CHECK(web_service_init());
}
