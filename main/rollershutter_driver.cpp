#include "rollershutter_driver.h"
#include "rollershutter.h"

static RollerShutter* shutter_instance = nullptr;

app_driver_handle_t shutter_driver_init() {
    if (shutter_instance) delete shutter_instance;
    shutter_instance = new RollerShutter();
    return (app_driver_handle_t)shutter_instance;
}

void shutter_driver_loop(app_driver_handle_t handle) {
    if (!handle) return;
    ((RollerShutter*)handle)->loop();
}

esp_err_t shutter_driver_go_to_lift_percent(app_driver_handle_t handle, uint8_t percent) {
    if (!handle) return ESP_FAIL;
    ((RollerShutter*)handle)->moveToPercent(percent);
    return ESP_OK;
}

esp_err_t shutter_driver_stop_motion(app_driver_handle_t handle) {
    if (!handle) return ESP_FAIL;
    ((RollerShutter*)handle)->stop();
    return ESP_OK;
}

esp_err_t shutter_driver_start_calibration(app_driver_handle_t handle) {
    if (!handle) return ESP_FAIL;
    ((RollerShutter*)handle)->startCalibration();
    return ESP_OK;
}

void shutter_driver_set_direction(app_driver_handle_t handle, bool inverted) {
    if (!handle) return;
    ((RollerShutter*)handle)->setDirectionInverted(inverted);
}

bool shutter_driver_get_direction_inverted(app_driver_handle_t handle) {
    if (!handle) return false;
    return ((RollerShutter*)handle)->isDirectionInverted();
}

bool shutter_driver_toggle_direction(app_driver_handle_t handle) {
    if (!handle) return false;
    bool current = ((RollerShutter*)handle)->isDirectionInverted();
    ((RollerShutter*)handle)->setDirectionInverted(!current);
    return !current;
}

uint8_t shutter_driver_get_current_percent(app_driver_handle_t handle) {
    if (!handle) return 0;
    return ((RollerShutter*)handle)->getCurrentPercent();
}

bool shutter_driver_is_position_changed(app_driver_handle_t handle) {
    if (!handle) return false;
    return ((RollerShutter*)handle)->hasPositionChanged();
}

bool shutter_driver_is_calibrated(app_driver_handle_t handle) {
    if (!handle) return false;
    return ((RollerShutter*)handle)->isCalibrated();
}

void shutter_driver_set_window_state(app_driver_handle_t handle, bool isOpen) {
    if (!handle) return;
    ((RollerShutter*)handle)->setWindowState(isOpen);
}

void shutter_driver_set_window_open_logic(app_driver_handle_t handle, WindowOpenLogic logic) {
    if (!handle) return;
    ((RollerShutter*)handle)->setWindowOpenLogic(logic);
}

RollerShutter::State shutter_driver_get_current_state(app_driver_handle_t handle) {
    if (!handle) return RollerShutter::State::STOPPED;
    return ((RollerShutter*)handle)->getCurrentState();
}
