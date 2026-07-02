#include "web_server.h"
#include "brew_control.h"
#include "profile_store.h"

#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "web";

// index.html встроен в прошивку через EMBED_FILES (см. main/CMakeLists.txt).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// ---- Хендлеры ---------------------------------------------------------------

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)index_html_start, len);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = brew_status_json();
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    cJSON_free(out);
    return ESP_OK;
}

// Прочитать тело запроса в буфер (с завершающим нулём).
static char *read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        return NULL;
    }
    char *buf = malloc(total + 1);
    if (!buf) {
        return NULL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[total] = '\0';
    return buf;
}

// Распарсить cJSON-массив шагов в brew_step_t[]. Возвращает число шагов (0 при ошибке).
static int parse_steps(const cJSON *steps, brew_step_t *out)
{
    if (!cJSON_IsArray(steps)) return 0;
    int n = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, steps) {
        if (n >= BREW_MAX_STEPS) break;
        cJSON *t = cJSON_GetObjectItem(it, "temp");
        cJSON *d = cJSON_GetObjectItem(it, "dur");
        if (!cJSON_IsNumber(t) || !cJSON_IsNumber(d)) continue;
        out[n].temp_c     = (float)t->valuedouble;
        out[n].duration_s = (int)d->valuedouble;
        n++;
    }
    return n;
}

// POST /api/profile  тело: {"steps":[{"temp":50,"dur":600}, ...]}
static esp_err_t profile_post_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    cJSON *root  = cJSON_Parse(body);
    free(body);
    cJSON *steps = root ? cJSON_GetObjectItem(root, "steps") : NULL;

    brew_step_t parsed[BREW_MAX_STEPS];
    int n = parse_steps(steps, parsed);
    cJSON_Delete(root);

    if (n < 1 || !brew_set_profile(parsed, n)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid profile");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/control  тело: {"action":"start"|"stop", "timeScale":20}
static esp_err_t control_post_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    cJSON *ts = cJSON_GetObjectItem(root, "timeScale");
    if (cJSON_IsNumber(ts)) {
        brew_set_time_scale((float)ts->valuedouble);
    }

    cJSON *act = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(act)) {
        if (strcmp(act->valuestring, "start") == 0)      brew_start();
        else if (strcmp(act->valuestring, "stop") == 0)  brew_stop();
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// GET /api/profiles  →  {"profiles":["имя1","имя2",...]}
static esp_err_t profiles_list_handler(httpd_req_t *req)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "profiles", profile_store_list_json());
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{\"profiles\":[]}");
    cJSON_free(out);
    return ESP_OK;
}

// POST /api/profiles/save  тело: {"name":"...","steps":[...]}
static esp_err_t profiles_save_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *name  = root ? cJSON_GetObjectItem(root, "name")  : NULL;
    cJSON *steps = root ? cJSON_GetObjectItem(root, "steps") : NULL;

    brew_step_t parsed[BREW_MAX_STEPS];
    int n = parse_steps(steps, parsed);
    bool ok = cJSON_IsString(name) && n > 0 &&
              profile_store_save(name->valuestring, parsed, n);
    cJSON_Delete(root);

    if (!ok) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "save failed"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/profiles/load  тело: {"name":"..."}  →  {"name":..,"steps":[..]}
// Также делает загруженный профиль активным в контроллере.
static esp_err_t profiles_load_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *name = root ? cJSON_GetObjectItem(root, "name") : NULL;

    brew_step_t steps[BREW_MAX_STEPS];
    int n = 0;
    bool ok = cJSON_IsString(name) &&
              profile_store_load(name->valuestring, steps, &n);
    if (ok) {
        brew_set_profile(steps, n);   // сразу делаем активным
    }

    cJSON *resp = cJSON_CreateObject();
    if (ok) {
        cJSON_AddStringToObject(resp, "name", name->valuestring);
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < n; i++) {
            cJSON *st = cJSON_CreateObject();
            cJSON_AddNumberToObject(st, "temp", steps[i].temp_c);
            cJSON_AddNumberToObject(st, "dur",  steps[i].duration_s);
            cJSON_AddItemToArray(arr, st);
        }
        cJSON_AddItemToObject(resp, "steps", arr);
    }
    cJSON_Delete(root);

    if (!ok) {
        cJSON_Delete(resp);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    cJSON_free(out);
    return ESP_OK;
}

// POST /api/profiles/delete  тело: {"name":"..."}
static esp_err_t profiles_delete_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *name = root ? cJSON_GetObjectItem(root, "name") : NULL;
    bool ok = cJSON_IsString(name) && profile_store_delete(name->valuestring);
    cJSON_Delete(root);

    if (!ok) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// -----------------------------------------------------------------------------

void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 12;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось запустить HTTP-сервер");
        return;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get_handler },
        { .uri = "/api/profile", .method = HTTP_POST, .handler = profile_post_handler },
        { .uri = "/api/control", .method = HTTP_POST, .handler = control_post_handler },
        { .uri = "/api/profiles",        .method = HTTP_GET,  .handler = profiles_list_handler },
        { .uri = "/api/profiles/save",   .method = HTTP_POST, .handler = profiles_save_handler },
        { .uri = "/api/profiles/load",   .method = HTTP_POST, .handler = profiles_load_handler },
        { .uri = "/api/profiles/delete", .method = HTTP_POST, .handler = profiles_delete_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP-сервер запущен на порту %d", config.server_port);
}
