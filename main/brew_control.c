#include "brew_control.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "temp_sensor.h"

static const char *TAG = "brew";

// ---- Параметры регулятора и симулятора --------------------------------------

#define CONTROL_PERIOD_MS   200      // период цикла управления, мс
#define HYST_LOW            0.5f     // включить ТЭН при temp <= target - HYST_LOW
#define REACH_BAND          0.3f     // считаем что вышли на температуру в пределах ±REACH_BAND
#define AMBIENT_C           22.0f    // температура окружающей среды

#define HEATER_GPIO         25       // управляющий вход SSR («+»)
#define TEMP_LIMIT_C        105.0f   // аварийный предел: выше — принудительно выключить ТЭН

// Термо-модель (в единицах "варочного" времени, т.е. до умножения на масштаб).
// Равновесная температура при постоянном нагреве = AMBIENT + HEAT_RATE/COOL_COEF.
// Держим её заметно выше 100 °C, иначе ТЭН "упирается" в потолок и высокие
// шаги профиля (72/78 °C) никогда не достигаются.
#define HEAT_RATE_C_PER_S   0.9f     // прирост при включённом ТЭНе, °C/с
#define COOL_COEF_PER_S     0.006f   // коэффициент остывания к окружающей среде, 1/с
                                     // равновесие ≈ 22 + 0.9/0.006 = 172 °C

// ---- Общее состояние (под мьютексом) ----------------------------------------

typedef struct {
    brew_state_t state;
    brew_step_t  profile[BREW_MAX_STEPS];
    int          step_count;
    int          cur_step;

    float        temp_c;        // текущая (симулированная) температура
    float        target_c;      // целевая температура текущего шага
    bool         heater_on;     // состояние ТЭНа
    bool         sensor_ok;     // есть ли реальный датчик (пока false — симуляция)

    double       step_hold_s;   // накоплено времени выдержки на текущем шаге
    double       total_s;       // общее время с момента старта
    bool         reached;       // вышли ли на температуру текущего шага

    float        time_scale;    // ускорение времени симуляции
} brew_ctx_t;

static brew_ctx_t s_ctx;
static SemaphoreHandle_t s_mtx;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

// -----------------------------------------------------------------------------

static void reset_run_state(void)
{
    s_ctx.cur_step    = 0;
    s_ctx.step_hold_s = 0.0;
    s_ctx.total_s     = 0.0;
    s_ctx.reached     = false;
    s_ctx.heater_on   = false;
    s_ctx.target_c    = (s_ctx.step_count > 0) ? s_ctx.profile[0].temp_c : 0.0f;
}

// Обновление симулированной температуры за dt "варочных" секунд.
static void simulate_temp(float dt_s)
{
    float t = s_ctx.temp_c;
    if (s_ctx.heater_on) {
        t += HEAT_RATE_C_PER_S * dt_s;
    }
    // Ньютоновское остывание к окружающей среде.
    t -= COOL_COEF_PER_S * (t - AMBIENT_C) * dt_s;
    s_ctx.temp_c = t;
}

// Логика bang-bang регулятора с гистерезисом.
static void run_controller(void)
{
    float target = s_ctx.target_c;
    if (s_ctx.temp_c <= target - HYST_LOW) {
        s_ctx.heater_on = true;
    } else if (s_ctx.temp_c >= target) {
        s_ctx.heater_on = false;
    }
    // в мёртвой зоне между (target-HYST_LOW) и target состояние сохраняется
}

// Продвижение по профилю. dt — прошедшее "варочное" время, сек.
static void advance_profile(float dt_s)
{
    if (s_ctx.state != BREW_RUNNING) {
        return;
    }
    if (s_ctx.cur_step >= s_ctx.step_count) {
        s_ctx.state     = BREW_DONE;
        s_ctx.heater_on = false;
        return;
    }

    s_ctx.total_s += dt_s;
    s_ctx.target_c = s_ctx.profile[s_ctx.cur_step].temp_c;

    // Отсчёт выдержки начинается только после выхода на температуру.
    if (!s_ctx.reached && s_ctx.temp_c >= s_ctx.target_c - REACH_BAND) {
        s_ctx.reached = true;
    }
    if (s_ctx.reached) {
        s_ctx.step_hold_s += dt_s;
        if (s_ctx.step_hold_s >= s_ctx.profile[s_ctx.cur_step].duration_s) {
            // шаг завершён — переходим к следующему
            s_ctx.cur_step++;
            s_ctx.step_hold_s = 0.0;
            s_ctx.reached     = false;
            if (s_ctx.cur_step >= s_ctx.step_count) {
                s_ctx.state     = BREW_DONE;
                s_ctx.heater_on = false;
                ESP_LOGI(TAG, "Профиль завершён");
            }
        }
    }
}

