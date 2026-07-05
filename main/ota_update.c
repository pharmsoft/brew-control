#include "ota_update.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "ota";

// Манифест по умолчанию — публичный репозиторий проекта.
#define DEFAULT_MANIFEST_URL \
    "https://raw.githubusercontent.com/pharmsoft/brew-control/main/firmware/manifest.json"

#define NVS_NS          "ota"
#define CHECK_PERIOD_S  3600            // автопроверка раз в час
#define MANIFEST_MAX    1024

typedef enum { OTA_IDLE, OTA_CHECKING, OTA_UPDATING, OTA_UPTODATE, OTA_ERROR } ota_state_t;

static char        s_url[256];
static bool        s_auto = true;
static ota_state_t s_state = OTA_IDLE;
static char        s_avail[32] = {0};
static char        s_msg[96]   = "ещё не проверялось";
static TaskHandle_t s_task = NULL;

// ---- NVS --------------------------------------------------------------------

static void load_config(void)
{
    strlcpy(s_url, DEFAULT_MANIFEST_URL, sizeof(s_url));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_url);
        nvs_get_str(h, "url", s_url, &len);      // при отсутствии — остаётся дефолт
        uint8_t a = 1;
        nvs_get_u8(h, "auto", &a);
        s_auto = a != 0;
        nvs_close(h);
    }
}

static void save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "url", s_url);
        nvs_set_u8(h, "auto", s_auto ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- Версии -----------------------------------------------------------------

// Сравнение версий "a.b.c": >0 если v1 новее v2.
static int ver_cmp(const char *v1, const char *v2)
{
    for (;;) {
        int a = atoi(v1), b = atoi(v2);
        if (a != b) return a - b;
        const char *d1 = strchr(v1, '.'), *d2 = strchr(v2, '.');
        if (!d1 || !d2) return 0;
        v1 = d1 + 1; v2 = d2 + 1;
    }
}

static const char *running_version(void)
{
    return esp_app_get_description()->version;
}

// ---- Загрузка манифеста -----------------------------------------------------

static bool fetch_manifest(char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url               = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            int total = 0, r;
            while (total < (int)cap - 1 &&
                   (r = esp_http_client_read(c, buf + total, cap - 1 - total)) > 0) {
                total += r;
            }
            buf[total] = '\0';
            ok = total > 0;
        } else {
            snprintf(s_msg, sizeof(s_msg), "манифест: HTTP %d", status);
        }
        esp_http_client_close(c);
    } else {
        snprintf(s_msg, sizeof(s_msg), "нет связи с сервером обновлений");
    }
    esp_http_client_cleanup(c);
    return ok;
}

// ---- Основная логика проверки/обновления ------------------------------------

static void do_check(void)
{
    s_state = OTA_CHECKING;
    ESP_LOGI(TAG, "Проверка обновления: %s", s_url);

    char buf[MANIFEST_MAX];
    if (!fetch_manifest(buf, sizeof(buf))) {
        s_state = OTA_ERROR;
        ESP_LOGW(TAG, "%s", s_msg);
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    cJSON *ver  = root ? cJSON_GetObjectItem(root, "version") : NULL;
    cJSON *url  = root ? cJSON_GetObjectItem(root, "url")     : NULL;
    if (!cJSON_IsString(ver) || !cJSON_IsString(url)) {
        cJSON_Delete(root);
        s_state = OTA_ERROR;
        strlcpy(s_msg, "некорректный манифест", sizeof(s_msg));
        return;
    }

    strlcpy(s_avail, ver->valuestring, sizeof(s_avail));
    const char *cur = running_version();

    if (ver_cmp(ver->valuestring, cur) <= 0) {
        s_state = OTA_UPTODATE;
        snprintf(s_msg, sizeof(s_msg), "установлена актуальная версия %s", cur);
        ESP_LOGI(TAG, "%s", s_msg);
        cJSON_Delete(root);
        return;
    }

    // Есть более новая версия — обновляемся.
    ESP_LOGI(TAG, "Найдена версия %s (текущая %s), загрузка: %s",
             ver->valuestring, cur, url->valuestring);
    s_state = OTA_UPDATING;
    snprintf(s_msg, sizeof(s_msg), "загрузка версии %s...", ver->valuestring);

    esp_http_client_config_t http = {
        .url                   = url->valuestring,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 20000,
        .keep_alive_enable     = true,
        // GitHub Releases редиректит на CDN с длинным подписанным URL — буфер
        // запроса по умолчанию (1 КБ) переполняется ("Out of buffer"). Увеличиваем.
        .buffer_size           = 4096,
        .buffer_size_tx        = 4096,
    };
    esp_https_ota_config_t ota = { .http_config = &http };

    esp_err_t err = esp_https_ota(&ota);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA успешно, перезагрузка");
        snprintf(s_msg, sizeof(s_msg), "обновлено, перезагрузка...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        s_state = OTA_ERROR;
        snprintf(s_msg, sizeof(s_msg), "ошибка загрузки: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_msg);
    }
}

static void ota_task(void *arg)
{
    (void)arg;
    // Небольшая задержка после старта, чтобы STA успел подключиться.
    vTaskDelay(pdMS_TO_TICKS(15000));
    for (;;) {
        if (s_auto) do_check();
        // Ждём период ИЛИ внешнего пинка через ota_check_now().
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_PERIOD_S * 1000));
    }
}

// ---- Публичное API ----------------------------------------------------------

void ota_update_init(void)
{
    load_config();
    xTaskCreate(ota_task, "ota", 8192, NULL, 4, &s_task);
    ESP_LOGI(TAG, "Модуль OTA запущен (авто=%d), манифест: %s", s_auto, s_url);
}

void ota_check_now(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void ota_set_config(const char *manifest_url, bool auto_check)
{
    if (manifest_url && manifest_url[0] && strlen(manifest_url) < sizeof(s_url)) {
        strlcpy(s_url, manifest_url, sizeof(s_url));
    }
    s_auto = auto_check;
    save_config();
    ESP_LOGI(TAG, "Настройки OTA обновлены (авто=%d): %s", s_auto, s_url);
}

void ota_status_json(cJSON *root)
{
    static const char *names[] = { "idle", "checking", "updating", "uptodate", "error" };
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state",        names[s_state]);
    cJSON_AddStringToObject(o, "curVersion",   running_version());
    cJSON_AddStringToObject(o, "availVersion", s_avail);
    cJSON_AddStringToObject(o, "msg",          s_msg);
    cJSON_AddStringToObject(o, "url",          s_url);
    cJSON_AddBoolToObject  (o, "auto",         s_auto);
    cJSON_AddItemToObject(root, "ota", o);
}
