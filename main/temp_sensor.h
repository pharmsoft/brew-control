#pragma once

#include <stdbool.h>

// Драйвер датчика температуры DS18B20 (1-Wire через RMT).
// Пин задаётся в temp_sensor.c (DS18B20_GPIO). Запускает фоновую задачу опроса.
void temp_sensor_init(void);

// Есть ли исправный датчик с валидным последним измерением.
bool temp_sensor_present(void);

// Последнее измеренное значение, °C. Возвращает false, если датчика нет/чтение невалидно.
bool temp_sensor_get(float *celsius);
