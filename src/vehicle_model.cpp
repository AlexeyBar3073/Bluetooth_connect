#include <Arduino.h>
#include "vehicle_model.h"
#include "app_config.h"

// -----------------------------------------------------------------------------
// vehicle_model.cpp
// Доменная модель "автомобиля" для симулятора:
// преобразует скорость в RPM, RPM в voltage и определяет текущую передачу.
// -----------------------------------------------------------------------------

// calculateRPM:
// Кусочно-линейная кривая:
// - 0..60 км/ч: RPM_IDLE + speed*12.5
// - 60.. : 1500 + (speed-60)*25
float calculateRPM(float speed) {
    if (speed <= 0.1f) return RPM_IDLE;

    if (speed <= 60.0f) {
        return RPM_IDLE + (speed * 12.5f);
    }
    return 1500.0f + ((speed - 60.0f) * 25.0f);
}

// calculateVoltage:
// Перевод RPM в напряжение (использует Arduino map как линейную шкалу).
// Важно: map работает с int, поэтому аргументы приводятся к int.
float calculateVoltage(float rpm) {
    return map((int)rpm, 750, 6500, (int)VOLTAGE_IDLE * 100, 1600) / 100.0f;
}

// calculateGear:
// Условная 4-ступенчатая АКПП по порогам скорости:
// <20 => 1, <40 => 2, <60 => 3, иначе => 4
int calculateGear(float speed) {
    if (speed < 20.0f) return 1;
    if (speed < 40.0f) return 2;
    if (speed < 60.0f) return 3;
    return 4;
}
