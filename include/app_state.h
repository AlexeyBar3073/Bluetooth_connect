#pragma once

// -----------------------------------------------------------------------------
// app_state.h
// Централизованное объявление глобального состояния приложения.
// Фактические определения переменных вынесены в `src/app_state.cpp`.
// -----------------------------------------------------------------------------

#include <Arduino.h>
#include "BluetoothSerial.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "app_types.h"
#include "filters.h"

// SerialBT: интерфейс BluetoothSerial (Classic BT SPP).
extern BluetoothSerial SerialBT;

// Хендлы FreeRTOS-задач (создаются в src/main.cpp).
extern TaskHandle_t physicsTaskHandle;
extern TaskHandle_t bluetoothTaskHandle;
extern TaskHandle_t connectionTaskHandle;

// deviceSettings: калибровки/параметры авто, присылаемые Android в settings.
extern Settings deviceSettings;

// currentState: текущее состояние телеметрии (скорость, RPM, топливо и т.д.).
extern CarState currentState;

// speedFilter: адаптивный фильтр для скорости.
extern AdaptiveFilter speedFilter;
// rpmFilter: простой экспоненциальный фильтр для RPM.
extern SimpleFilter rpmFilter;
// voltageFilter: простой экспоненциальный фильтр для voltage.
extern SimpleFilter voltageFilter;

// lastSentSpeed: последняя отправленная скорость (для пороговой отправки).
extern float lastSentSpeed;
// lastSentRPM: последняя отправленная RPM.
extern float lastSentRPM;
// lastSentFuel: последняя отправленная величина fuel_level.
extern float lastSentFuel;
// odometerAccumulator: внутренний счётчик пробега для physicsTask.
extern float odometerAccumulator;

// lastSendTime: момент времени (millis), когда последний раз ушло сообщение data.
extern unsigned long lastSendTime;

// lastKeepAliveTime: момент времени, когда отправлялась keep-alive телеметрия.
extern unsigned long lastKeepAliveTime;

// lastPhysicsCalcTime: внутреннее время для интегрирования пробега/топлива.
extern unsigned long lastPhysicsCalcTime;

// forceSendData: флаг принудительной отправки (ресет топлива/трипов/обновление).
extern volatile bool forceSendData;

// isBluetoothConnected: есть ли активное клиентское подключение на стороне Android.
extern volatile bool isBluetoothConnected;

// isStreamingActive: активирован ли стрим (GET_DATA) и можно ли обновлять/шлём телеметрию.
extern volatile bool isStreamingActive;

// dataMutex: mutex, защищающий чтение/запись currentState и связанных глобальных данных.
extern SemaphoreHandle_t dataMutex;

// tripAccumulatorA: накопленный пробег поездки A (для physicsTask и сброса).
extern float tripAccumulatorA;
// tripAccumulatorB: накопленный пробег поездки B (для physicsTask и сброса).
extern float tripAccumulatorB;

// currentProgramState: состояние логики приложения/протокола.
extern volatile ProgramState currentProgramState;

// settingsReceived: получены ли settings от Android (нужно для расчёта топлива).
extern volatile bool settingsReceived;
