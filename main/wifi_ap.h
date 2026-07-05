#pragma once

#include <stdbool.h>
#include "cJSON.h"

// Поднять Wi-Fi в режиме AP+STA:
//  - AP  «BrewControl» (192.168.4.1) всегда доступен для локальной настройки;
//  - STA подключается к домашнему роутеру, если в NVS сохранены данные сети.
void wifi_init(void);

// Сохранить данные сети роутера в NVS и переподключить STA. false при ошибке.
bool wifi_set_sta_creds(const char *ssid, const char *pass);

// Добавить в объект root поле "wifi" с состоянием STA (ssid, подключение, ip, rssi).
void wifi_status_json(cJSON *root);
