#pragma once

// -----------------------------------------------------------------------------
// bluetooth_transport.h
// Transport layer для обмена по Bluetooth:
// задача bluetoothTask читает входящие сообщения и периодически отправляет
// телеметрию, если активен стриминг.
// -----------------------------------------------------------------------------

// bluetoothTask: FreeRTOS-задача обмена по Bluetooth.
void bluetoothTask(void* parameter);
