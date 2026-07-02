#pragma once

#include <stdbool.h>
#include "brew_control.h"
#include "cJSON.h"

#define PROFILE_NAME_MAX 32

// Инициализация хранилища именованных профилей (NVS). Вызывать после nvs_flash_init().
void profile_store_init(void);

// Список имён сохранённых профилей: cJSON-массив строк (вызывающий владеет объектом).
cJSON *profile_store_list_json(void);

// Сохранить/перезаписать профиль под именем name. Возвращает false при ошибке.
bool profile_store_save(const char *name, const brew_step_t *steps, int count);

// Загрузить профиль по имени в steps_out (ёмкость BREW_MAX_STEPS).
// При успехе кладёт число шагов в *count_out и возвращает true.
bool profile_store_load(const char *name, brew_step_t *steps_out, int *count_out);

// Удалить профиль по имени. Возвращает false, если не найден.
bool profile_store_delete(const char *name);