static void control_task(void *arg)
{
    (void)arg;
    int64_t last_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));

        int64_t now_us = esp_timer_get_time();
        float real_dt  = (now_us - last_us) / 1e6f;
        last_us = now_us;

        LOCK();
        // При подключённом датчике — только реальное время; ускорение допустимо
        // лишь в режиме симуляции (иначе выдержки профиля идут кратно быстрее).
        float scale = temp_sensor_present() ? 1.0f : s_ctx.time_scale;
        float dt = real_dt * scale;              // "варочное" время

        if (s_ctx.state == BREW_RUNNING) {
            run_controller();
            advance_profile(dt);
        } else {
            s_ctx.heater_on = false;
        }

        // Реальный датчик имеет приоритет над термо-моделью.
        float real_t;
        if (temp_sensor_get(&real_t)) {
            s_ctx.temp_c    = real_t;
            s_ctx.sensor_ok = true;
        } else {
            simulate_temp(dt);
            s_ctx.sensor_ok = false;
        }

        // Аварийный предел: при перегреве принудительно гасим ТЭН.
        if (s_ctx.sensor_ok && s_ctx.temp_c >= TEMP_LIMIT_C) {
            if (s_ctx.heater_on) {
                ESP_LOGW(TAG, "АВАРИЯ: %.1f°C >= предела %.0f°C — ТЭН выключен",
                         s_ctx.temp_c, (float)TEMP_LIMIT_C);
            }
            s_ctx.heater_on = false;
        }

        // Управление твердотельным реле (SSR).
        gpio_set_level(HEATER_GPIO, s_ctx.heater_on ? 1 : 0);
        UNLOCK();
    }
}

// -----------------------------------------------------------------------------

void brew_control_init(void)
{
    s_mtx = xSemaphoreCreateMutex();

    // Управляющий выход на SSR. Первым делом — гарантированно в 0 + подтяжка вниз,
    // чтобы ТЭН был выключен при старте (до внешнего резистора это подстраховка).
    gpio_config_t heater_io = {
        .pin_bit_mask = 1ULL << HEATER_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&heater_io);
    gpio_set_level(HEATER_GPIO, 0);

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state      = BREW_IDLE;
    s_ctx.temp_c     = AMBIENT_C;
    s_ctx.sensor_ok  = false;
    s_ctx.time_scale = 20.0f;   // по умолчанию ускорение x20 для отладки прототипа

    // Профиль при старте не загружаем — пользователь выбирает его из
    // библиотеки в UI (memset уже обнулил step_count).
    s_ctx.step_count = 0;
    reset_run_state();

    xTaskCreate(control_task, "brew_ctrl", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Управление варкой инициализировано (симуляция, x%.0f)", s_ctx.time_scale);
}

bool brew_set_profile(const brew_step_t *steps, int count)
{
    if (count < 1 || count > BREW_MAX_STEPS) {
        return false;
    }
    LOCK();
    memcpy(s_ctx.profile, steps, count * sizeof(brew_step_t));
    s_ctx.step_count = count;
    // Смена профиля останавливает текущую варку.
    s_ctx.state  = BREW_IDLE;
    s_ctx.temp_c = s_ctx.temp_c;   // температуру не сбрасываем — она физическая
    reset_run_state();
    UNLOCK();
    ESP_LOGI(TAG, "Загружен профиль: %d шаг(ов)", count);
    return true;
}

void brew_start(void)
{
    LOCK();
    if (s_ctx.step_count > 0) {
        reset_run_state();
        s_ctx.state = BREW_RUNNING;
        ESP_LOGI(TAG, "Старт варки");
    }
    UNLOCK();
}

void brew_stop(void)
{
    LOCK();
    s_ctx.state     = BREW_IDLE;
    s_ctx.heater_on = false;
    ESP_LOGI(TAG, "Стоп варки");
    UNLOCK();
}

void brew_set_time_scale(float scale)
{
    if (scale < 1.0f)   scale = 1.0f;
    if (scale > 240.0f) scale = 240.0f;
    LOCK();
    s_ctx.time_scale = scale;
    UNLOCK();
}

static const char *state_str(brew_state_t s)
{
    switch (s) {
        case BREW_IDLE:    return "idle";
        case BREW_RUNNING: return "running";
        case BREW_DONE:    return "done";
        default:           return "unknown";
    }
}

cJSON *brew_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    LOCK();

    cJSON_AddStringToObject(root, "state", state_str(s_ctx.state));
    cJSON_AddNumberToObject(root, "temp",        roundf(s_ctx.temp_c * 10) / 10.0f);
    cJSON_AddNumberToObject(root, "target",      s_ctx.target_c);
    cJSON_AddBoolToObject  (root, "heater",      s_ctx.heater_on);
    cJSON_AddBoolToObject  (root, "sensorOk",    s_ctx.sensor_ok);
    cJSON_AddNumberToObject(root, "step",        s_ctx.cur_step);
    cJSON_AddNumberToObject(root, "stepCount",   s_ctx.step_count);
    cJSON_AddNumberToObject(root, "stepHold",    (int)s_ctx.step_hold_s);
    cJSON_AddNumberToObject(root, "totalElapsed",(int)s_ctx.total_s);
    // Отдаём фактическое ускорение: с датчиком оно всегда 1.
    cJSON_AddNumberToObject(root, "timeScale",
                            temp_sensor_present() ? 1.0f : s_ctx.time_scale);

    cJSON *prof = cJSON_CreateArray();
    for (int i = 0; i < s_ctx.step_count; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddNumberToObject(st, "temp", s_ctx.profile[i].temp_c);
        cJSON_AddNumberToObject(st, "dur",  s_ctx.profile[i].duration_s);
        cJSON_AddItemToArray(prof, st);
    }
    cJSON_AddItemToObject(root, "profile", prof);

    UNLOCK();
    return root;
}
