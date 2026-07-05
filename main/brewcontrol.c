#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"

#include "wifi_ap.h"
#include "web_server.h"
#include "brew_control.h"
#include "profile_store.h"

static const char *TAG = "app";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:  return "включение питания";
        case ESP_RST_SW:       return "программный сброс";
        case ESP_RST_PANIC:    return "паника/исключение";
        case ESP_RST_INT_WDT:  return "watchdog (прерывания)";
        case ESP_RST_TASK_WDT: return "watchdog (задачи)";
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "просадка питания";
        case ESP_RST_DEEPSLEEP:return "выход из deep sleep";
        default:               return "прочее";
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "=== BrewControl v%s (сборка %s) ===", app->version, app->date);
    ESP_LOGI(TAG, "Причина перезагрузки: %s", reset_reason_str(esp_reset_reason()));

    brew_control_init();   // движок профилей + симулятор температуры
    wifi_init();           // AP+STA: точка доступа + подключение к роутеру (+ init NVS)
    profile_store_init();  // библиотека именованных профилей в NVS
    web_server_start();    // веб-интерфейс + REST API + OTA

    // Дошли сюда без паники/зависания — считаем прошивку исправной и отменяем
    // откат. Без этого следующая перезагрузка вернёт предыдущую прошивку.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Прошивка подтверждена как исправная (откат отменён)");
    }

    ESP_LOGI(TAG, "Готово. AP 'BrewControl' / http://192.168.4.1");
}
