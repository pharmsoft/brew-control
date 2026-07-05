#include "telemetry.h"
#include "brew_control.h"
#include "wifi_ap.h"
#include "ota_update.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "nvs.h"

static const char *TAG = "mqtt";

#define NVS_NS         "mqtt"
#define PUBLISH_PERIOD_MS  10000

static char s_uri[128];
static char s_user[64];
static char s_pass[64];
static char s_topic[48] = "brewcontrol";
static bool s_enabled = false;

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;

// ---- NVS --------------------------------------------------------------------

static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n;
    n = sizeof(s_uri);   nvs_get_str(h, "uri",   s_uri,   &n);
    n = sizeof(s_user);  nvs_get_str(h, "user",  s_user,  &n);
    n = sizeof(s_pass);  nvs_get_str(h, "pass",  s_pass,  &n);
    n = sizeof(s_topic); nvs_get_str(h, "topic", s_topic, &n);
    uint8_t en = 0; nvs_get_u8(h, "en", &en); s_enabled = en != 0;
    nvs_close(h);
}

static void save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "uri",   s_uri);
    nvs_set_str(h, "user",  s_user);
    nvs_set_str(h, "pass",  s_pass);
    nvs_set_str(h, "topic", s_topic);
    nvs_set_u8 (h, "en",    s_enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// ---- Топики -----------------------------------------------------------------

static void topic(char *out, size_t cap, const char *suffix)
{
    snprintf(out, cap, "%s/%s", s_topic, suffix);
}

// ---- Команды ----------------------------------------------------------------

static void handle_cmd(const char *data, int len)
{
    cJSON *j = cJSON_ParseWithLength(data, len);
    cJSON *a = j ? cJSON_GetObjectItem(j, "action") : NULL;
    if (cJSON_IsString(a)) {
        ESP_LOGI(TAG, "Команда: %s", a->valuestring);
        if      (!strcmp(a->valuestring, "start"))        brew_start();
        else if (!strcmp(a->valuestring, "stop"))         brew_stop();
        else if (!strcmp(a->valuestring, "check_update")) ota_check_now();
    }
    cJSON_Delete(j);
}

// ---- Телеметрия -------------------------------------------------------------

static void publish_telemetry(void)
{
    if (!s_connected || !s_client) return;

    cJSON *root = brew_status_json();       // temp/target/state/heater/step/...
    wifi_status_json(root);
    ota_status_json(root);
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddNumberToObject(sys, "uptime",   (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(sys, "freeHeap", esp_get_free_heap_size());
    cJSON_AddStringToObject(sys, "version",  esp_app_get_description()->version);
    cJSON_AddItemToObject(root, "sys", sys);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) {
        char t[64]; topic(t, sizeof(t), "telemetry");
        esp_mqtt_client_publish(s_client, t, s, 0, 0, 0);
        cJSON_free(s);
    }
}

// ---- События MQTT -----------------------------------------------------------

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    char t[64];
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "Подключено к брокеру");
            topic(t, sizeof(t), "online");
            esp_mqtt_client_publish(s_client, t, "1", 0, 1, 1);   // retained
            topic(t, sizeof(t), "cmd");
            esp_mqtt_client_subscribe(s_client, t, 1);
            publish_telemetry();
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "Отключено от брокера");
            break;
        case MQTT_EVENT_DATA:
            handle_cmd(e->data, e->data_len);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "Ошибка MQTT");
            break;
        default:
            break;
    }
}

// ---- Жизненный цикл клиента -------------------------------------------------

static void client_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
}

static void client_start(void)
{
    if (!s_enabled || s_uri[0] == '\0') {
        ESP_LOGW(TAG, "MQTT выключен или не задан URI");
        return;
    }
    char will[64]; topic(will, sizeof(will), "online");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.username = s_user[0] ? s_user : NULL,
        .credentials.authentication.password = s_pass[0] ? s_pass : NULL,
        .session.last_will.topic = will,
        .session.last_will.msg = "0",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };
    // Для mqtts:// проверяем сертификат брокера по встроенному CA-bundle.
    if (strncmp(s_uri, "mqtts", 5) == 0) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "Не удалось создать MQTT-клиент"); return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT-клиент запущен: %s (топик %s)", s_uri, s_topic);
}

static void telemetry_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_PERIOD_MS));
        publish_telemetry();
    }
}

// ---- Публичное API ----------------------------------------------------------

void telemetry_init(void)
{
    load_config();
    client_start();
    xTaskCreate(telemetry_task, "mqtt_pub", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Модуль телеметрии инициализирован (вкл=%d)", s_enabled);
}

void telemetry_set_config(const char *uri, const char *user, const char *pass,
                          const char *topic_base, bool enabled)
{
    if (uri)        strlcpy(s_uri,   uri,        sizeof(s_uri));
    if (user)       strlcpy(s_user,  user,       sizeof(s_user));
    if (pass)       strlcpy(s_pass,  pass,       sizeof(s_pass));
    if (topic_base && topic_base[0]) strlcpy(s_topic, topic_base, sizeof(s_topic));
    s_enabled = enabled;
    save_config();

    // Переподключаемся с новыми настройками.
    client_stop();
    client_start();
    ESP_LOGI(TAG, "Настройки MQTT обновлены (вкл=%d): %s", s_enabled, s_uri);
}

void telemetry_status_json(cJSON *root)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddBoolToObject  (m, "enabled",   s_enabled);
    cJSON_AddBoolToObject  (m, "connected", s_connected);
    cJSON_AddStringToObject(m, "uri",       s_uri);
    cJSON_AddStringToObject(m, "topic",     s_topic);
    cJSON_AddItemToObject(root, "mqtt", m);
}
