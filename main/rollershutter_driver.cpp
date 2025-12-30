#include "rollershutter_driver.h"
#include "rollershutter.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/window-covering-server/window-covering-server.h>

static const char* TAG = "ShutterDriver";

static RollerShutter* shutter_instance = nullptr;
static operational_state_callback_t operational_state_callback = nullptr;

// ============================================================================
// Window Covering Delegate (Matter Command Handler)
// ============================================================================

class WindowCoveringDelegateImpl : public chip::app::Clusters::WindowCovering::Delegate {
public:
    WindowCoveringDelegateImpl() = default;
    ~WindowCoveringDelegateImpl() = default;
    
    void SetEndpoint(chip::EndpointId endpoint) {
        mEndpointId = endpoint;
    }
    
    /**
     * @brief Called when a movement command is received (UpOrOpen, DownOrClose, GoToPosition)
     * 
     * Das Matter SDK verarbeitet die Commands automatisch und setzt die Attribute.
     * Diese Methode wird als Notification aufgerufen.
     * 
     * @param type WindowCoveringType (Lift, Tilt, etc.)
     * @return CHIP_NO_ERROR on success
     */
    CHIP_ERROR HandleMovement(chip::app::Clusters::WindowCovering::WindowCoveringType type) override {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   Delegate: HandleMovement        ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "Movement Type: %d", (int)type);
        ESP_LOGI(TAG, "Endpoint: %d", mEndpointId);
        ESP_LOGI(TAG, "");
        
        // Für Rolladen: Keine besondere Aktion nötig
        // Die Position wird über Attribut-Updates gesteuert
        
        return CHIP_NO_ERROR;
    }
    
    /**
     * @brief Called when StopMotion command is received
     * 
     * @return CHIP_NO_ERROR on success
     */
    CHIP_ERROR HandleStopMotion() override {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   🛑 DELEGATE: StopMotion         ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "Endpoint: %d", mEndpointId);
        ESP_LOGI(TAG, "Timestamp: %llu ms", esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "");
        
        if (!shutter_instance) {
            ESP_LOGE(TAG, "✗ Shutter instance is NULL!");
            return CHIP_ERROR_INCORRECT_STATE;
        }
        
        // Aktuelle State loggen
        RollerShutter::State current_state = shutter_instance->getCurrentState();
        ESP_LOGI(TAG, "Current State before stop: %s", 
                current_state == RollerShutter::State::MOVING_UP ? "MOVING_UP" :
                current_state == RollerShutter::State::MOVING_DOWN ? "MOVING_DOWN" :
                current_state == RollerShutter::State::STOPPED ? "STOPPED" : "OTHER");
        
        shutter_instance->stop();
        
        ESP_LOGI(TAG, "✓ Stop command executed");
        ESP_LOGI(TAG, "");
        
        return CHIP_NO_ERROR;
    }

private:
    chip::EndpointId mEndpointId = chip::kInvalidEndpointId;
};

static WindowCoveringDelegateImpl coveringDelegate;

// ============================================================================
// Delegate Access Functions
// ============================================================================

chip::app::Clusters::WindowCovering::Delegate* shutter_driver_get_covering_delegate() {
    return &coveringDelegate;
}

void shutter_driver_set_covering_delegate_endpoint(uint16_t endpoint_id) {
    coveringDelegate.SetEndpoint(endpoint_id);
    ESP_LOGI(TAG, "✓ Covering Delegate configured for endpoint %d", endpoint_id);
}

// ============================================================================
// Driver Implementation (Rest bleibt wie gehabt)
// ============================================================================

app_driver_handle_t shutter_driver_init() {
    if (shutter_instance) delete shutter_instance;
    shutter_instance = new RollerShutter();
    return (app_driver_handle_t)shutter_instance;
}

void shutter_driver_set_operational_state_callback(app_driver_handle_t handle, 
                                                   operational_state_callback_t callback) {
    if (!handle) return;
    operational_state_callback = callback;
}

void shutter_driver_loop(app_driver_handle_t handle) {
    if (!handle) return;
    
    RollerShutter* shutter = (RollerShutter*)handle;
    
    static RollerShutter::State last_state = RollerShutter::State::STOPPED;
    RollerShutter::State current_state = shutter->getCurrentState();
    
    shutter->loop();
    
    if (current_state != last_state && operational_state_callback != nullptr) {
        operational_state_callback(current_state);
        last_state = current_state;
    }
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

bool shutter_driver_should_send_matter_update(app_driver_handle_t handle) {
    if (!handle) return false;
    return ((RollerShutter*)handle)->shouldSendMatterUpdate();
}

void shutter_driver_mark_matter_update_sent(app_driver_handle_t handle) {
    if (!handle) return;
    ((RollerShutter*)handle)->markMatterUpdateSent();
}
