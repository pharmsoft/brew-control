#include "esp_log.h"

#include "wifi_ap.h"
#include "web_server.h"
#include "brew_control.h"
#include "profile_store.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "=== BrewControl: прототип управления пивоварней ===");

    brew_control_init();   // движок профилей + симулятор температуры
    wifi_ap_init();        // точка доступа BrewControl / 192.168.4.1 (+ init NVS)
    profile_store_init();  // библиотека именованных профилей в NVS
    web_server_start();    // веб-интерфейс + REST API

    ESP_LOGI(TAG, "Готово. Подключитесь к Wi-Fi 'BrewControl' и откройте http://192.168.4.1");
}
