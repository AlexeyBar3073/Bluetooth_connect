#include <Arduino.h>
#include "telemetry_simulator.h"
#include "app_state.h"
#include "app_config.h"
#include "vehicle_model.h"
#include "fuel_model.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// -----------------------------------------------------------------------------
// telemetry_simulator.cpp
// Реализация симулятора телеметрии:
// physicsTask читает аналоговый сигнал (потенциометр) и вычисляет:
// скорость, RPM, voltage, передачи, пробег и топливо.
// -----------------------------------------------------------------------------

// physicsTask:
// FreeRTOS-задача, выполняющая цикл вычислений телеметрии.
void physicsTask(void* parameter) {
    // parameter: по текущей реализации не используется.
    (void)parameter;

    // last_calc_time: когда последний раз обновляли интегрирование пробега/топлива.
    unsigned long last_calc_time = millis();
    // last_adc_time: когда последний раз читали АЦП (ограничиваем частоту ADC).
    unsigned long last_adc_time = millis();

    // accumulated_trip_a: накопленный пробег поездки A (км).
    float& accumulated_trip_a = tripAccumulatorA;
    // accumulated_trip_b: накопленный пробег поездки B (км).
    float& accumulated_trip_b = tripAccumulatorB;
    // accumulated_odometer: глобальный накопленный одометр (км) для коррекции odo.
    float& accumulated_odometer = odometerAccumulator;

    // filtered_fuel_per_sec: сглаженный расход топлива в л/с (для расчёта л/100км).
    float filtered_fuel_per_sec = 0;

    // Конфигурация АЦП: 12-бит.
    analogReadResolution(12);
    // Настройка затухания АЦП.
    analogSetAttenuation(ADC_11db);

    while (1) {
        // Если стриминг выключен — "не крутим" модель.
        if (!isStreamingActive) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        // current_time: текущее время для вычисления delta_ms.
        unsigned long current_time = millis();

        // Ограничиваем частоту чтения АЦП ~50 Гц (каждые 20 мс).
        if (current_time - last_adc_time >= 20) {
            // raw_value: сырой АЦП (0..4095 при 12-бит).
            int raw_value = analogRead(POTENTIOMETER_PIN);
            // raw_speed: перевод АЦП в км/ч без учёта фильтра.
            float raw_speed = (raw_value / ADC_MAX) * SPEED_MAX;

            // filtered_speed: сглаженная скорость.
            float filtered_speed = speedFilter.update(raw_speed);

            // Записываем в currentState под mutex.
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                // Округляем скорость до 0.1 км/ч.
                currentState.speed = roundf(filtered_speed * 10.0f) / 10.0f;

                // Gear: простая 4-ступенчатая логика по скорости.
                currentState.gear = calculateGear(currentState.speed);

                // rawRPM: модель RPM по скорости (vehicle_model).
                float rawRPM = calculateRPM(currentState.speed);
                // currentState.rpm: отфильтрованный RPM.
                currentState.rpm = roundf(rpmFilter.update(rawRPM));

                // Voltage: модель напряжения (из RPM).
                float rawVoltage = calculateVoltage(currentState.rpm);
                // Округляем voltage до 0.01? (в коде *100 и /100)
                currentState.voltage = roundf(rawVoltage * 100.0f) / 100.0f;

                // delta_ms: сколько времени прошло с прошлого интегрирования.
                unsigned long delta_ms =
                    (last_calc_time > 0) ? (current_time - last_calc_time) : 50;

                // distance_km: сколько "проехали" за delta_ms.
                float distance_km = currentState.speed * (delta_ms / 3600000.0f);

                // Инкремент пробегов.
                accumulated_trip_a += distance_km;
                accumulated_trip_b += distance_km;
                accumulated_odometer += distance_km;

                // Сохраняем текущие значения пробега.
                currentState.trip_a = roundf(accumulated_trip_a * 100.0f) / 100.0f;
                currentState.trip_b = roundf(accumulated_trip_b * 100.0f) / 100.0f;
                currentState.odometer = roundf(accumulated_odometer * 100.0f) / 100.0f;

                // Топливо считаем только после получения settings от Android.
                if (settingsReceived) {
                    // fuel_used: сколько топлива израсходовали за delta_ms.
                    float fuel_used =
                        calculateFuelUsage(currentState.rpm, delta_ms, deviceSettings, settingsReceived);
                    currentState.fuel_used_total += fuel_used;

                    // new_fuel_level: вычисленный остаток топлива (л).
                    float new_fuel_level = deviceSettings.fuel_tank_capacity - currentState.fuel_used_total;
                    if (new_fuel_level < 0) new_fuel_level = 0;

                    // Плавная корректировка уровня топлива (чтобы модель не прыгала).
                    currentState.fuel_level = currentState.fuel_level * 0.9f + new_fuel_level * 0.1f;
                    currentState.fuel_level = roundf(currentState.fuel_level * 10.0f) / 10.0f;

                    // raw_fuel_per_sec: "мгновенный" расход топлива (л/с) за delta_ms.
                    float raw_fuel_per_sec = (delta_ms > 0) ? fuel_used / (delta_ms / 1000.0f) : 0;
                    // filtered_fuel_per_sec: сглаженный расход для устойчивого л/100км.
                    filtered_fuel_per_sec = filtered_fuel_per_sec * 0.8f + raw_fuel_per_sec * 0.2f;

                    // Пересчитываем расход в л/100км.
                    currentState.fuel_consumption =
                        calculateInstantConsumption(currentState.speed, filtered_fuel_per_sec);
                    currentState.fuel_consumption =
                        roundf(currentState.fuel_consumption * 10.0f) / 10.0f;

                    // Запас хода — производная величина от fuel_level и fuel_consumption.
                    currentState.remaining_range =
                        calculateRemainingRange(currentState.fuel_level, currentState.fuel_consumption);
                    currentState.remaining_range =
                        roundf(currentState.remaining_range * 10.0f) / 10.0f;
                }

                // debug_counter: счётчик для периодического вывода отладочной информации.
                static int debug_counter = 0;
                debug_counter++;
                if (debug_counter >= 5) {
                    debug_counter = 0;
                    Serial.printf(
                        "Physics: Speed=%.1f km/h, RPM=%.0f, Gear=%d, RawADC=%d, RawSpeed=%.1f\n",
                        currentState.speed,
                        currentState.rpm,
                        currentState.gear,
                        raw_value,
                        raw_speed);
                }

                // Освобождаем mutex после записи currentState.
                xSemaphoreGive(dataMutex);
                // Обновляем время последнего интегрирования.
                last_calc_time = current_time;
            }

            // Запоминаем время последнего чтения АЦП.
            last_adc_time = current_time;
        }

        // Небольшая задержка, чтобы не грузить CPU.
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
