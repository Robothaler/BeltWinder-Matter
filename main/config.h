// config.h

#ifndef CONFIG_H
#define CONFIG_H

#pragma once

// #define CONFIG_ENABLE_SCENE_CLUSTER
// #define CONFIG_ENABLE_CUSTOM_CLUSTER_IP

#define DEVELOP_BUILD // ← Uncomment for develop builds (NVS WiFi only)

#ifndef APP_VERSION
#define APP_VERSION "v1.3.0"  // ← Manuell pflegen!
#endif

#ifndef BUILD_DATE
#define BUILD_DATE __DATE__  // ← Automatisch vom Compiler
#endif

#ifndef BUILD_TIME
#define BUILD_TIME __TIME__  // ← Automatisch vom Compiler
#endif

// --- Window Open Logic (legacy, kept for KVS compat) ---
enum class WindowOpenLogic {
    LOGIC_DISABLED,
    BLOCK_DOWNWARD,
    OPEN_FULLY,
    VENTILATION_POSITION
};

#define DEFAULT_WINDOW_LOGIC WindowOpenLogic::OPEN_FULLY
#define VENTILATION_PERCENTAGE 15

// --- Window State (derived from BLE sensor reed + rotation) ---
enum class WindowState : uint8_t {
    CLOSED,    // Reed contact closed — window shut
    PENDING,   // Reed just opened, waiting for angle to stabilise
    TILTED,    // Rotation within tilt range — window tilted (gekippt)
    OPEN       // Rotation within open range — window fully open
};

// --- Window Logic Configuration ---
struct WindowLogicConfig {
    bool     enabled      = false;  // Master enable/disable
    uint16_t reedDelayMs  = 3000;   // ms to wait after reed opens before classifying angle
    int16_t  tiltAngleMin = 5;      // °  lower bound for TILTED
    int16_t  tiltAngleMax = 45;     // °  upper bound for TILTED
    int16_t  openAngleMin = 46;     // °  lower bound for OPEN
    uint8_t  ventPosition = 15;     // %  ventilation target position
};

#define DEVICE_IP_MAX_LENGTH 16

#endif // CONFIG_H
