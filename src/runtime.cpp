#include <Arduino.h>
#include "runtime.h"
#include "app_state.h"
#include "app_config.h"

// -----------------------------------------------------------------------------
// runtime.cpp
// Содержит функции "рантайма":
// - старт/стоп расчётов
// - обновление уровня топлива
// - получение флага подключения
// -----------------------------------------------------------------------------

// getConnectionState: возвращает, есть ли подключенный BT-клиент.
bool getConnectionState() {
    return isBluetoothConnected;
}

// stopAllCalculations:
// Останавливает режим стриминга и сбрасывает вычисленные параметры к базовым.
void stopAllCalculations() {
    // Отключаем активный стриминг и принудительную отправку
    isStreamingActive = false;
    forceSendData = false;

    // Синхронизируем запись currentState через mutex
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        // Сбрасываем основные поля состояния
        currentState.speed = 0.0f;
        currentState.rpm = RPM_IDLE;
        currentState.voltage = VOLTAGE_IDLE;
        currentState.fuel_consumption = 0.0f;
        currentState.remaining_range = 0.0f;
        currentState.gear = 1;
        xSemaphoreGive(dataMutex);
    }

    // Сброс фильтров, чтобы после старта не было "скачков" из прошлых данных
    speedFilter.reset();
    rpmFilter.reset();
    voltageFilter.reset();

    // Сброс истории отправки телеметрии
    lastSentSpeed = -1.0f;
    lastSentRPM = -1.0f;
    lastSentFuel = -1.0f;
    lastSendTime = 0;
    lastKeepAliveTime = 0;
    lastPhysicsCalcTime = 0;

    // Лог в терминал
    Serial.println("Calculations STOPPED");
}

// startAllCalculations:
// Включает режим стриминга и инициализирует таймеры отправки.
void startAllCalculations() {
    // Запускаем стриминг (physicsTask начнёт обновлять currentState, bluetoothTask начнёт рассылку)
    isStreamingActive = true;
    Serial.println("Calculations STARTED");

    // Сбрасываем историю отправок
    lastSentSpeed = -1.0f;
    lastSentRPM = -1.0f;
    lastSentFuel = -1.0f;
    lastSendTime = 0;
    lastKeepAliveTime = 0;
    lastPhysicsCalcTime = millis();
}

// updateFuelLevel:
// Пересчитывает currentState.fuel_level на основе fuel_used_total.
// Также форсирует отправку телеметрии (forceSendData=true).
void updateFuelLevel() {
    // Обновляем состояние под mutex
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        // new_fuel_level: сколько литров осталось = ёмкость - использовано
        float new_fuel_level = deviceSettings.fuel_tank_capacity - currentState.fuel_used_total;

        // Защита от выходов за границы
        if (new_fuel_level < 0) new_fuel_level = 0;
        if (new_fuel_level > deviceSettings.fuel_tank_capacity) {
            new_fuel_level = deviceSettings.fuel_tank_capacity;
        }

        // Округляем до 0.1 литра
        currentState.fuel_level = roundf(new_fuel_level * 10.0f) / 10.0f;

        // Лог изменения топлива
        Serial.printf("Fuel updated: Tank=%.1fL, Used=%.1fL, Remaining=%.1fL\n",
                      deviceSettings.fuel_tank_capacity,
                      currentState.fuel_used_total,
                      currentState.fuel_level);

        xSemaphoreGive(dataMutex);
    }

    // Форсим отправку, чтобы Android увидал изменение топлива/диапазона сразу
    forceSendData = true;
}
