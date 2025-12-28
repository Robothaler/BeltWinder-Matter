// config.h

#ifndef CONFIG_H
#define CONFIG_H

#pragma once

#define APP_VERSION "1.3.0"

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
