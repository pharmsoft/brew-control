#include "wifi_ap.h"

#include <string.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

#define AP_SSID     "BrewControl"
#define AP_PASS     "brew1234"   // WPA2, минимум 8 символов
#define AP_CHANNEL  1
#define AP_MAX_CONN 4

#define NVS_NS      "wifi"
#define STA_MAX_RETRY_LOG  10    // ограничение спама лога переподключений

static esp_netif_t *s_sta_netif = NULL;

// Состояние STA (пишется из обработчика событий, читается из HTTP — single writer).
static volatile bool s_sta_connected = false;
static char s_sta_ssid[33] = {0};
static char s_sta_ip[16]   = {0};
static int  s_retry = 0;

// ---- NVS: данные сети роутера -----------------------------------------------

static bool load_sta_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK) &&
              (nvs_get_str(h, "pass", pass, &pass_len) == ESP_OK) &&
              ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

static bool save_sta_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_set_str(h, "ssid", ssid);
    esp_err_t e2 = nvs_set_str(h, "pass", pass);
    if (e1 == ESP_OK && e2 == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK;
}

// ---- Обработчики событий ----------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_sta_connected = false;
            s_sta_ip[0] = '\0';
            if (s_retry < STA_MAX_RETRY_LOG) {
                ESP_LOGW(TAG, "STA отключён, переподключение...");
            }
            s_retry++;
            // Повтор сразу: драйвер сам выдерживает паузу через таймаут подключения,
            // поэтому шторма не будет, а цикл событий не блокируется.
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "К точке доступа подключился клиент");
            break;
        default:
            break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_retry = 0;
        s_sta_connected = true;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "STA подключён, IP: %s", s_sta_ip);
    }
}

// ---- Настройка STA ----------------------------------------------------------

static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

// ---- Публичное API ----------------------------------------------------------

void wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    // Точка доступа (всегда включена для локального доступа).
    wifi_config_t ap = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = strlen(AP_SSID),
            .channel        = AP_CHANNEL,
            .password       = AP_PASS,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(AP_PASS) == 0) ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    // Если есть сохранённые данные роутера — настраиваем STA.
    char ssid[33] = {0}, pass[65] = {0};
    if (load_sta_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        apply_sta_config(ssid, pass);
        ESP_LOGI(TAG, "Найдены данные сети \"%s\", подключаюсь к роутеру", ssid);
    } else {
        ESP_LOGW(TAG, "Данные роутера не заданы — только точка доступа. "
                      "Настройте сеть через веб-интерфейс.");
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP «%s» (пароль %s) поднята, http://192.168.4.1", AP_SSID, AP_PASS);
}

bool wifi_set_sta_creds(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) {
        return false;
    }
    if (!save_sta_creds(ssid, pass ? pass : "")) return false;

    // Переприменяем конфигурацию и переподключаемся.
    esp_wifi_disconnect();
    apply_sta_config(ssid, pass ? pass : "");
    s_retry = 0;
    esp_wifi_connect();
    ESP_LOGI(TAG, "Данные сети обновлены: \"%s\"", ssid);
    return true;
}

void wifi_status_json(cJSON *root)
{
    cJSON *w = cJSON_CreateObject();
    cJSON_AddBoolToObject(w, "connected", s_sta_connected);
    cJSON_AddStringToObject(w, "ssid", s_sta_ssid);
    cJSON_AddStringToObject(w, "ip", s_sta_connected ? s_sta_ip : "");
    if (s_sta_connected) {
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
            cJSON_AddNumberToObject(w, "rssi", rssi);
        }
    }
    cJSON_AddItemToObject(root, "wifi", w);
}
