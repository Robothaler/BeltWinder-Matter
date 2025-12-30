// rollershutter_driver.h

#ifndef ROLLERSHUTTER_DRIVER_H
#define ROLLERSHUTTER_DRIVER_H

#pragma once

#include <esp_err.h>
#include <cstdint>
#include "config.h"
#include "rollershutter.h"

// ============================================================================
// Type Definitions
// ============================================================================

// Opaque handle to the shutter driver instance
typedef void* app_driver_handle_t;

// Forward Declaration
namespace chip {
namespace app {
namespace Clusters {
namespace WindowCovering {
class Delegate;
}
}
}
}

// Initialization
app_driver_handle_t shutter_driver_init();
void shutter_driver_loop(app_driver_handle_t handle);

// Movement Commands
esp_err_t shutter_driver_go_to_lift_percent(app_driver_handle_t handle, uint8_t percent);
esp_err_t shutter_driver_stop_motion(app_driver_handle_t handle);

// Calibration
esp_err_t shutter_driver_start_calibration(app_driver_handle_t handle);

// Configuration
void shutter_driver_set_direction(app_driver_handle_t handle, bool inverted);
bool shutter_driver_get_direction_inverted(app_driver_handle_t handle);
bool shutter_driver_toggle_direction(app_driver_handle_t handle);

// Status Queries
uint8_t shutter_driver_get_current_percent(app_driver_handle_t handle);
bool shutter_driver_is_position_changed(app_driver_handle_t handle);
bool shutter_driver_is_calibrated(app_driver_handle_t handle);
RollerShutter::State shutter_driver_get_current_state(app_driver_handle_t handle);

// Window Sensor
void shutter_driver_set_window_state(app_driver_handle_t handle, bool isOpen);
void shutter_driver_set_window_open_logic(app_driver_handle_t handle, WindowOpenLogic logic);

// Operational State Callback
typedef void (*operational_state_callback_t)(RollerShutter::State state);
void shutter_driver_set_operational_state_callback(app_driver_handle_t handle, 
                                                   operational_state_callback_t callback);

// Smart Update Strategy
bool shutter_driver_should_send_matter_update(app_driver_handle_t handle);
void shutter_driver_mark_matter_update_sent(app_driver_handle_t handle);

// ============================================================================
// Window Covering Delegate (Matter Integration)
// ============================================================================

chip::app::Clusters::WindowCovering::Delegate* shutter_driver_get_covering_delegate();
void shutter_driver_set_covering_delegate_endpoint(uint16_t endpoint_id);

#endif // ROLLERSHUTTER_DRIVER_H