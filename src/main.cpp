#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "app_state.h"
#include "app_config.h"
#include "connection.h"
#include "telemetry_simulator.h"
#include "bluetooth_transport.h"

// -----------------------------------------------------------------------------
// main.cpp
// Точка входа для ESP32:
// - инициализация Bluetooth и mutex
// - создание FreeRTOS-задач:
//   1) connectionTask: мониторит подключение по Bluetooth
//   2) physicsTask: обновляет CarState (симулятор "физики")
//   3) bluetoothTask: принимает команды/стримит телеметрию
// -----------------------------------------------------------------------------

// setup: вызывается однократно при старте контроллера.
void setup() {
    // Инициализация Serial для отладки в терминал
    Serial.begin(115200);
    delay(2000);

    // Баннер/подсказки в терминал (удобно сверять, что прошивка запущена)
    Serial.println("\n\n=== Car Physics Simulator ESP32 ===");
    Serial.println("4-Speed Automatic Transmission");
    Serial.println("Adaptive Filter + Safe JSON");
    Serial.println("FIXED: RPM curve 0-60: 12.5, 60+: 25");
    Serial.println("=============================================\n");

    // Создаём mutex для защиты shared-state (currentState, настройки и т.п.)
    dataMutex = xSemaphoreCreateMutex();
    if (dataMutex == NULL) {
        Serial.println("ERROR: Failed to create mutex!");
        while (1)
            ;
    }

    // Запускаем Bluetooth SPP (Classic). Название видно на стороне Android.
    SerialBT.begin("Car Simulator");
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // Инициализация флагов/состояний протокола
    isBluetoothConnected = false;
    isStreamingActive = false;
    currentProgramState = STATE_WAITING_CONNECTION;
    settingsReceived = false;

    // Подсказки: какие диапазоны/передачи использует модель
    Serial.println("System initialized.");
    Serial.println("\n=== 4-SPEED TRANSMISSION ===");
    Serial.println("  1st gear:   0-20 km/h");
    Serial.println("  2nd gear:  20-40 km/h");
    Serial.println("  3rd gear:  40-60 km/h");
    Serial.println("  4th gear:   60+ km/h");

    Serial.println("\n=== RPM CURVE (FIXED) ===");
    Serial.println("   0 km/h →  750 RPM");
    Serial.println("  60 km/h → 1500 RPM");
    Serial.println("  80 km/h → 2000 RPM");
    Serial.println(" 100 km/h → 2500 RPM");
    Serial.println(" 120 km/h → 3000 RPM");
    Serial.println(" 140 km/h → 3500 RPM");
    Serial.println(" 160 km/h → 4000 RPM");
    Serial.println(" 180 km/h → 4500 RPM");
    Serial.println(" 200 km/h → 5000 RPM");

    Serial.println("\n=== ADAPTIVE FILTER ===");
    Serial.println("  Noise level: 0.2 km/h");
    Serial.println("  Fast threshold: 5.0 km/h");
    Serial.println("  Alpha slow: 0.05");
    Serial.println("  Alpha fast: 0.7");

    Serial.println("\n=== COMMANDS ===");
    Serial.println("  {\"settings\":{\"fuel_tank_capacity\":60.0, \"initial_odometer\":15234.5}}");
    Serial.println("  {\"command\":\"GET_DATA\"} - Start");
    Serial.println("  {\"command\":\"STOP\"} - Stop");
    Serial.println("  {\"command\":\"RESET_FUEL\"} - Full tank");
    Serial.println("  {\"command\":\"RESET_TRIP_A\"} - Reset trip A");
    Serial.println("  {\"command\":\"RESET_TRIP_B\"} - Reset trip B");
    Serial.println("  {\"command\":\"RESET_ALL_TRIPS\"} - Reset all trips");
    Serial.println("  {\"command\":\"CORRECT_ODO\", \"odo\": 15234.5} - Correct odometer");
    Serial.println("=============================================");


    // Создание FreeRTOS задач:
    // - Pinned to core: распределяем по ядрам ESP32 для предсказуемости.
    // - TASK_STACK_SIZE: размер стека каждой задачи.
    // - priority: выше у connectionTask (3), затем physics (2), затем bluetooth (1).
    xTaskCreatePinnedToCore(connectionTask, "Connection", TASK_STACK_SIZE, NULL, 3, &connectionTaskHandle, 0);
    xTaskCreatePinnedToCore(physicsTask, "Physics", TASK_STACK_SIZE, NULL, 2, &physicsTaskHandle, 0);
    xTaskCreatePinnedToCore(bluetoothTask, "Bluetooth", TASK_STACK_SIZE, NULL, 1, &bluetoothTaskHandle, 1);

    // Сообщение в терминал о готовности
    Serial.println("Tasks created. Ready for connection.\n");
}

// loop: в этой архитектуре вся логика в задачах FreeRTOS,
// поэтому loop просто ждёт.
void loop() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}
