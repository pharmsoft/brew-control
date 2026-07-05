#pragma once

#include "cJSON.h"

// Удалённое обновление «по воздуху» с GitHub:
//  - периодически (и по запросу) скачивает манифест по HTTPS;
//  - сравнивает версию с текущей прошивкой;
//  - при наличии новой качает образ и обновляется (откат при сбое уже включён).
void ota_update_init(void);

// Запросить внеочередную проверку обновления (неблокирующе).
void ota_check_now(void);

// Задать URL манифеста и автопроверку (сохраняется в NVS). Пустой url — оставить текущий.
void ota_set_config(const char *manifest_url, bool auto_check);

// Добавить в root поле "ota" с состоянием (state, curVersion, availVersion, msg, url, auto).
void ota_status_json(cJSON *root);
