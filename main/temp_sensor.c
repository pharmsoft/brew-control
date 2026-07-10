#include "temp_sensor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "onewire_bus.h"
#include "ds18b20.h"

static const char *TAG = "ds18b20";

// «P4» = GPIO4. Поменяй здесь, если датчик на другом пине.
#define DS18B20_GPIO   4
#define CONV_WAIT_MS   800     // время преобразования для 12 бит (~750 мс) + запас

static ds18b20_device_handle_t s_dev = NULL;
static volatile bool  s_present = false;
static volatile float s_temp    = 0.0f;

static void sensor_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_dev) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        esp_err_t err = ds18b20_trigger_temperature_conversion(s_dev);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(CONV_WAIT_MS));
            float t = 0;
            if (ds18b20_get_temperature(s_dev, &t) == ESP_OK) {
                s_temp    = t;
                s_present = true;
                ESP_LOGI(TAG, "температура: %.2f °C", t);
            } else {
                s_present = false;
                ESP_LOGW(TAG, "ошибка чтения температуры");
            }
        } else {
            s_present = false;
            ESP_LOGW(TAG, "ошибка запуска преобразования: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void temp_sensor_init(void)
{
    onewire_bus_handle_t bus = NULL;
    onewire_bus_config_t bus_cfg = { .bus_gpio_num = DS18B20_GPIO };
    onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };

    esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "не удалось поднять 1-Wire на GPIO%d: %s",
                 DS18B20_GPIO, esp_err_to_name(err));
        return;
    }

    ds18b20_config_t ds_cfg = {};
    err = ds18b20_new_device_from_bus(bus, &ds_cfg, &s_dev);
    if (err != ESP_OK || !s_dev) {
        s_dev = NULL;
        ESP_LOGW(TAG, "DS18B20 не найден на GPIO%d. Проверь: подтяжку 4.7к на 3.3В, "
                      "питание датчика, распайку DATA.", DS18B20_GPIO);
    } else {
        ds18b20_set_resolution(s_dev, DS18B20_RESOLUTION_12B);
        ESP_LOGI(TAG, "DS18B20 найден на GPIO%d (разрешение 12 бит)", DS18B20_GPIO);
    }

    xTaskCreate(sensor_task, "ds18b20", 4096, NULL, 4, NULL);
}

bool temp_sensor_present(void)
{
    return s_present;
}

bool temp_sensor_get(float *celsius)
{
    if (s_present && celsius) {
        *celsius = s_temp;
        return true;
    }
    return false;
}
