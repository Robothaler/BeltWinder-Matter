// config.h

#ifndef CONFIG_H
#define CONFIG_H

#pragma once

// #define CONFIG_ENABLE_SCENE_CLUSTER

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

// --- Window Open Logic ---
enum class WindowOpenLogic {
    LOGIC_DISABLED,
    BLOCK_DOWNWARD,
    OPEN_FULLY,
    VENTILATION_POSITION
};

#define DEFAULT_WINDOW_LOGIC WindowOpenLogic::OPEN_FULLY
#define VENTILATION_PERCENTAGE 15

#define DEVICE_IP_MAX_LENGTH 16

#endif // CONFIG_H
