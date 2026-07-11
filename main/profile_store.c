#include "profile_store.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "profstore";

#define NVS_NS   "brewprof"
#define NVS_KEY  "lib"        // единый JSON-блоб со всеми профилями
#define NVS_SEEDED "seeded"   // флаг: заводские профили уже засеяны

// Библиотека профилей целиком держится в памяти как cJSON-массив объектов:
//   [ { "name": "...", "steps": [ {"temp":..,"dur":..}, ... ] }, ... ]
static cJSON *s_lib;
static SemaphoreHandle_t s_mtx;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

// ---- Работа с NVS -----------------------------------------------------------

static void persist_locked(void)
{
    char *json = cJSON_PrintUnformatted(s_lib);
    if (!json) {
        ESP_LOGE(TAG, "Не удалось сериализовать библиотеку профилей");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_set_blob(h, NVS_KEY, json, strlen(json) + 1);
        if (err == ESP_OK) nvs_commit(h);
        else ESP_LOGE(TAG, "nvs_set_blob: %s", esp_err_to_name(err));
        nvs_close(h);
    }
    cJSON_free(json);
}

static void load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        s_lib = cJSON_CreateArray();
        return;
    }
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0) {
        char *buf = malloc(len);
        if (buf && nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK) {
            s_lib = cJSON_Parse(buf);
        }
        free(buf);
    }
    nvs_close(h);
    if (!cJSON_IsArray(s_lib)) {
        cJSON_Delete(s_lib);
        s_lib = cJSON_CreateArray();
    }
}

// ---- Вспомогательное --------------------------------------------------------

// Найти профиль по имени; вернуть его cJSON или NULL (под блокировкой).
static cJSON *find_locked(const char *name)
{
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, s_lib) {
        cJSON *n = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) {
            return it;
        }
    }
    return NULL;
}

static cJSON *steps_to_json(const brew_step_t *steps, int count)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddNumberToObject(st, "temp", steps[i].temp_c);
        cJSON_AddNumberToObject(st, "dur",  steps[i].duration_s);
        cJSON_AddItemToArray(arr, st);
    }
    return arr;
}

// -----------------------------------------------------------------------------

void profile_store_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    LOCK();
    load_from_nvs();
    UNLOCK();
    ESP_LOGI(TAG, "Хранилище профилей готово (%d сохранено)", cJSON_GetArraySize(s_lib));
}

cJSON *profile_store_list_json(void)
{
    cJSON *names = cJSON_CreateArray();
    LOCK();
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, s_lib) {
        cJSON *n = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsString(n)) {
            cJSON_AddItemToArray(names, cJSON_CreateString(n->valuestring));
        }
    }
    UNLOCK();
    return names;
}

bool profile_store_save(const char *name, const brew_step_t *steps, int count)
{
    if (!name || !name[0] || strlen(name) >= PROFILE_NAME_MAX) return false;
    if (count < 1 || count > BREW_MAX_STEPS) return false;

    LOCK();
    cJSON *existing = find_locked(name);
    cJSON *steps_json = steps_to_json(steps, count);
    if (existing) {
        // перезапись шагов у существующего профиля
        cJSON_DeleteItemFromObject(existing, "steps");
        cJSON_AddItemToObject(existing, "steps", steps_json);
    } else {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", name);
        cJSON_AddItemToObject(obj, "steps", steps_json);
        cJSON_AddItemToArray(s_lib, obj);
    }
    persist_locked();
    UNLOCK();
    ESP_LOGI(TAG, "Профиль сохранён: \"%s\" (%d шаг.)", name, count);
    return true;
}

bool profile_store_load(const char *name, brew_step_t *steps_out, int *count_out)
{
    if (!name || !steps_out || !count_out) return false;
    bool ok = false;
    LOCK();
    cJSON *obj = find_locked(name);
    if (obj) {
        cJSON *steps = cJSON_GetObjectItem(obj, "steps");
        if (cJSON_IsArray(steps)) {
            int n = 0;
            cJSON *st = NULL;
            cJSON_ArrayForEach(st, steps) {
                if (n >= BREW_MAX_STEPS) break;
                cJSON *t = cJSON_GetObjectItem(st, "temp");
                cJSON *d = cJSON_GetObjectItem(st, "dur");
                if (!cJSON_IsNumber(t) || !cJSON_IsNumber(d)) continue;
                steps_out[n].temp_c     = (float)t->valuedouble;
                steps_out[n].duration_s = (int)d->valuedouble;
                n++;
            }
            if (n > 0) { *count_out = n; ok = true; }
        }
    }
    UNLOCK();
    return ok;
}

bool profile_store_seed(const char *name, const brew_step_t *steps, int count)
{
    // Флаг посева храним отдельно от блоба, чтобы удаление пользователем
    // засеянного профиля не приводило к повторному посеву при перезагрузке.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t seeded = 0;
        esp_err_t err = nvs_get_u8(h, NVS_SEEDED, &seeded);
        nvs_close(h);
        if (err == ESP_OK && seeded) return false;   // уже засевали
    }

    bool ok = profile_store_save(name, steps, count);

    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_SEEDED, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    if (ok) ESP_LOGI(TAG, "Засеян заводской профиль: \"%s\"", name);
    return ok;
}

bool profile_store_delete(const char *name)
{
    if (!name) return false;
    bool ok = false;
    LOCK();
    int idx = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, s_lib) {
        cJSON *n = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) {
            cJSON_DeleteItemFromArray(s_lib, idx);
            persist_locked();
            ok = true;
            break;
        }
        idx++;
    }
    UNLOCK();
    if (ok) ESP_LOGI(TAG, "Профиль удалён: \"%s\"", name);
    return ok;
}
