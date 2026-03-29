#pragma once

// -----------------------------------------------------------------------------
// connection.h
// Управление состоянием подключения по Bluetooth и FreeRTOS-задача мониторинга.
// -----------------------------------------------------------------------------

// connectionTask: периодически проверяет наличие BT-клиента (SerialBT.hasClient())
void connectionTask(void* parameter);

// setConnectionState: обновляет флаг isBluetoothConnected и реакции (LED, stop/flags).
void setConnectionState(bool connected);
