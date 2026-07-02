#pragma once

#include <stdbool.h>
#include "cJSON.h"

#define BREW_MAX_STEPS 16

typedef struct {
    float   temp_c;      // целевая температура, °C
    int     duration_s;  // длительность выдержки после выхода на температуру, сек
} brew_step_t;

typedef enum {
    BREW_IDLE = 0,   // ожидание, нагрев выключен
    BREW_RUNNING,    // идёт варка по профилю
    BREW_DONE,       // профиль завершён
} brew_state_t;

// Инициализация состояния и запуск фоновой задачи управления.
void brew_control_init(void);

// Заменить температурный профиль (потокобезопасно). Возвращает false при ошибке.
bool brew_set_profile(const brew_step_t *steps, int count);

// Команды управления.
void brew_start(void);
void brew_stop(void);

// Установить масштаб времени симуляции (1 = реальное время, >1 = ускорение).
void brew_set_time_scale(float scale);

// Сериализовать текущий статус в объект cJSON (вызывающий владеет объектом).
cJSON *brew_status_json(void);
