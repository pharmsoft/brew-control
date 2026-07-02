#include "wifi_ap.h"

#include <string.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_ap";

#define AP_SSID     "BrewControl"
#define AP_PASS     "brew1234"   // WPA2, минимум 8 символов
#define AP_CHANNEL  1
#define AP_MAX_CONN 4

// ВАЖНО: не логировать на каждом событии. При нестабильной ассоциации события
// сыплются тысячами в секунду, а синхронный вывод в UART (115200 бод) блокирует
// системные задачи и рушит работу HTTP-сервера. Логируем с ограничением частоты.
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    static int64_t last_log_us = 0;

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        int64_t now = esp_timer_get_time();
        if (now - last_log_us > 1000000) {   // не чаще 1 раза в секунду
            last_log_us = now;
            ESP_LOGI(TAG, "Клиент подключился, AID=%d", e->aid);
        }
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        int64_t now = esp_timer_get_time();
        if (now - last_log_us > 1000000) {
            last_log_us = now;
            ESP_LOGI(TAG, "Клиент отключился, AID=%d, reason=%d", e->aid, e->reason);
        }
    }
}

void wifi_ap_init(void)
{
    // NVS нужен для калибровки/хранения параметров Wi-Fi.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = strlen(AP_SSID),
            .channel        = AP_CHANNEL,
            .password       = AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(AP_PASS) == 0) {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Точка доступа поднята. SSID:%s пароль:%s", AP_SSID, AP_PASS);
    ESP_LOGI(TAG, "Откройте http://192.168.4.1 после подключения к сети");
}
