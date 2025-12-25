#pragma once

// --- MQTT Broker Configuration (optional, not implemented) ---
#define MQTT_PORT           1883
#define MQTT_CLIENT_ID      "ESP32-RollerShutter"
#define MQTT_TOPIC_PREFIX   "rollershutter/livingroom"
#define MQTT_STATE_TOPIC    MQTT_TOPIC_PREFIX "/state"
#define MQTT_AVAIL_TOPIC    MQTT_TOPIC_PREFIX "/status"
#define MQTT_COMMAND_TOPIC  MQTT_TOPIC_PREFIX "/set"

// --- BLE Sensor Configuration (optional, currently not implemented) ---
#define BLE_SENSOR_ADDRESS         "11:22:33:44:55:66"
#define BLE_SERVICE_UUID           "0000180F-0000-1000-8000-00805F9B34FB"
#define BLE_CHARACTERISTIC_UUID    "00002A19-0000-1000-8000-00805F9B34FB"

// --- Window Open Logic ---
enum class WindowOpenLogic {
    LOGIC_DISABLED,
    BLOCK_DOWNWARD,
    OPEN_FULLY,
    VENTILATION_POSITION
};

#define DEFAULT_WINDOW_LOGIC WindowOpenLogic::BLOCK_DOWNWARD
#define VENTILATION_PERCENTAGE 15

#define DEVICE_IP_MAX_LENGTH 16
