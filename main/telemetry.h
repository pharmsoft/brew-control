#pragma once

#include <stdbool.h>
#include "cJSON.h"

// Телеметрия и удалённое управление через MQTT (TLS).
//  - публикует статус в <topic>/telemetry раз в несколько секунд;
//  - presence: <topic>/online (retained, "1"/LWT "0");
//  - принимает команды в <topic>/cmd: {"action":"start|stop|check_update"}.
// Настройки брокера задаются из веб-интерфейса и хранятся в NVS.
void telemetry_init(void);

// Задать настройки MQTT и переподключиться (сохраняется в NVS).
void telemetry_set_config(const char *uri, const char *user, const char *pass,
                          const char *topic, bool enabled);

// Добавить в root поле "mqtt" (enabled, connected, uri, topic).
void telemetry_status_json(cJSON *root);
