#include <Arduino.h>
#include "connection.h"
#include "app_state.h"
#include "app_config.h"
#include "runtime.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// -----------------------------------------------------------------------------
// connection_task.cpp
// Задача контроля подключения по Bluetooth и переключение режима логики.
// -----------------------------------------------------------------------------

// setConnectionState:
// Обновляет флаг isBluetoothConnected и делает побочные эффекты:
// - при подключении: включает LED
// - при отключении: останавливает расчёты и сбрасывает settingsReceived
void setConnectionState(bool connected) {
    // lastState: запоминаем предыдущее состояние, чтобы не спамить логами.
    static bool lastState = false;

    // Изменилось ли состояние подключения?
    if (connected != lastState) {
        // isBluetoothConnected: глобальный флаг для других задач.
        isBluetoothConnected = connected;
        // Обновляем lastState.
        lastState = connected;

        // Побочные эффекты для индикации и сброса состояния
        if (connected) {
            Serial.println("\n=== Bluetooth CONNECTED ===");
            digitalWrite(LED_BUILTIN, HIGH);
        } else {
            Serial.println("\n=== Bluetooth DISCONNECTED ===");
            // Останавливаем стриминг/обновления при разрыве соединения.
            stopAllCalculations();
            // Возвращаемся в ожидание подключения.
            currentProgramState = STATE_WAITING_CONNECTION;
            // settingsReceived сбрасывается: пока не придут settings, топливо не считается.
            settingsReceived = false;
            digitalWrite(LED_BUILTIN, LOW);
        }
    }
}

// connectionTask:
// Периодически проверяет: есть ли клиент у SerialBT (Android подключен или нет).
// Также делает "anti-reconnect debounce": если клиент отвалился и сразу подключился,
// не спамим состояниями.
void connectionTask(void* parameter) {
    // parameter: сейчас не используется (оставлен по сигнатуре FreeRTOS).
    (void)parameter;

    // wasConnected: предыдущее состояние соединения.
    bool wasConnected = false;
    // lastConnectionTime: когда в последний раз фиксировали переход в connected.
    unsigned long lastConnectionTime = 0;

    while (1) {
        // currentConnected: текущее состояние (есть ли клиент).
        bool currentConnected = SerialBT.hasClient();
        // currentTime: текущее время (millis()) для debounce.
        unsigned long currentTime = millis();

        if (currentConnected && !wasConnected) {
            if (currentTime - lastConnectionTime < CONNECTION_DEBOUNCE_MS) {
                Serial.println("Ignoring rapid reconnection - debouncing");
                vTaskDelay(500 / portTICK_PERIOD_MS);
                continue;
            }
            lastConnectionTime = currentTime;
        }

        // Если изменилось состояние подключения — синхронизируем флаг.
        if (currentConnected != wasConnected) {
            setConnectionState(currentConnected);
            wasConnected = currentConnected;
        }

        // Пауза между итерациями.
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
