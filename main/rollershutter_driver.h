#pragma once

#include <esp_err.h>
#include <cstdint>
#include "config.h"
#include "rollershutter.h"

typedef void *app_driver_handle_t;


/**
 * @brief Initializes the roller shutter driver and hardware.
 * @return Handle to the driver instance.
 */
app_driver_handle_t shutter_driver_init();

/**
 * @brief Main loop for the shutter driver, handles button presses and pulse counting.
 * Needs to be called repeatedly.
 * @param handle Handle to the driver instance.
 */
void shutter_driver_loop(app_driver_handle_t handle);

/**
 * @brief Commands the roller shutter to move to a specific lift percentage.
 * 0% is fully open (UP), 100% is fully closed (DOWN).
 * @param handle Handle to the driver instance.
 * @param percent Target percentage.
 * @return ESP_OK on success.
 */
esp_err_t shutter_driver_go_to_lift_percent(app_driver_handle_t handle, uint8_t percent);

/**
 * @brief Commands the roller shutter to stop its current movement.
 * @param handle Handle to the driver instance.
 * @return ESP_OK on success.
 */
esp_err_t shutter_driver_stop_motion(app_driver_handle_t handle);

/**
 * @brief Starts the calibration sequence.
 * @param handle Handle to the driver instance.
 * @return ESP_OK on success.
 */
esp_err_t shutter_driver_start_calibration(app_driver_handle_t handle);

/**
 * @brief Sets the motor and button direction logic.
 * @param handle Handle to the driver instance.
 * @param inverted True to invert direction, false for normal.
 */
void shutter_driver_set_direction(app_driver_handle_t handle, bool inverted);

/**
 * @brief Gets the current motor and button direction logic.
 * @param handle Handle to the driver instance.
 * @return True if direction is inverted, false otherwise.
 */
bool shutter_driver_get_direction_inverted(app_driver_handle_t handle);

/**
 * @brief Toggles the motor and button direction logic.
 * @param handle Handle to the driver instance.
 * @return The new state (true if inverted).
 */
bool shutter_driver_toggle_direction(app_driver_handle_t handle);

/**
 * @brief Gets the current position as a percentage.
 * @param handle Handle to the driver instance.
 * @return Current position (0-100).
 */
uint8_t shutter_driver_get_current_percent(app_driver_handle_t handle);

/**
 * @brief Checks if the position has changed since the last check.
 * @param handle Handle to the driver instance.
 * @return True if the position has changed.
 */
bool shutter_driver_is_position_changed(app_driver_handle_t handle);

/**
 * @brief Fetches whether the shutter is calibrated.
 * @param handle Handle to the driver instance.
 * @return True if the shutter is calibrated.
 */
bool shutter_driver_is_calibrated(app_driver_handle_t handle);

/**
 * @brief Sets the current window state (open/closed).
 * @param handle Handle to the driver instance.
 * @param isOpen True if the window is open, false if closed.
 */
void shutter_driver_set_window_state(app_driver_handle_t handle, bool isOpen);

/**
 * @brief Sets the logic for handling window open state during downward commands.
 * @param handle Handle to the driver instance.
 * @param logic The desired WindowOpenLogic setting.
 */
void shutter_driver_set_window_open_logic(app_driver_handle_t handle, WindowOpenLogic logic);

/**
 * @brief Gets the current state of the shutter.
 * @param handle Handle to the driver instance.
 * @return Current state enum value.
 */
RollerShutter::State shutter_driver_get_current_state(app_driver_handle_t handle);

