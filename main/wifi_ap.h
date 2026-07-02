#pragma once

// Поднять Wi-Fi в режиме точки доступа (SoftAP).
// SSID/пароль заданы в wifi_ap.c. Веб-интерфейс будет доступен на 192.168.4.1.
void wifi_ap_init(void);
