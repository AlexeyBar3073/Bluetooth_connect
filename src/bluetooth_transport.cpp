#include <Arduino.h>
#include "bluetooth_transport.h"
#include "app_state.h"
#include "protocol_json.h"
#include "runtime.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// -----------------------------------------------------------------------------
// bluetooth_transport.cpp
// Транспортный слой обмена:
// - читает входящие сообщения от Android по BluetoothSerial
// - передаёт входящие строки в processIncomingJSON()
// - в режиме STREAMING_DATA периодически отправляет sendCarData()
// -----------------------------------------------------------------------------

// bluetoothTask:
// FreeRTOS-задача для работы с Bluetooth.
void bluetoothTask(void* parameter) {
    // parameter сейчас не используется.
    (void)parameter;

    while (1) {
        // Работаем только когда есть подключение к Android.
        if (getConnectionState()) {
            // Если в SerialBT есть данные — читаем очередную строку.
            if (SerialBT.available()) {
                // incoming: входящая JSON-строка (до '\n').
                String incoming = SerialBT.readStringUntil('\n');
                incoming.trim();
                if (incoming.length() > 0) {
                    // processIncomingJSON: парсит и обрабатывает settings/command, плюс ack.
                    processIncomingJSON(incoming);
                }
            }

            // Включаем периодическую отправку телеметрии только при стриминге.
            if (currentProgramState == STATE_STREAMING_DATA && isStreamingActive) {
                sendCarData();
                // LED "мигает" после отправки, чтобы визуально видеть активность.
                digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            }
        }

        // Небольшая пауза, чтобы не грузить CPU.
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}
