#include <Arduino.h>
#include <ArduinoOTA.h> 
#include <WiFi.h>
#include <Matter.h>
#include <Preferences.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_bt.h>
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <platform/PlatformManager.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_cluster.h>
#include <esp_matter_feature.h> 

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "config.h"
#include "credentials.h"
#include "rollershutter_driver.h"
#include "rollershutter.h"
#include "matter_cluster_defs.h"
#include "web_ui_handler.h"
#include "shelly_ble_manager.h"
#include "device_naming.h"
#include "wifi_manager.h"

using namespace chip;
using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::cluster;
using namespace esp_matter::command;
using namespace esp_matter::endpoint;

static const char* TAG = "Main";

const uint16_t INSTALLED_OPEN_LIMIT_LIFT_CM = 0;
const uint16_t INSTALLED_CLOSED_LIMIT_LIFT_CM = 200;

// ============================================================================
// State Variables
// ============================================================================

static app_driver_handle_t shutter_handle = nullptr;
static WebUIHandler* webUI = nullptr;
static ShellyBLEManager* bleManager = nullptr;
DeviceNaming* deviceNaming = nullptr;
static Preferences matterPref;

bool matter_node_created = false;  // extern zugänglich
bool matter_stack_started = false; // extern zugänglich

static char device_ip_str[DEVICE_IP_MAX_LENGTH] = "0.0.0.0";

static bool hardware_initialized = false;
static TaskHandle_t loop_task_handle = nullptr;

uint16_t window_covering_endpoint_id = 0;

// Contact Sensor
static endpoint_t* contact_sensor_endpoint = nullptr;
static uint16_t contact_sensor_endpoint_id = 0;
bool contact_sensor_endpoint_active = false;
bool contact_sensor_matter_enabled = false;

// Power Source Endpoint
static endpoint_t* power_source_endpoint = nullptr;
static uint16_t power_source_endpoint_id = 0;
bool power_source_endpoint_active = false;

// ============================================================================
// Forward Declarations
// ============================================================================

static esp_err_t app_attribute_update_cb(callback_type_t type, uint16_t endpoint_id, 
                                        uint32_t cluster_id, uint32_t attribute_id, 
                                        esp_matter_attr_val_t *val, void *priv_data);
static esp_err_t app_command_cb(const ConcreteCommandPath &command_path, 
                                TLV::TLVReader &tlv_reader, void *priv_data);

static endpoint_t* createContactSensorEndpoint(node_t* node);
static endpoint_t* createPowerSourceEndpoint(node_t* node); 
static void removeContactSensorEndpoint();
static void removePowerSourceEndpoint();
void enableContactSensorMatter();
void disableContactSensorMatter();
void performCompleteFactoryReset();

struct MatterStartResult;
extern MatterStartResult initializeAndStartMatter();

// ============================================================================
// Subscription Handlers (Vereinfacht - ohne private Methoden)
// ============================================================================

class SubscriptionHandler : public chip::app::ReadHandler::ApplicationCallback {
public:
    void OnSubscriptionEstablished(chip::app::ReadHandler& readHandler) override {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   ✓ SUBSCRIPTION ESTABLISHED     ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        // Sende initiales Update (verzögert)
        // Nicht sofort, sondern nach 100ms damit Session bereit ist
        subscriptionEstablishedTime = millis();
        needsInitialUpdate = true;
    }
    
    void OnSubscriptionTerminated(chip::app::ReadHandler& readHandler) override {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGW(TAG, "║   ⚠ SUBSCRIPTION TERMINATED      ║");
        ESP_LOGW(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGW(TAG, "Reason: Session closed or timeout");
        ESP_LOGW(TAG, "");
    }
    
    // Öffentliche Methode zum Senden des initialen Updates
    void sendInitialUpdateIfNeeded() {
        if (!needsInitialUpdate) {
            return;
        }
        
        // Warte 100ms nach Subscription
        if (millis() - subscriptionEstablishedTime < 100) {
            return;
        }
        
        needsInitialUpdate = false;
        
        if (!shutter_handle || window_covering_endpoint_id == 0) {
            ESP_LOGW(TAG, "⚠ Cannot send initial update - endpoints not ready");
            return;
        }
        
        ESP_LOGI(TAG, "→ Sending initial attribute updates...");
        
        // Current Position
        uint8_t percent = shutter_driver_get_current_percent(shutter_handle);
        uint16_t pos_100ths = percent * 100;
        
        esp_matter_attr_val_t pos_val = esp_matter_uint16(pos_100ths);
        attribute::report(window_covering_endpoint_id, 
                         chip::app::Clusters::WindowCovering::Id,
                         chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id, 
                         &pos_val);
        
        // Operational Status
        RollerShutter::State state = shutter_driver_get_current_state(shutter_handle);
        uint8_t opStateValue = 0x00;  // Default: Stopped
        
        if (state == RollerShutter::State::MOVING_UP || 
            state == RollerShutter::State::CALIBRATING_UP) {
            opStateValue = 0x05;  // MovingUpOrOpen
        } else if (state == RollerShutter::State::MOVING_DOWN || 
                   state == RollerShutter::State::CALIBRATING_DOWN) {
            opStateValue = 0x06;  // MovingDownOrClose
        }
        
        esp_matter_attr_val_t opstate_val = esp_matter_uint8(opStateValue);
        attribute::report(window_covering_endpoint_id, 
                         chip::app::Clusters::WindowCovering::Id,
                         chip::app::Clusters::WindowCovering::Attributes::OperationalStatus::Id, 
                         &opstate_val);
        
        ESP_LOGI(TAG, "✓ Initial update sent: Position=%d%%, OpState=0x%02X", 
                 percent, opStateValue);
        ESP_LOGI(TAG, "");
    }
    
private:
    uint32_t subscriptionEstablishedTime = 0;
    bool needsInitialUpdate = false;
};

static SubscriptionHandler subscriptionHandler;

// ============================================================================
// Operational State Callback (für Shutter → Matter Updates)
// ============================================================================

void onShutterStateChanged(RollerShutter::State state) {
    // ⚠️ NUR Matter updaten, wenn Stack läuft!
    if (!matter_stack_started || window_covering_endpoint_id == 0) {
        return;
    }
    
    uint8_t opStateValue;
    
    switch (state) {
        case RollerShutter::State::MOVING_UP:
        case RollerShutter::State::CALIBRATING_UP:
            opStateValue = 0x05;  // MovingUpOrOpen
            break;
            
        case RollerShutter::State::MOVING_DOWN:
        case RollerShutter::State::CALIBRATING_DOWN:
            opStateValue = 0x06;  // MovingDownOrClose
            break;
            
        case RollerShutter::State::STOPPED:
        case RollerShutter::State::CALIBRATING_VALIDATION:
        default:
            opStateValue = 0x00;  // Stopped
            break;
    }
    
    esp_matter_attr_val_t opstate_val = esp_matter_uint8(opStateValue);
    attribute::update(window_covering_endpoint_id, 
                     chip::app::Clusters::WindowCovering::Id,
                     chip::app::Clusters::WindowCovering::Attributes::OperationalStatus::Id, 
                     &opstate_val);
    
    ESP_LOGD(TAG, "Operational State updated: %d → Matter: 0x%02X", 
             (int)state, opStateValue);
}


// ============================================================================
// Last BLE sensor data — kept so the main loop can re-broadcast when window
// state changes asynchronously (e.g. PENDING → TILTED after the delay timer).
// ============================================================================
static String             lastBLEAddress;
static ShellyBLESensorData lastBLEData;
static bool               lastBLEDataValid = false;

// ============================================================================
// BLE Sensor Data Callback
// ============================================================================

void onBLESensorData(const String& address, const ShellyBLESensorData& data) {
    ESP_LOGI(TAG, "BLE Sensor: %s | Contact: %s | Battery: %d%% | Illuminance: %u lux | Rotation: %d° | RSSI: %d dBm",
             address.c_str(),
             data.windowOpen ? "OPEN" : "CLOSED",
             data.battery,
             data.illuminance,
             data.rotation,
             data.rssi);
    ESP_LOGI(TAG, "  lastUpdate: %u (millis: %u)", data.lastUpdate, millis());
    
    if (data.hasButtonEvent) {
        const char* eventName = (data.buttonEvent == BUTTON_SINGLE_PRESS) ? "SINGLE PRESS" : 
                               (data.buttonEvent == BUTTON_HOLD) ? "HOLD" : "UNKNOWN";
        ESP_LOGI(TAG, "  Button Event: %s", eventName);
    }

    // WebUI Update (IMMER aktiv)
    if (webUI) {
        ESP_LOGI(TAG, "→ Sending sensor data to WebUI...");
        webUI->broadcastSensorDataUpdate(address, data);
    }

        // ════════════════════════════════════════════════════════════════
    // Matter Update (nur wenn commissioned UND Matter läuft!)
    // ════════════════════════════════════════════════════════════════
    
    if (contact_sensor_matter_enabled && matter_stack_started) {
        bool is_commissioned = Matter.isDeviceCommissioned() && 
                              (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0);
        
        if (is_commissioned) {
            // ────────────────────────────────────────────────────────
            // Contact Sensor Attribute updaten
            // ────────────────────────────────────────────────────────
            
            if (contact_sensor_endpoint_active && contact_sensor_endpoint_id != 0) {
                ESP_LOGD(TAG, "→ Updating Contact Sensor attributes...");
                
                // Contact State
                esp_matter_attr_val_t contact_val = esp_matter_bool(!data.windowOpen);
                attribute::update(contact_sensor_endpoint_id,
                                 chip::app::Clusters::BooleanState::Id,
                                 chip::app::Clusters::BooleanState::Attributes::StateValue::Id,
                                 &contact_val);
            }
            
            // ────────────────────────────────────────────────────────
            // Power Source Attribute updaten
            // ────────────────────────────────────────────────────────
            
            if (power_source_endpoint_active && power_source_endpoint_id != 0) {
                ESP_LOGD(TAG, "→ Updating Power Source attributes...");
                
                // Battery Percentage (0-200, where 200 = 100%)
                esp_matter_attr_val_t battery_val = esp_matter_nullable_uint8(data.battery * 2);
                attribute::update(power_source_endpoint_id,
                                 chip::app::Clusters::PowerSource::Id,
                                 chip::app::Clusters::PowerSource::Attributes::BatPercentRemaining::Id,
                                 &battery_val);

                // Battery Charge Level
                chip::app::Clusters::PowerSource::BatChargeLevelEnum charge_level;
                if (data.battery < 10) {
                    charge_level = chip::app::Clusters::PowerSource::BatChargeLevelEnum::kCritical;
                } else if (data.battery < 20) {
                    charge_level = chip::app::Clusters::PowerSource::BatChargeLevelEnum::kWarning;
                } else {
                    charge_level = chip::app::Clusters::PowerSource::BatChargeLevelEnum::kOk;
                }
                
                esp_matter_attr_val_t charge_val = esp_matter_enum8((uint8_t)charge_level);
                attribute::update(power_source_endpoint_id,
                                 chip::app::Clusters::PowerSource::Id,
                                 chip::app::Clusters::PowerSource::Attributes::BatChargeLevel::Id,
                                 &charge_val);
                
                // Battery Replacement Needed
                esp_matter_attr_val_t replacement_val = esp_matter_bool(data.battery < 10);
                attribute::update(power_source_endpoint_id,
                                 chip::app::Clusters::PowerSource::Id,
                                 chip::app::Clusters::PowerSource::Attributes::BatReplacementNeeded::Id,
                                 &replacement_val);
                
                // Battery Voltage
                uint16_t voltage_mv = 2400 + (data.battery * 6);
                esp_matter_attr_val_t voltage_val = esp_matter_nullable_uint16(voltage_mv);
                attribute::update(power_source_endpoint_id,
                                 chip::app::Clusters::PowerSource::Id,
                                 chip::app::Clusters::PowerSource::Attributes::BatVoltage::Id,
                                 &voltage_val);
                
                ESP_LOGD(TAG, "✓ Power Source attributes updated (Battery: %d%%, Level: %s)",
                         data.battery,
                         charge_level == chip::app::Clusters::PowerSource::BatChargeLevelEnum::kCritical ? "CRITICAL" :
                         charge_level == chip::app::Clusters::PowerSource::BatChargeLevelEnum::kWarning ? "WARNING" : "OK");
            }
        }
    }
    
    // ════════════════════════════════════════════════════════════════
    // Rolladen-Logik (IMMER aktiv, unabhängig von Matter!)
    // ════════════════════════════════════════════════════════════════
    
    shutter_driver_set_window_sensor_data(shutter_handle, data.windowOpen, data.rotation);

    // Cache for async re-broadcast when window state changes via loop() timer
    lastBLEAddress   = address;
    lastBLEData      = data;
    lastBLEDataValid = true;
}


    // ============================================================================
    // Contact Sensor Endpoint Management
    // ============================================================================

    static endpoint_t* createContactSensorEndpoint(node_t* node) {
    if (contact_sensor_endpoint != nullptr) {
        ESP_LOGW(TAG, "Contact sensor endpoint already exists");
        return contact_sensor_endpoint;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   CREATING CONTACT SENSOR         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    
    // ========================================================================
    // Contact Sensor Endpoint
    // ========================================================================
    
    contact_sensor::config_t cs_config;
    contact_sensor_endpoint = contact_sensor::create(node, &cs_config, 
                                                     ENDPOINT_FLAG_NONE, NULL);
    
    if (!contact_sensor_endpoint) {
        ESP_LOGE(TAG, "✗ Failed to create Contact Sensor endpoint");
        return nullptr;
    }
    
    contact_sensor_endpoint_id = endpoint::get_id(contact_sensor_endpoint);
    ESP_LOGI(TAG, "✓ Contact Sensor endpoint created (ID: %d)", 
             contact_sensor_endpoint_id);
    
    // ========================================================================
    // Boolean State (Contact)
    // ========================================================================
    
    cluster_t* boolean_cluster = cluster::get(contact_sensor_endpoint, 
                                             chip::app::Clusters::BooleanState::Id);
    if (boolean_cluster) {
        esp_matter_attr_val_t contact_val = esp_matter_bool(false);
        attribute::update(contact_sensor_endpoint_id, 
                         chip::app::Clusters::BooleanState::Id,
                         chip::app::Clusters::BooleanState::Attributes::StateValue::Id, 
                         &contact_val);
        ESP_LOGI(TAG, "✓ Boolean State cluster configured");
    }
    
    // ========================================================================
    // Fixed Label Cluster (um Gerät als "Fenster" zu markieren)
    // ========================================================================

    cluster::fixed_label::config_t label_config;
    cluster_t* label_cluster = cluster::fixed_label::create(contact_sensor_endpoint, 
                                                            &label_config, 
                                                            CLUSTER_FLAG_SERVER);

    if (label_cluster) {
        // Label: "type" = "window"
        // Das wird von HomeKit ausgewertet!
        ESP_LOGI(TAG, "✓ Fixed Label cluster added (type: window)");
    }
    
    // ========================================================================
    // Endpoint Status speichern
    // ========================================================================
    
    contact_sensor_endpoint_active = true;
    ESP_LOGI(TAG, "✓ Contact Sensor endpoint fully configured");
    ESP_LOGI(TAG, "");
    
    return contact_sensor_endpoint;
}

    // ============================================================================
    // Power Source Endpoint (Matter 1.2 Spec Section 11.7)
    // ============================================================================

    static endpoint_t* createPowerSourceEndpoint(node_t* node) {
    if (power_source_endpoint != nullptr) {
        ESP_LOGW(TAG, "Power Source endpoint already exists");
        return power_source_endpoint;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   CREATING POWER SOURCE ENDPOINT  ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    
    uint8_t flags = ENDPOINT_FLAG_DESTROYABLE;
    power_source_endpoint = endpoint::create(node, flags, nullptr);
    
    if (!power_source_endpoint) {
        ESP_LOGE(TAG, "✗ Failed to create generic endpoint");
        return nullptr;
    }
    
    power_source_endpoint_id = endpoint::get_id(power_source_endpoint);
    ESP_LOGI(TAG, "✓ Generic endpoint created (ID: %d)", power_source_endpoint_id);
    
    endpoint::add_device_type(power_source_endpoint, 0x0011, 1);
    ESP_LOGI(TAG, "✓ Device Type set to Power Source (0x0011)");
    
    descriptor::config_t descriptor_config;
    descriptor::create(power_source_endpoint, &descriptor_config, CLUSTER_FLAG_SERVER);
    ESP_LOGI(TAG, "✓ Descriptor cluster added");
    
    cluster_t* ps_cluster = cluster::create(power_source_endpoint,
                                            chip::app::Clusters::PowerSource::Id,
                                            CLUSTER_FLAG_SERVER);
    
    if (!ps_cluster) {
        ESP_LOGE(TAG, "✗ Failed to create Power Source cluster");
        power_source_endpoint = nullptr;
        return nullptr;
    }
    
    ESP_LOGI(TAG, "✓ Power Source cluster created");
    
    // ========================================================================
    // Mandatory Attributes mit FESTEN Buffer-Größen
    // ========================================================================
    
    // Status
    attribute::create(ps_cluster,
                     chip::app::Clusters::PowerSource::Attributes::Status::Id,
                     ATTRIBUTE_FLAG_NONE,
                     esp_matter_enum8((uint8_t)chip::app::Clusters::PowerSource::PowerSourceStatusEnum::kActive));
    
    // Order
    attribute::create(ps_cluster,
                     chip::app::Clusters::PowerSource::Attributes::Order::Id,
                     ATTRIBUTE_FLAG_NONE,
                     esp_matter_uint8(0));
    
    static char ps_description[16] = "Battery";

    attribute::create(ps_cluster,
                    chip::app::Clusters::PowerSource::Attributes::Description::Id,
                    ATTRIBUTE_FLAG_NONE,
                    esp_matter_char_str(ps_description, sizeof(ps_description)));

    ESP_LOGI(TAG, "✓ Description attribute configured");
    
    // FeatureMap
    attribute::create(ps_cluster,
                     chip::app::Clusters::Globals::Attributes::FeatureMap::Id,
                     ATTRIBUTE_FLAG_NONE,
                     esp_matter_uint32(0x02));
    
    ESP_LOGI(TAG, "✓ Mandatory attributes configured");
    
    // Battery Attributes
    attribute::create(ps_cluster,
                     chip::app::Clusters::PowerSource::Attributes::BatPercentRemaining::Id,
                     ATTRIBUTE_FLAG_NULLABLE,
                     esp_matter_nullable_uint8(0));
    ESP_LOGI(TAG, "✓ Attribute 0x000C (BatPercentRemaining) configured");
    
    attribute::create(ps_cluster,
                     chip::app::Clusters::PowerSource::Attributes::BatChargeLevel::Id,
                     ATTRIBUTE_FLAG_NONE,
                     esp_matter_enum8((uint8_t)chip::app::Clusters::PowerSource::BatChargeLevelEnum::kOk));
    ESP_LOGI(TAG, "✓ Attribute 0x000D (BatChargeLevel) configured");
    
    attribute::create(ps_cluster,
                     chip::app::Clusters::PowerSource::Attributes::BatReplacementNeeded::Id,
                     ATTRIBUTE_FLAG_NONE,
                     esp_matter_bool(false));
    ESP_LOGI(TAG, "✓ Attribute 0x000F (BatReplacementNeeded) configured");
    
    attribute::create(ps_cluster,
                     chip::app::Clusters::PowerSource::Attributes::BatVoltage::Id,
                     ATTRIBUTE_FLAG_NULLABLE,
                     esp_matter_nullable_uint16(3000));
    ESP_LOGI(TAG, "✓ Attribute BatVoltage configured");
    
    power_source_endpoint_active = true;
    ESP_LOGI(TAG, "✓ Power Source endpoint fully configured");
    ESP_LOGI(TAG, "");
    
    return power_source_endpoint;
}
    static void removeContactSensorEndpoint() {
    if (contact_sensor_endpoint == nullptr) {
        ESP_LOGW(TAG, "No contact sensor endpoint to remove");
        return;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   DEACTIVATING CONTACT SENSOR     ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    
    // ⚠️ Matter unterstützt KEIN echtes Löschen von Endpoints!
    // Wir markieren den Endpoint nur als "inaktiv"
    
    contact_sensor_endpoint_active = false;
    
    // Status aus Preferences löschen
    matterPref.begin("matter", false);
    matterPref.putBool("cs_active", false);
    matterPref.end();
    
    ESP_LOGI(TAG, "✓ Contact Sensor endpoint marked as inactive");
    ESP_LOGI(TAG, "⚠️  Note: Endpoint remains in Matter structure!");
    ESP_LOGI(TAG, "⚠️  To fully remove: Factory Reset required");
    ESP_LOGI(TAG, "");
    
    // Hinweis: contact_sensor_endpoint bleibt gültig!
    // Wir setzen nur das Flag zurück, damit keine Updates mehr gesendet werden
}

    static void removePowerSourceEndpoint() {
        if (power_source_endpoint == nullptr) {
            ESP_LOGW(TAG, "No Power Source endpoint to remove");
            return;
        }
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   DEACTIVATING POWER SOURCE       ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        
        power_source_endpoint_active = false;
        
        matterPref.begin("matter", false);
        matterPref.putBool("ps_active", false);
        matterPref.end();
        
        ESP_LOGI(TAG, "✓ Power Source endpoint marked as inactive");
        ESP_LOGI(TAG, "⚠️  Note: Endpoint remains in Matter structure!");
        ESP_LOGI(TAG, "⚠️  To fully remove: Factory Reset required");
        ESP_LOGI(TAG, "");
    }


void enableContactSensorMatter() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   ENABLING MATTER CONTACT SENSOR  ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    if (!bleManager || !bleManager->isPaired()) {
        ESP_LOGW(TAG, "⚠ No BLE sensor paired - endpoints will be created on first data");
    }
    
    if (!matter_stack_started) {
        ESP_LOGW(TAG, "⚠ Matter not started yet - endpoints will be created after Matter starts");
    }
    
    contact_sensor_matter_enabled = true;
    
    // State speichern
    matterPref.begin("matter", false);
    matterPref.putBool("cs_matter_en", true);
    matterPref.end();
    
    // ⚠️ Endpoints NUR erstellen, wenn Matter läuft!
    if (matter_stack_started && Matter.isDeviceCommissioned() && bleManager && bleManager->isPaired()) {
        node_t* node = node::get();
        if (node) {
            // Contact Sensor Endpoint
            if (!contact_sensor_endpoint_active) {
                createContactSensorEndpoint(node);
            }
            
            // Power Source Endpoint
            if (!power_source_endpoint_active) {
                createPowerSourceEndpoint(node);
            }
            
            // Wenn bereits Daten vorhanden, sofort updaten
            ShellyBLESensorData data;
            if (bleManager->getSensorData(data) && data.dataValid) {
                ESP_LOGI(TAG, "→ Updating with existing sensor data...");
                onBLESensorData(bleManager->getPairedDevice().address, data);
            }
        }
    }
    
    ESP_LOGI(TAG, "✓ Matter Contact Sensor enabled");
    ESP_LOGI(TAG, "");
}

void disableContactSensorMatter() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  DISABLING MATTER CONTACT SENSOR  ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    contact_sensor_matter_enabled = false;
    
    // State speichern
    matterPref.begin("matter", false);
    matterPref.putBool("cs_matter_en", false);
    matterPref.end();
    
    // Contact Sensor Endpoint deaktivieren
    if (contact_sensor_endpoint_active) {
        removeContactSensorEndpoint();
        ESP_LOGI(TAG, "✓ Contact Sensor endpoint deactivated");
    }
    
    // Power Source Endpoint deaktivieren
    if (power_source_endpoint_active) {
        removePowerSourceEndpoint();
        ESP_LOGI(TAG, "✓ Power Source endpoint deactivated");
    }
    
    ESP_LOGI(TAG, "✓ Matter Contact Sensor disabled");
    ESP_LOGI(TAG, "ℹ Sensor data will still be used for shutter logic");
    ESP_LOGI(TAG, "");
}

    // ════════════════════════════════════════════════════════════════════
    // Calibration Complete Callback
    // ════════════════════════════════════════════════════════════════════

    void onCalibrationComplete(bool success) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   CALIBRATION %s", success ? "COMPLETE ✓    " : "FAILED ✗      ");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        if (webUI) {
            char msg[256];
            if (success) {
                snprintf(msg, sizeof(msg),
                        "{\"type\":\"calibration_complete\","
                        "\"success\":true,"
                        "\"message\":\"Calibration successful! Shutter is now calibrated.\"}");
            } else {
                snprintf(msg, sizeof(msg),
                        "{\"type\":\"calibration_complete\","
                        "\"success\":false,"
                        "\"message\":\"Calibration failed! Please try again.\"}");
            }
        
            for (int i = 0; i < 3; i++) {
                webUI->broadcast_to_all_clients(msg);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            
            ESP_LOGI(TAG, "✓ Sent calibration result to WebUI (3x for reliability)");
        }
        
        ESP_LOGI(TAG, "");
    }


// ════════════════════════════════════════════════════════════════════════
// Global State für verzögerte Matter Initialization
// ════════════════════════════════════════════════════════════════════════

static node_t* matter_node = nullptr;

// ════════════════════════════════════════════════════════════════════════
// STEP 1: Create Matter Node & Endpoints
// ════════════════════════════════════════════════════════════════════════

bool createMatterNode() {
    if (matter_node_created) {
        ESP_LOGW(TAG, "Matter node already created");
        return true;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "║         CREATING MATTER NODE & ENDPOINTS                  ║");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Matter Node erstellen
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Creating Matter node...");
    
    node::config_t node_config;
    matter_node = node::create(&node_config, app_attribute_update_cb, nullptr);
    
    if (!matter_node) {
        ESP_LOGE(TAG, "✗ Failed to create Matter node");
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Matter node created");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Window Covering Endpoint
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Creating Window Covering endpoint...");
    
    window_covering_device::config_t wc_config;
    wc_config.window_covering.type = 0;  // Rollershutter
    wc_config.window_covering.feature_flags = 
        (uint32_t)chip::app::Clusters::WindowCovering::Feature::kLift |
        (uint32_t)chip::app::Clusters::WindowCovering::Feature::kPositionAwareLift |
        (uint32_t)chip::app::Clusters::WindowCovering::Feature::kAbsolutePosition;
    
    endpoint_t *ep = window_covering_device::create(matter_node, &wc_config, 
                                                     ENDPOINT_FLAG_NONE, NULL);
    
    if (!ep) {
        ESP_LOGE(TAG, "✗ Failed to create Window Covering endpoint");
        return false;
    }
    
    window_covering_endpoint_id = endpoint::get_id(ep);
    
    ESP_LOGI(TAG, "✓ Window Covering endpoint created (ID: %d)", window_covering_endpoint_id);
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Mode Attribute konfigurieren
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Configuring Mode attribute...");
    
    cluster_t* wc_cluster = cluster::get(ep, chip::app::Clusters::WindowCovering::Id);
    
    if (wc_cluster) {
        bool inverted = shutter_driver_get_direction_inverted(shutter_handle);
        uint8_t mode_value = inverted ? 0x01 : 0x00;
        
        esp_matter_attr_val_t mode_val = esp_matter_bitmap8(mode_value);
        attribute_t* mode_attr = attribute::create(wc_cluster, 
                                    chip::app::Clusters::WindowCovering::Attributes::Mode::Id,
                                    ATTRIBUTE_FLAG_WRITABLE,
                                    mode_val);
        
        if (mode_attr) {
            ESP_LOGI(TAG, "✓ Mode attribute configured: 0x%02X (inverted=%s)", 
                    mode_value, inverted ? "YES" : "NO");
        }
    }
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Position aus KVS wiederherstellen
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Restoring saved position...");
    
    if (wc_cluster) {
        uint8_t saved_percent = shutter_driver_get_current_percent(shutter_handle);
        bool is_calibrated = shutter_driver_is_calibrated(shutter_handle);
        
        if (is_calibrated) {
            uint16_t pos_100ths = saved_percent * 100;
            
            // Current Position
            attribute_t* current_attr = attribute::get(wc_cluster, 
                chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id);
            
            if (current_attr) {
                esp_matter_attr_val_t current_val = esp_matter_uint16(pos_100ths);
                attribute::set_val(current_attr, &current_val);
                ESP_LOGI(TAG, "✓ Current Position restored: %d%%", saved_percent);
            }
            
            // Target Position
            attribute_t* target_attr = attribute::get(wc_cluster, 
                chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id);
            
            if (target_attr) {
                esp_matter_attr_val_t target_val = esp_matter_uint16(pos_100ths);
                attribute::set_val(target_attr, &target_val);
                ESP_LOGI(TAG, "✓ Target Position restored: %d%%", saved_percent);
            }
            
            // Operational Status (Stopped)
            attribute_t* opstate_attr = attribute::get(wc_cluster, 
                chip::app::Clusters::WindowCovering::Attributes::OperationalStatus::Id);
            
            if (opstate_attr) {
                esp_matter_attr_val_t opstate_val = esp_matter_uint8(0x00);
                attribute::set_val(opstate_attr, &opstate_val);
                ESP_LOGI(TAG, "✓ Operational Status: Stopped");
            }
            
        } else {
            ESP_LOGW(TAG, "⚠ Shutter not calibrated - using default position (0%)");
        }
    }
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Covering Delegate registrieren
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Registering Covering Delegate...");
    
    shutter_driver_set_covering_delegate_endpoint(window_covering_endpoint_id);
    
    chip::app::Clusters::WindowCovering::Delegate* delegate = 
        shutter_driver_get_covering_delegate();
    
    chip::app::Clusters::WindowCovering::SetDefaultDelegate(
        (chip::EndpointId)window_covering_endpoint_id, 
        delegate
    );
    
    ESP_LOGI(TAG, "✓ Covering Delegate registered");
    ESP_LOGI(TAG, "");
    
    // Endpoint aktivieren
    esp_matter::endpoint::enable(ep);
    
    // ════════════════════════════════════════════════════════════════════
    // Optional: Custom Cluster (Device IP)
    // ════════════════════════════════════════════════════════════════════
    
    #ifdef CONFIG_ENABLE_CUSTOM_CLUSTER_DEVICE_IP
    
    ESP_LOGI(TAG, "→ Creating Custom Cluster (Device IP)...");
    
    cluster_t *custom_cluster = cluster::create(ep, CLUSTER_ID_ROLLERSHUTTER_CONFIG, 
                                                CLUSTER_FLAG_SERVER);
    if (custom_cluster) {
        esp_matter_attr_val_t ip_val = esp_matter_char_str(device_ip_str, DEVICE_IP_MAX_LENGTH);
        
        attribute_t* ip_attr = attribute::create(
            custom_cluster, 
            ATTR_ID_DEVICE_IP, 
            ATTRIBUTE_FLAG_NONE,
            ip_val
        );
        
        command::create(custom_cluster, CMD_ID_START_CALIBRATION, 
                    COMMAND_FLAG_ACCEPTED, app_command_cb);
        
        ESP_LOGI(TAG, "✓ Custom cluster created (0x%04X)", CLUSTER_ID_ROLLERSHUTTER_CONFIG);
    }
    
    #endif
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Optional: Scene Cluster
    // ════════════════════════════════════════════════════════════════════
    
    #ifdef CONFIG_ENABLE_SCENE_CLUSTER
    
    ESP_LOGI(TAG, "→ Creating Scene Cluster...");
    
    cluster_t* scene_cluster = cluster::create(ep, CLUSTER_ID_SCENES, 
                                            CLUSTER_FLAG_SERVER);
    if (scene_cluster) {
        attribute::create(scene_cluster, ATTR_ID_SCENE_COUNT, 
                        ATTRIBUTE_FLAG_NONE, 
                        esp_matter_uint8(SCENE_MAPPING_COUNT));
        
        attribute::create(scene_cluster, ATTR_ID_CURRENT_SCENE, 
                        ATTRIBUTE_FLAG_NONE, 
                        esp_matter_uint8(0));
        
        attribute::create(scene_cluster, ATTR_ID_CURRENT_GROUP, 
                        ATTRIBUTE_FLAG_NONE, 
                        esp_matter_uint16(0));
        
        attribute::create(scene_cluster, ATTR_ID_SCENE_VALID, 
                        ATTRIBUTE_FLAG_NONE, 
                        esp_matter_bool(false));
        
        attribute::create(scene_cluster, ATTR_ID_NAME_SUPPORT, 
                        ATTRIBUTE_FLAG_NONE, 
                        esp_matter_bitmap8(0x80));
        
        command::create(scene_cluster, CMD_ID_RECALL_SCENE, 
                    COMMAND_FLAG_ACCEPTED, app_command_cb);
        
        command::create(scene_cluster, CMD_ID_GET_SCENE_MEMBERSHIP, 
                    COMMAND_FLAG_ACCEPTED, app_command_cb);
        
        ESP_LOGI(TAG, "✓ Scene Cluster created (%d scenes)", SCENE_MAPPING_COUNT);
    }
    
    #endif
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Contact Sensor & Power Source (falls aktiviert)
    // ════════════════════════════════════════════════════════════════════
    
    matterPref.begin("matter", true);
    // Default true: Contact Sensor and Power Source must be present at commissioning
    // time so that Apple Home (and other controllers) discover them on first pairing.
    // Dynamic post-commissioning endpoint addition is not reliably picked up.
    contact_sensor_matter_enabled = matterPref.getBool("cs_matter_en", true);
    matterPref.end();

    if (contact_sensor_matter_enabled) {
        ESP_LOGI(TAG, "→ Creating Contact Sensor + Power Source endpoints...");
        createContactSensorEndpoint(matter_node);
        createPowerSourceEndpoint(matter_node);
    }
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Node Creation Complete
    // ════════════════════════════════════════════════════════════════════
    
    matter_node_created = true;
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "║         ✓ MATTER NODE CREATION COMPLETE                  ║");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    return true;
}

// ════════════════════════════════════════════════════════════════════════
// STEP 2: Start Matter Stack
// ════════════════════════════════════════════════════════════════════════

bool startMatterStack() {
    if (!matter_node_created) {
        ESP_LOGE(TAG, "✗ Matter node not created yet!");
        ESP_LOGE(TAG, "  Call createMatterNode() first");
        return false;
    }
    
    if (matter_stack_started) {
                ESP_LOGW(TAG, "Matter stack already started");
        return true;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "║            STARTING MATTER STACK (ON-DEMAND)             ║");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Matter Stack starten
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Starting Matter stack...");
    
    esp_err_t err = esp_matter::start(nullptr);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "✗ Failed to start Matter stack: %s", esp_err_to_name(err));
        return false;
    }
    
    matter_stack_started = true;

    ESP_LOGI(TAG, "✓ Matter stack started successfully");
    ESP_LOGI(TAG, "");

    // Commissioning window is opened on-demand via the WebUI button
    // ("matter_open_commissioning" command). Opening it here would call
    // into the CHIP server before Server::Init() completes asynchronously,
    // which causes a crash or silent failure. The button press always
    // happens well after the CHIP task has fully initialized.
    if (Matter.isDeviceCommissioned()) {
        ESP_LOGI(TAG, "✓ Device already commissioned — skipping commissioning window");
    } else {
        ESP_LOGI(TAG, "→ Not commissioned — use WebUI button to open commissioning window");
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Subscription Handler aktivieren
    // ════════════════════════════════════════════════════════════════════
    
    chip::app::InteractionModelEngine* imEngine = chip::app::InteractionModelEngine::GetInstance();
    if (imEngine) {
        ESP_LOGI(TAG, "✓ Subscription monitoring active");
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Device Naming anwenden
    // ════════════════════════════════════════════════════════════════════
    
    if (deviceNaming) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        deviceNaming->apply();
        
        DeviceNaming::DeviceName names = deviceNaming->getNames();
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Device Identification:");
        ESP_LOGI(TAG, "  Network Hostname: %s.local", names.hostname.c_str());
        ESP_LOGI(TAG, "  Matter Name: %s", names.matterName.c_str());
    }
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Hardware initialisieren (falls noch nicht)
    // ════════════════════════════════════════════════════════════════════
    
    if (!hardware_initialized) {
        ESP_LOGI(TAG, "→ Initializing hardware...");
        ((RollerShutter*)shutter_handle)->initHardware();
        hardware_initialized = true;
        ESP_LOGI(TAG, "✓ Hardware initialized");
    }
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Commissioning Status prüfen
    // ════════════════════════════════════════════════════════════════════
    
    bool commissioned = Matter.isDeviceCommissioned();
    uint8_t fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
    
    ESP_LOGI(TAG, "Matter Status:");
    ESP_LOGI(TAG, "  Commissioned: %s", commissioned ? "YES" : "NO");
    ESP_LOGI(TAG, "  Fabrics: %d", fabric_count);
    ESP_LOGI(TAG, "");
    
    if (!commissioned || fabric_count == 0) {
        // ════════════════════════════════════════════════════════════════
        // Pairing Information generieren
        // ════════════════════════════════════════════════════════════════
        
        String qrUrl = Matter.getOnboardingQRCodeUrl();
        String pairingCode = Matter.getManualPairingCode();
        
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "   📱  MATTER COMMISSIONING READY");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "QR Code URL:");
        ESP_LOGI(TAG, "  %s", qrUrl.c_str());
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Manual Pairing Code:");
        ESP_LOGI(TAG, "  %s", pairingCode.c_str());
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Steps to commission:");
        ESP_LOGI(TAG, "  1. Open your Matter controller app (Home Assistant, Apple Home, etc.)");
        ESP_LOGI(TAG, "  2. Select 'Add Device' or 'Add Matter Device'");
        ESP_LOGI(TAG, "  3. Scan the QR code shown in the WebUI");
        ESP_LOGI(TAG, "  4. Follow the on-screen instructions");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "");
        
    } else {
        ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
        ESP_LOGI(TAG, "║                                                           ║");
        ESP_LOGI(TAG, "║           ✓ ALREADY COMMISSIONED                         ║");
        ESP_LOGI(TAG, "║                                                           ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Device is operational with %d fabric(s)", fabric_count);
        ESP_LOGI(TAG, "");
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Complete
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "║         ✓ MATTER STACK FULLY OPERATIONAL                 ║");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    return true;
}


MatterStartResult initializeAndStartMatter() {
    MatterStartResult result;
    result.success = false;
    result.already_commissioned = false;
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "║         USER REQUESTED MATTER INITIALIZATION             ║");
    ESP_LOGI(TAG, "║              (via WebUI Button Click)                     ║");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Step 1: Stop BLE scanning so we don't hold resources across reboot
    // ════════════════════════════════════════════════════════════════════

    if (bleManager && bleManager->isScanActive()) {
        ESP_LOGI(TAG, "→ Stopping BLE scan before reboot...");
        // Use manualStop=false to preserve NVS continuous_scan flag.
        // This is an automatic stop for reboot preparation, not a user request.
        // On next boot, continuous scan will auto-start if it was enabled.
        bleManager->stopScan(false);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // ════════════════════════════════════════════════════════════════════
    // Step 2: SET NVS FLAG — Matter will auto-start on next boot
    //
    // We do NOT call createMatterNode() or startMatterStack() here.
    // Calling them while NimBLE owns the BT controller causes:
    //   BLEManagerImpl::InitESPBleLayer() → nimble_port_init() → FAIL
    //   (NimBLEDevice ignores the error, spawns second host task → deadlock)
    //
    // Instead: set flag, reboot. The boot sequence (Phase 2 then Phase 8)
    // is already designed to handle this cleanly.
    // ════════════════════════════════════════════════════════════════════

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ENABLING MATTER PERSISTENCE      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    matterPref.begin("matter", false);
    matterPref.putBool("matter_enabled", true);
    matterPref.end();

    ESP_LOGI(TAG, "✓ NVS Flag 'matter_enabled' set to TRUE");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "After reboot:");
    ESP_LOGI(TAG, "  ✓ Matter will auto-start (Phase 8)");
    ESP_LOGI(TAG, "  ✓ BLE sensor scanning ready (Phase 2)");
    ESP_LOGI(TAG, "  ✓ WebUI remains accessible");
    ESP_LOGI(TAG, "");

    result.success = true;
    result.reboot_triggered = true;

    return result;  // WebUI handler calls ESP.restart() after sending response
}


// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);

    esp_log_level_set("*", ESP_LOG_INFO);               // Global: INFO

    esp_log_level_set("chip[DL]", ESP_LOG_WARN);        // Matter: WARN
    esp_log_level_set("chip[DMG]", ESP_LOG_ERROR);  
    esp_log_level_set("chip[SC]", ESP_LOG_ERROR);
    esp_log_level_set("esp_matter_attribute", ESP_LOG_ERROR);
    esp_log_level_set("esp_matter_command", ESP_LOG_INFO);
    esp_log_level_set("esp_matter_cluster", ESP_LOG_WARN);
    esp_log_level_set("WiFiMgr", ESP_LOG_DEBUG);        // WiFi Manager: DEBUG
    esp_log_level_set("BLEAutoStart", ESP_LOG_NONE);    // BLE: INFO
    esp_log_level_set("ShellyBLE", ESP_LOG_NONE);       // BLE: INFO
    esp_log_level_set("BLESimple", ESP_LOG_NONE);       // BLE: INFO
    esp_log_level_set("NimBLE", ESP_LOG_NONE);          // NimBLE: INFO
    esp_log_level_set("wifi", ESP_LOG_NONE);            // WiFi: ERROR
    esp_log_level_set("Shutter", ESP_LOG_DEBUG);        // Shutter: DEBUG
    esp_log_level_set("ShutterDriver", ESP_LOG_DEBUG);  // ShutterDriver: DEBUG
    esp_log_level_set("Main", ESP_LOG_INFO);            // Main: INFO
    esp_log_level_set("WebUI", ESP_LOG_DEBUG);           // WebUI: DEBUG


    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                  BELTWINDER - STARTUP                    ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // PHASE 0: Check Matter Enable Flag
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PHASE 0: Matter Boot Decision");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    
    bool matter_enabled = false;
    
    matterPref.begin("matter", true);  // Read-only
    matter_enabled = matterPref.getBool("matter_enabled", false);
    matterPref.end();
    
    ESP_LOGI(TAG, "NVS Flag 'matter_enabled': %s", matter_enabled ? "TRUE" : "FALSE");
    ESP_LOGI(TAG, "");
    
    if (matter_enabled) {
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  MATTER AUTO-START MODE           ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Matter will be initialized during boot");
    } else {
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  BLE-ONLY MODE (First Boot)      ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Matter will NOT be started automatically");
        ESP_LOGI(TAG, "User can enable via WebUI → 'Start Matter Commissioning'");
    }
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // PHASE 1: GPIO & Hardware Basics
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PHASE 1: Hardware Initialization");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    
    pinMode(CONFIG_PULSE_COUNTER_PIN, INPUT_PULLUP);
    pinMode(CONFIG_MOTOR_UP_PIN, INPUT_PULLUP);
    pinMode(CONFIG_MOTOR_DOWN_PIN, INPUT_PULLUP);
    pinMode(CONFIG_BUTTON_UP_PIN, OUTPUT);
    digitalWrite(CONFIG_BUTTON_UP_PIN, HIGH);
    pinMode(CONFIG_BUTTON_DOWN_PIN, OUTPUT);
    digitalWrite(CONFIG_BUTTON_DOWN_PIN, HIGH);
    
    ESP_LOGI(TAG, "✓ GPIOs configured");
    ESP_LOGI(TAG, "");
    
    // Watchdog
    loop_task_handle = xTaskGetCurrentTaskHandle();
    esp_task_wdt_add(loop_task_handle);

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 2: NimBLE Initialization (ALWAYS FIRST)
    //
    // CRITICAL ARCHITECTURE RULE (ESP-IDF 5.x, CONFIG_NIMBLE_CPP_IDF=1):
    //   NimBLEDevice::init() calls nimble_port_init() which calls
    //   esp_bt_controller_init(). Matter's BLEManagerImpl::InitESPBleLayer()
    //   calls the SAME function. Whoever runs SECOND gets
    //   ESP_ERR_INVALID_STATE → crash (if NimBLEDevice) or graceful exit
    //   (if Matter, which uses SuccessOrExit).
    //
    //   Solution: NimBLE MUST be initialized first, always.
    //   When matter_enabled=true: Matter's BLE init then fails cleanly and
    //   BLE advertising is skipped. Matter continues via WiFi commissioning,
    //   which is the only path available on D1 Mini anyway.
    // ═══════════════════════════════════════════════════════════════════

    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PHASE 2: NimBLE Initialization");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");

    ESP_LOGI(TAG, "→ Initializing NimBLE (must precede Matter start)...");

    #ifdef CONFIG_BT_ENABLED
    {
        esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✓ Classic BT memory released");
        }
    }
    #endif

    if (!NimBLEDevice::isInitialized()) {
        NimBLEDevice::init("BeltWinder");
        if (NimBLEDevice::isInitialized()) {
            ESP_LOGI(TAG, "✓ NimBLE initialized");
            ESP_LOGI(TAG, "  MTU: %d", NimBLEDevice::getMTU());
        } else {
            ESP_LOGE(TAG, "✗ CRITICAL: NimBLE init failed!");
        }
    } else {
        ESP_LOGI(TAG, "✓ NimBLE already initialized");
    }

    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // PHASE 3: WiFi Connection
    // ════════════════════════════════════════════════════════════════════

    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PHASE 3: WiFi Connection");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");

    #if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)

    // ────────────────────────────────────────────────────────────
    // 1. Prüfe credentials.h
    // ────────────────────────────────────────────────────────────

    #ifdef DEVELOP_BUILD
        const char* credentials_h_ssid = WIFI_SSID;
        const char* credentials_h_password = WIFI_PASSWORD;
        bool has_credentials_h = (strlen(credentials_h_ssid) > 0);
    #else
        const char* credentials_h_ssid = "";
        const char* credentials_h_password = "";
        bool has_credentials_h = false;
    #endif

    // ────────────────────────────────────────────────────────────
    // 2. Prüfe NVS Credentials
    // ────────────────────────────────────────────────────────────

    bool has_nvs_credentials = WiFiCredentials::exists();

    // ────────────────────────────────────────────────────────────
    // 3. Entscheide: WiFi Manager nötig?
    // ────────────────────────────────────────────────────────────

    if (!has_credentials_h && !has_nvs_credentials) {
        // Weder credentials.h noch NVS → WiFi Setup NÖTIG!
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   FIRST BOOT DETECTED             ║");
        ESP_LOGI(TAG, "║   WiFi Setup Required             ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "No WiFi credentials found:");
        ESP_LOGI(TAG, "  • credentials.h: %s", has_credentials_h ? "YES" : "NOT DEFINED");
        ESP_LOGI(TAG, "  • NVS storage:   %s", has_nvs_credentials ? "YES" : "EMPTY");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Starting WiFi Setup Portal...");
        ESP_LOGI(TAG, "");
        
        // Start WiFi Setup (blockiert!)
        bool success = WiFiManager::runSetup("BeltWinder-Setup", 300000);
        
        if (!success) {
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGE(TAG, "║   WiFi SETUP TIMEOUT              ║");
            ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "No WiFi credentials configured!");
            ESP_LOGE(TAG, "Device cannot operate without WiFi.");
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "→ Halting...");
            
            while(1) {
                delay(1000);
            }
        }
        
    } else {
        // Credentials vorhanden → WiFi Setup ÜBERSPRUNGEN
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   WiFi CREDENTIALS AVAILABLE      ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Credentials found:");
        ESP_LOGI(TAG, "  • credentials.h: %s", has_credentials_h ? "YES ✓" : "NO");
        ESP_LOGI(TAG, "  • NVS storage:   %s", has_nvs_credentials ? "YES ✓" : "NO");
        ESP_LOGI(TAG, "");
        
        if (has_credentials_h) {
            ESP_LOGI(TAG, "Priority: Using credentials.h (development mode)");
        } else {
            ESP_LOGI(TAG, "Priority: Using NVS credentials (production mode)");
        }
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ WiFi Setup Portal SKIPPED");
        ESP_LOGI(TAG, "");
        
        // WiFi EXPLIZIT STARTEN!
        ESP_LOGI(TAG, "→ Starting WiFi connection...");
        
        if (has_credentials_h) {
            // Use credentials.h
            WiFi.begin(credentials_h_ssid, credentials_h_password);
            ESP_LOGI(TAG, "  Using: credentials.h (SSID: %s)", credentials_h_ssid);
        } else {
            
            if (has_nvs_credentials) {              
                Preferences prefs;
                prefs.begin("wifi_creds", true);
                String ssid = prefs.getString("ssid", "");
                String password = prefs.getString("password", "");
                prefs.end();
                
                WiFi.begin(ssid.c_str(), password.c_str());
                ESP_LOGI(TAG, "  Using: NVS (SSID: %s)", ssid.c_str());
            }
        }
        
        ESP_LOGI(TAG, "");
    }

    #endif

    ESP_LOGI(TAG, "");


    // ════════════════════════════════════════════════════════════════════
    // PHASE 4: Shutter Driver
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PHASE 4: Shutter Driver");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    
    shutter_handle = shutter_driver_init();
    if (!shutter_handle) {
        ESP_LOGE(TAG, "✗ Failed to initialize shutter driver");
        return;
    }
    
    ((RollerShutter*)shutter_handle)->loadStateFromKVS();
    ((RollerShutter*)shutter_handle)->setCalibrationCompleteCallback(onCalibrationComplete);
    shutter_driver_set_operational_state_callback(shutter_handle, onShutterStateChanged);
    
    ESP_LOGI(TAG, "✓ Shutter driver initialized");
    ESP_LOGI(TAG, "  Position: %d%%", shutter_driver_get_current_percent(shutter_handle));
    ESP_LOGI(TAG, "  Calibrated: %s", shutter_driver_is_calibrated(shutter_handle) ? "YES" : "NO");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // PHASE 5: Device Naming
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PHASE 5: Device Naming");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    
    deviceNaming = new DeviceNaming();
    deviceNaming->load();
    
    DeviceNaming::DeviceName names = deviceNaming->getNames();
    
    ESP_LOGI(TAG, "✓ Device naming initialized");
    ESP_LOGI(TAG, "  Hostname: %s.local", names.hostname.c_str());
    ESP_LOGI(TAG, "  Matter Name: %s", names.matterName.c_str());
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // PHASE 6: BLE Manager (LAZY - nur Struktur, kein Start)
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PHASE 6: BLE Manager (Lazy)");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    
    bleManager = new ShellyBLEManager();
    bleManager->setSensorDataCallback(onBLESensorData);
    
    if (!bleManager->begin()) {
        ESP_LOGE(TAG, "✗ Shelly BLE Manager init failed");
        delete bleManager;
        bleManager = nullptr;
    } else {
        ESP_LOGI(TAG, "✓ Shelly BLE Manager initialized (lazy mode)");
        ESP_LOGI(TAG, "  BLE scanner will start on-demand");
    }
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // PHASE 7: Web UI (NUR NACH WIFI!)
    // ════════════════════════════════════════════════════════════════════

    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PHASE 7: Web UI");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");

    // ✅ Warte auf WiFi-Verbindung (mit Watchdog-Reset!)
    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGW(TAG, "⏳ Waiting for WiFi connection...");
        
        uint8_t retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 20) {
            // ✅ WICHTIG: Watchdog zurücksetzen!
            esp_task_wdt_reset();
            
            delay(500);
            retries++;
            ESP_LOGI(TAG, "  Attempt %d/20... (Status: %d)", retries, WiFi.status());
        }
        
        if (WiFi.status() != WL_CONNECTED) {
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "✗ WiFi connection FAILED!");
            ESP_LOGE(TAG, "  Status: %d", WiFi.status());
            ESP_LOGE(TAG, "  SSID: %s", WiFi.SSID().c_str());
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "⚠️  WebUI may be unstable without WiFi!");
            ESP_LOGE(TAG, "");
        } else {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✓ WiFi connected successfully!");
            ESP_LOGI(TAG, "  SSID: %s", WiFi.SSID().c_str());
            ESP_LOGI(TAG, "  IP:   %s", WiFi.localIP().toString().c_str());
            ESP_LOGI(TAG, "  RSSI: %d dBm", WiFi.RSSI());
            ESP_LOGI(TAG, "");
        }
    }

    // Memory Check VOR WebUI-Start
    uint32_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "📊 Free Heap before WebUI: %u bytes (%.1f KB)", 
            free_heap, free_heap / 1024.0f);

    if (free_heap < 50000) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "⚠️⚠️⚠️ CRITICAL: Low Memory! ⚠️⚠️⚠️");
        ESP_LOGE(TAG, "  Free Heap: %u bytes (< 50KB)", free_heap);
        ESP_LOGE(TAG, "  WebUI may fail to start!");
        ESP_LOGE(TAG, "");
    }

    webUI = new WebUIHandler(shutter_handle, bleManager);
    webUI->begin();

    ESP_LOGI(TAG, "✓ Web UI started");
    ESP_LOGI(TAG, "  Access: http://%s.local", names.hostname.c_str());
    ESP_LOGI(TAG, "       or http://%s", WiFi.localIP().toString().c_str());
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // PHASE 8: OTA (falls WiFi vorhanden)
    // ════════════════════════════════════════════════════════════════════
    
    if (WiFi.status() == WL_CONNECTED) {
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "  PHASE 8: Arduino OTA");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "");
        
        ArduinoOTA.setHostname(names.hostname.c_str());
        ArduinoOTA.setPassword(OTA_PASSWORD);
        
        ArduinoOTA.onStart([]() {
            String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
            Serial.println("Start updating " + type);
            
            if (webUI) webUI->cleanup_idle_clients();
            // manualStop=false: preserve NVS continuous_scan flag so scan
            // auto-restarts after the OTA reboot (not a user stop request).
            if (bleManager) bleManager->stopScan(false);
        });
        
        ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("Error[%u]: ", error);
            if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
            else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
            else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
            else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
            else if (error == OTA_END_ERROR) Serial.println("End Failed");
        });
        
        ArduinoOTA.begin();
        
        ESP_LOGI(TAG, "✓ Arduino OTA enabled");
        ESP_LOGI(TAG, "  Hostname: %s.local", names.hostname.c_str());
        ESP_LOGI(TAG, "");
    }
    
    // ════════════════════════════════════════════════════════════════════
    // PHASE 9: CONDITIONAL MATTER INITIALIZATION
    // ════════════════════════════════════════════════════════════════════
    
        if (matter_enabled) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
        ESP_LOGI(TAG, "║                                                           ║");
        ESP_LOGI(TAG, "║         MATTER AUTO-START (FLAG ENABLED)                  ║");
        ESP_LOGI(TAG, "║                                                           ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        // ────────────────────────────────────────────────────────────────
        // Create Matter Node
        // ────────────────────────────────────────────────────────────────
        
        if (!createMatterNode()) {
            ESP_LOGE(TAG, "✗ Failed to create Matter node!");
            ESP_LOGE(TAG, "  Continuing without Matter...");
        } else {
            ESP_LOGI(TAG, "✓ Matter node created");
            
            // Kurze Pause für Node-Stabilisierung
            vTaskDelay(pdMS_TO_TICKS(500));
            
            // ────────────────────────────────────────────────────────────
            // Start Matter Stack
            // ────────────────────────────────────────────────────────────
            
            if (!startMatterStack()) {
                ESP_LOGE(TAG, "✗ Failed to start Matter stack!");
                ESP_LOGE(TAG, "  Continuing without Matter...");
            } else {
                ESP_LOGI(TAG, "✓ Matter stack started successfully");
                
                // Hardware initialisieren (falls commissioned)
                bool is_commissioned = Matter.isDeviceCommissioned() && 
                                    (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0);
                
                if (is_commissioned && !hardware_initialized) {
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "→ Device is commissioned - initializing hardware...");
                    ((RollerShutter*)shutter_handle)->initHardware();
                    hardware_initialized = true;
                    ESP_LOGI(TAG, "✓ Hardware initialized");
                }
                
                // ════════════════════════════════════════════════════════════════
                // NimBLE was already initialized in PHASE 2 (before Matter).
                // Matter's BLEManagerImpl::InitESPBleLayer() will fail gracefully
                // because the controller is already owned by NimBLE. That is
                // intentional: we need WiFi commissioning only (no CHIPoBLE).
                // The scanner (ShellyBLEManager) uses the NimBLE stack from Phase 2.
                // ════════════════════════════════════════════════════════════════

                ESP_LOGI(TAG, "");
                if (NimBLEDevice::isInitialized()) {
                    ESP_LOGI(TAG, "✓ NimBLE already initialized (Phase 2) — scanner ready");
                } else {
                    ESP_LOGW(TAG, "⚠ NimBLE not initialized — BLE sensor scanning unavailable");
                }
            }  // end startMatterStack else
        }  // end createMatterNode else

        ESP_LOGI(TAG, "");
    }  // end matter_enabled

    // ════════════════════════════════════════════════════════════════════
    // AUTO-START BLE CONTINUOUS SCAN (if paired and was enabled before)
    // ════════════════════════════════════════════════════════════════════
    // loadPairedDevice() restores continuousScan from NVS.
    // startContinuousScan() is only called if user had it enabled before
    // reboot (i.e. not explicitly stopped via stopScan(manualStop=true)).
    // ════════════════════════════════════════════════════════════════════
    if (bleManager && bleManager->isPaired() && bleManager->isContinuousScanEnabled()) {
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "  AUTO-START: Continuous BLE Scan");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "  Paired device found, continuous scan was enabled.");
        ESP_LOGI(TAG, "  Starting scan after 1s BLE stabilization delay...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        bleManager->startContinuousScan();
        ESP_LOGI(TAG, "✓ Continuous scan started");
        ESP_LOGI(TAG, "");
    }

    // ════════════════════════════════════════════════════════════════════
    // SETUP COMPLETE
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "║               ✓ DEVICE READY FOR USE                     ║");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device Status:");
    ESP_LOGI(TAG, "  ✓ WiFi:        %s", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    ESP_LOGI(TAG, "  ✓ Shutter:     %s", shutter_driver_is_calibrated(shutter_handle) ? "Calibrated" : "Not calibrated");
    ESP_LOGI(TAG, "  ✓ BLE Manager: Initialized (lazy)");
    ESP_LOGI(TAG, "  ✓ Web UI:      Running");
    
    if (matter_enabled) {
        if (matter_stack_started) {
            bool commissioned = Matter.isDeviceCommissioned();
            ESP_LOGI(TAG, "  ✓ Matter:      Running (%s)", commissioned ? "Commissioned" : "Ready for commissioning");
        } else {
            ESP_LOGI(TAG, "  ✗ Matter:      Failed to start");
        }
    } else {
        ESP_LOGI(TAG, "  ○ Matter:      Disabled (enable via WebUI)");
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Next Steps:");
    ESP_LOGI(TAG, "  1. Access Web UI:    http://%s.local", names.hostname.c_str());
    ESP_LOGI(TAG, "                      or http://%s", WiFi.localIP().toString().c_str());
    
    if (!matter_enabled) {
        ESP_LOGI(TAG, "  2. Configure device (optional: pair BLE sensor)");
        ESP_LOGI(TAG, "  3. Click 'Start Matter Commissioning' to enable Matter");
    } else {
        bool commissioned = matter_stack_started && Matter.isDeviceCommissioned();
        if (!commissioned) {
            ESP_LOGI(TAG, "  2. Scan QR code with Matter controller app");
        } else {
            ESP_LOGI(TAG, "  2. Control via Matter controller or WebUI");
        }
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "");
}

    // ============================================================================
    // Loop
    // ============================================================================

void loop() {
    esp_task_wdt_reset();

     if (matter_stack_started) {
        // ────────────────────────────────────────────────────────────
        // Subscription Handler Update
        // ────────────────────────────────────────────────────────────
        subscriptionHandler.sendInitialUpdateIfNeeded();

        // ────────────────────────────────────────────────────────────
        // Commissioning Check
        // ────────────────────────────────────────────────────────────
        static bool was_commissioned = false;
        bool has_fabrics = (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0);
        bool is_commissioned = Matter.isDeviceCommissioned() && has_fabrics;
        
        if (!was_commissioned && is_commissioned && !hardware_initialized) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "█████████████████████████████████████████████████████████");
            ESP_LOGI(TAG, "█                                                       █");
            ESP_LOGI(TAG, "█           ✓ COMMISSIONING COMPLETE                  █");
            ESP_LOGI(TAG, "█                                                       █");
            ESP_LOGI(TAG, "█████████████████████████████████████████████████████████");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Matter Status:");
            ESP_LOGI(TAG, "  • Fabrics: %d", has_fabrics);
            ESP_LOGI(TAG, "  • Commissioned: YES ✓");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Initializing hardware...");
            
            ((RollerShutter*)shutter_handle)->initHardware();
            hardware_initialized = true;
            
            ESP_LOGI(TAG, "✓ Hardware initialized");
            ESP_LOGI(TAG, "✓ Device is now fully operational");
            ESP_LOGI(TAG, "");
        }
        was_commissioned = is_commissioned;

        // ────────────────────────────────────────────────────────────
        // Window state async re-broadcast (PENDING → TILTED/OPEN after delay)
        // classifyWindowAngle() sets windowStateChanged when called from loop().
        // We re-broadcast the last cached BLE data with the updated window_state.
        // ────────────────────────────────────────────────────────────
        if (lastBLEDataValid &&
            shutter_driver_consume_window_state_changed(shutter_handle)) {
            if (webUI) webUI->broadcastSensorDataUpdate(lastBLEAddress, lastBLEData);
        }

        // ────────────────────────────────────────────────────────────
        // Shutter Control Loop — always run when hardware is ready.
        // State machine, calibration, and manual buttons must work
        // even before Matter commissioning is complete.
        // ────────────────────────────────────────────────────────────
        if (hardware_initialized) {
            shutter_driver_loop(shutter_handle);
        }

        // ────────────────────────────────────────────────────────────
        // Matter Attribute Updates — only when commissioned
        // ────────────────────────────────────────────────────────────
        if (is_commissioned && hardware_initialized) {

            // Matter Update Strategy
            if (shutter_driver_should_send_matter_update(shutter_handle)) {
                uint8_t percent = shutter_driver_get_current_percent(shutter_handle);
                uint16_t pos_100ths = percent * 100;
                
                RollerShutter::State state = shutter_driver_get_current_state(shutter_handle);
                
                esp_matter_attr_val_t val = esp_matter_uint16(pos_100ths);
                
                if (state == RollerShutter::State::MOVING_UP || 
                    state == RollerShutter::State::MOVING_DOWN) {
                    
                    ESP_LOGI(TAG, "Matter Update (live): %d%%", percent);
                    
                    attribute::update(window_covering_endpoint_id, 
                                    chip::app::Clusters::WindowCovering::Id,
                                    chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id, 
                                    &val);
                    
                } else if (state == RollerShutter::State::STOPPED) {
                    
                    ESP_LOGI(TAG, "Matter Update (stopped): %d%%", percent);
                    
                    attribute::update(window_covering_endpoint_id, 
                                    chip::app::Clusters::WindowCovering::Id,
                                    chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id, 
                                    &val);
                    
                    attribute::update(window_covering_endpoint_id, 
                                    chip::app::Clusters::WindowCovering::Id,
                                    chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id, 
                                    &val);
                }
                
                // Operational Status
                uint8_t opStateValue = (state == RollerShutter::State::MOVING_UP) ? 0x05 :
                                    (state == RollerShutter::State::MOVING_DOWN) ? 0x06 : 0x00;
                
                esp_matter_attr_val_t opstate_val = esp_matter_uint8(opStateValue);
                attribute::update(window_covering_endpoint_id, 
                                chip::app::Clusters::WindowCovering::Id,
                                chip::app::Clusters::WindowCovering::Attributes::OperationalStatus::Id, 
                                &opstate_val);
                
                shutter_driver_mark_matter_update_sent(shutter_handle);
            }
        }

        // ────────────────────────────────────────────────────────────
        // Mode Sync (nur wenn Matter läuft!)
        // ────────────────────────────────────────────────────────────
        static uint32_t last_mode_sync = 0;
        static uint8_t last_mode_value = 0xFF;
        
        if (millis() - last_mode_sync >= 2000 && window_covering_endpoint_id != 0) {
            last_mode_sync = millis();
            
            cluster_t* wc_cluster = cluster::get(window_covering_endpoint_id, 
                                                chip::app::Clusters::WindowCovering::Id);
            if (wc_cluster) {
                attribute_t* mode_attr = attribute::get(wc_cluster, 
                                                    chip::app::Clusters::WindowCovering::Attributes::Mode::Id);
                if (mode_attr) {
                    esp_matter_attr_val_t mode_val;
                    if (attribute::get_val(mode_attr, &mode_val) == ESP_OK) {
                        uint8_t current_mode = mode_val.val.u8;
                        
                        if (current_mode != last_mode_value) {
                            bool inverted = (current_mode & 0x01) != 0;
                            
                            ESP_LOGI(TAG, "Mode changed: 0x%02X → 0x%02X (inverted=%s)", 
                                    last_mode_value, current_mode, inverted ? "YES" : "NO");
                            
                            if (webUI) {
                                char broadcast_msg[128];
                                snprintf(broadcast_msg, sizeof(broadcast_msg),
                                        "{\"type\":\"direction\",\"inverted\":%s}",
                                        inverted ? "true" : "false");
                                
                                webUI->broadcast_to_all_clients(broadcast_msg);
                            }
                            
                            last_mode_value = current_mode;
                        }
                    }
                }
            }
        }

        #ifdef CONFIG_ENABLE_CUSTOM_CLUSTER_IP
        // IP Address Update
        static uint32_t last_ip_check = 0;
        if (WiFi.status() == WL_CONNECTED && millis() - last_ip_check >= 30000) {
            last_ip_check = millis();
            String new_ip = WiFi.localIP().toString();
            
            if (strcmp(device_ip_str, new_ip.c_str()) != 0) {
                ESP_LOGI(TAG, "IP changed: %s → %s", device_ip_str, new_ip.c_str());
                snprintf(device_ip_str, sizeof(device_ip_str), "%s", new_ip.c_str());
                
                cluster_t* custom_cluster = cluster::get(window_covering_endpoint_id, 
                                                        CLUSTER_ID_ROLLERSHUTTER_CONFIG);
                
                if (custom_cluster) {
                    attribute_t* ip_attr = attribute::get(custom_cluster, ATTR_ID_DEVICE_IP);
                    
                    if (ip_attr) {
                        esp_matter_attr_val_t ip_val = esp_matter_char_str(device_ip_str, DEVICE_IP_MAX_LENGTH);
                        esp_err_t ret = attribute::set_val(ip_attr, &ip_val);
                        
                        if (ret != ESP_OK) {
                            ESP_LOGE(TAG, "✗ Failed to update Device IP: %s", esp_err_to_name(ret));
                        }
                    }
                }
            }
        }
        #endif

    } else {
        // ════════════════════════════════════════════════════════════════
        // Matter nicht gestartet → NUR Hardware-Basics
        // ════════════════════════════════════════════════════════════════

        // Window state async re-broadcast (same as in commissioned branch)
        if (lastBLEDataValid &&
            shutter_driver_consume_window_state_changed(shutter_handle)) {
            if (webUI) webUI->broadcastSensorDataUpdate(lastBLEAddress, lastBLEData);
        }

        // Shutter kann OHNE Matter laufen (z.B. bei Kalibrierung)
        if (hardware_initialized) {
            shutter_driver_loop(shutter_handle);
        }
    }

    // ════════════════════════════════════════════════════════════════
    // IMMER aktiv (unabhängig von Matter):
    // ════════════════════════════════════════════════════════════════

    // Web UI Updates
    static uint32_t last_web = 0;
    if (millis() - last_web >= 2000) {
        last_web = millis();
        if (webUI) {
            char status_msg[128];
            snprintf(status_msg, sizeof(status_msg),
                     "{\"type\":\"status\",\"pos\":%d,\"cal\":%s,\"inv\":%s}",
                     shutter_driver_get_current_percent(shutter_handle),
                     shutter_driver_is_calibrated(shutter_handle) ? "true" : "false",
                     shutter_driver_get_direction_inverted(shutter_handle) ? "true" : "false");
            webUI->broadcast_to_all_clients(status_msg);
        }
    }

    // BLE Manager Loop
    if (bleManager) {
        bleManager->loop();
        esp_task_wdt_reset();
    }

    // WebSocket Cleanup
    static uint32_t last_ws_cleanup = 0;
    if (millis() - last_ws_cleanup >= 10000) {
        last_ws_cleanup = millis();
        if (webUI) {
            webUI->cleanup_idle_clients();
        }
    }

    // BLE status heartbeat — keeps all open browser tabs in sync even if
    // no BLE event fires for a while (e.g. scan stopped, no sensor movement).
    static uint32_t last_ble_heartbeat = 0;
    if (millis() - last_ble_heartbeat >= 10000) {
        last_ble_heartbeat = millis();
        if (webUI && bleManager && webUI->get_client_count() > 0) {
            webUI->broadcastBLEStatus();
        }
    }

    // Arduino OTA
    ArduinoOTA.handle();
   
    // Memory Status
    static uint32_t last_mem_check = 0;
    if (millis() - last_mem_check >= 300000) {
        last_mem_check = millis();
        
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_heap = esp_get_minimum_free_heap_size();
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║     SYSTEM MEMORY STATUS          ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "Free heap: %u bytes (%.1f KB)", 
                 free_heap, free_heap / 1024.0f);
        ESP_LOGI(TAG, "Min free heap: %u bytes (%.1f KB)", 
                 min_heap, min_heap / 1024.0f);
        
        if (webUI) {
            ESP_LOGI(TAG, "WebSocket clients: %d", webUI->get_client_count());
        }
        
        if (free_heap < 50000) {
            ESP_LOGE(TAG, "✗✗✗ CRITICAL: Low memory! ✗✗✗");
        }
        ESP_LOGI(TAG, "");
    }

    delay(1);
}

// ============================================================================
// Callbacks
// ============================================================================

static esp_err_t app_attribute_update_cb(callback_type_t type, uint16_t endpoint_id, 
                                        uint32_t cluster_id, uint32_t attribute_id, 
                                        esp_matter_attr_val_t *val, void *priv) {
    
    // ════════════════════════════════════════════════════════════════
    // Nur Window Covering Endpoint beachten
    // ════════════════════════════════════════════════════════════════
    
    if (endpoint_id != window_covering_endpoint_id) {
        return ESP_OK;  // Andere Endpoints ignorieren
    }
    
    // ════════════════════════════════════════════════════════════════
    // PRE_UPDATE Events (bevor Attribut geschrieben wird)
    // ════════════════════════════════════════════════════════════════
    
    if (type == PRE_UPDATE) {
        
        // ────────────────────────────────────────────────────────────
        // Target Position (von GoToLiftPercentage Command)
        // ────────────────────────────────────────────────────────────
        
        if (cluster_id == chip::app::Clusters::WindowCovering::Id &&
            attribute_id == chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id) {
            
            uint16_t target_100ths = val->val.u16;
            uint8_t percent = target_100ths / 100;
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║       TARGET POSITION ATTRIBUTE UPDATE (PRE)              ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Raw value (100ths): %d", target_100ths);
            ESP_LOGI(TAG, "Calculated percent: %d%%", percent);
            ESP_LOGI(TAG, "Source: GoToLiftPercentage Command (0x05)");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "→ Calling shutter_driver_go_to_lift_percent(%d)", percent);
            ESP_LOGI(TAG, "");
            
            shutter_driver_go_to_lift_percent(shutter_handle, percent);
            
            return ESP_OK;
        }
        
        // ────────────────────────────────────────────────────────────
        // Mode Attribut (Direction)
        // ────────────────────────────────────────────────────────────
        
        if (cluster_id == chip::app::Clusters::WindowCovering::Id &&
            attribute_id == chip::app::Clusters::WindowCovering::Attributes::Mode::Id) {
            
            uint8_t mode_bitmap = val->val.u8;
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║       MODE ATTRIBUTE UPDATE (PRE)                         ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Raw value (Bitmap8): 0x%02X", mode_bitmap);
            ESP_LOGI(TAG, "");
            
            // Bit 0: Motor Direction Reversed
            bool new_inverted = (mode_bitmap & 0x01) != 0;
            bool old_inverted = shutter_driver_get_direction_inverted(shutter_handle);
            
            if (new_inverted != old_inverted) {
                ESP_LOGI(TAG, "→ Direction changed: %s → %s", 
                         old_inverted ? "INVERTED" : "NORMAL",
                         new_inverted ? "INVERTED" : "NORMAL");
                
                shutter_driver_set_direction(shutter_handle, new_inverted);
                
                ESP_LOGI(TAG, "✓ Direction updated in driver");
                
                // WebUI Broadcast
                if (webUI) {
                    char broadcast_msg[128];
                    snprintf(broadcast_msg, sizeof(broadcast_msg),
                             "{\"type\":\"direction\",\"inverted\":%s}",
                             new_inverted ? "true" : "false");
                    
                    webUI->broadcast_to_all_clients(broadcast_msg);
                    
                    ESP_LOGI(TAG, "✓ Direction change broadcasted to WebUI clients");
                }
            } else {
                ESP_LOGI(TAG, "ℹ Direction unchanged: %s", 
                         new_inverted ? "INVERTED" : "NORMAL");
            }
            
            ESP_LOGI(TAG, "");
            
            return ESP_OK;
        }
    }
    
    // ════════════════════════════════════════════════════════════════
    // POST_UPDATE Events (nachdem Attribut geschrieben wurde)
    // ════════════════════════════════════════════════════════════════
    
    else if (type == POST_UPDATE) {
        
        // ────────────────────────────────────────────────────────────
        // Target Position (Falls PRE_UPDATE nicht funktioniert)
        // ────────────────────────────────────────────────────────────
        
        if (cluster_id == chip::app::Clusters::WindowCovering::Id &&
            attribute_id == chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id) {
            
            uint16_t target_100ths = val->val.u16;
            uint8_t percent = target_100ths / 100;
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║       TARGET POSITION ATTRIBUTE UPDATE (POST)             ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Raw value (100ths): %d", target_100ths);
            ESP_LOGI(TAG, "Calculated percent: %d%%", percent);
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "→ Calling shutter_driver_go_to_lift_percent(%d)", percent);
            ESP_LOGI(TAG, "");
            
            shutter_driver_go_to_lift_percent(shutter_handle, percent);
            
            return ESP_OK;
        }
    }
    
    // ════════════════════════════════════════════════════════════════
    // Default: Unbekanntes Attribut → OK zurückgeben
    // ════════════════════════════════════════════════════════════════
    
    return ESP_OK;
}


static esp_err_t app_command_cb(const ConcreteCommandPath &path, 
                                TLV::TLVReader &reader, void *priv) {
    
    ESP_LOGI(TAG, "┌─────────────────────────────────");
    ESP_LOGI(TAG, "│ MATTER COMMAND RECEIVED");
    ESP_LOGI(TAG, "├─────────────────────────────────");
    ESP_LOGI(TAG, "│ Cluster:  0x%04X", path.mClusterId);
    ESP_LOGI(TAG, "│ Command:  0x%04X", path.mCommandId);
    ESP_LOGI(TAG, "│ Endpoint: %d", path.mEndpointId);

    // ════════════════════════════════════════════════════════════════
    // Window Covering Commands (Fallback wenn Delegate nicht funktioniert)
    // ════════════════════════════════════════════════════════════════

    if (path.mClusterId == chip::app::Clusters::WindowCovering::Id) {
        ESP_LOGI(TAG, "│ → Window Covering Command");
        
        switch (path.mCommandId) {
            
            case chip::app::Clusters::WindowCovering::Commands::UpOrOpen::Id: {
                ESP_LOGI(TAG, "│   Type: UpOrOpen");
                ESP_LOGI(TAG, "└─────────────────────────────────");
                
                shutter_driver_go_to_lift_percent(shutter_handle, 0);
                return ESP_OK;
            }
            
            case chip::app::Clusters::WindowCovering::Commands::DownOrClose::Id: {
                ESP_LOGI(TAG, "│   Type: DownOrClose");
                ESP_LOGI(TAG, "└─────────────────────────────────");
                
                shutter_driver_go_to_lift_percent(shutter_handle, 100);
                return ESP_OK;
            }
            
            case chip::app::Clusters::WindowCovering::Commands::StopMotion::Id: {
                ESP_LOGI(TAG, "│   Type: StopMotion");
                ESP_LOGI(TAG, "└─────────────────────────────────");
                
                shutter_driver_stop_motion(shutter_handle);
                return ESP_OK;
            }
            
            case chip::app::Clusters::WindowCovering::Commands::GoToLiftPercentage::Id: {
                ESP_LOGI(TAG, "│   Type: GoToLiftPercentage");
                
                chip::app::Clusters::WindowCovering::Commands::GoToLiftPercentage::DecodableType cmd;
                
                if (chip::app::DataModel::Decode(reader, cmd) == CHIP_NO_ERROR) {
                    uint8_t percent = cmd.liftPercent100thsValue / 100;
                    
                    ESP_LOGI(TAG, "│   Target: %d%%", percent);
                    ESP_LOGI(TAG, "└─────────────────────────────────");
                    
                    shutter_driver_go_to_lift_percent(shutter_handle, percent);
                    return ESP_OK;
                    
                } else {
                    ESP_LOGE(TAG, "│   ✗ Failed to decode command payload");
                    ESP_LOGI(TAG, "└─────────────────────────────────");
                    return ESP_FAIL;
                }
            }
            
            default:
                ESP_LOGW(TAG, "│   ⚠ Unknown command ID: 0x%04X", path.mCommandId);
                ESP_LOGI(TAG, "└─────────────────────────────────");
                return ESP_ERR_NOT_SUPPORTED;
        }
    }
    
    // ════════════════════════════════════════════════════════════════
    // Custom Cluster - Calibration
    // ════════════════════════════════════════════════════════════════
    
    if (path.mClusterId == CLUSTER_ID_ROLLERSHUTTER_CONFIG && 
        path.mCommandId == CMD_ID_START_CALIBRATION) {
        ESP_LOGI(TAG, "│ → Custom: START_CALIBRATION");
        ESP_LOGI(TAG, "└─────────────────────────────────");
        
        shutter_driver_start_calibration(shutter_handle);
        
        // Command Response senden
        return ESP_OK;
    }

    #ifdef CONFIG_ENABLE_SCENE_CLUSTER
    // ════════════════════════════════════════════════════════════════
    // Scene Cluster Commands
    // ════════════════════════════════════════════════════════════════
    
    if (path.mClusterId == CLUSTER_ID_SCENES) {
        ESP_LOGI(TAG, "│ → Scene Cluster Command");
        
        if (path.mCommandId == CMD_ID_RECALL_SCENE) {
            ESP_LOGI(TAG, "│   Type: RecallScene");
            
            uint16_t groupId = 0;
            uint8_t sceneId = 0;
            
            chip::TLV::TLVType containerType;
            if (reader.EnterContainer(containerType) == CHIP_NO_ERROR) {
                if (reader.Next() == CHIP_NO_ERROR) {
                    reader.Get(groupId);
                }
                if (reader.Next() == CHIP_NO_ERROR) {
                    reader.Get(sceneId);
                }
                reader.ExitContainer(containerType);
                
                ESP_LOGI(TAG, "│   Group: %d", groupId);
                ESP_LOGI(TAG, "│   Scene: %d", sceneId);
                
                // Finde Mapping
                bool found = false;
                for (int i = 0; i < SCENE_MAPPING_COUNT; i++) {
                    if (SCENE_MAPPINGS[i].sceneId == sceneId) {
                        uint8_t targetPos = SCENE_MAPPINGS[i].shutterPosition;
                        
                        ESP_LOGI(TAG, "│   → Moving to %d%% (%s)", 
                                 targetPos, SCENE_MAPPINGS[i].description);
                        ESP_LOGI(TAG, "└─────────────────────────────────");
                        
                        shutter_driver_go_to_lift_percent(shutter_handle, targetPos);
                        
                        // Update Scene Attributes
                        esp_matter_attr_val_t scene_val = esp_matter_uint8(sceneId);
                        attribute::update(window_covering_endpoint_id, 
                                        CLUSTER_ID_SCENES,
                                        ATTR_ID_CURRENT_SCENE, 
                                        &scene_val);
                        
                        esp_matter_attr_val_t valid_val = esp_matter_bool(true);
                        attribute::update(window_covering_endpoint_id,
                                        CLUSTER_ID_SCENES,
                                        ATTR_ID_SCENE_VALID,
                                        &valid_val);
                        
                        found = true;
                        
                        // ✅ Command Response senden
                        return ESP_OK;
                    }
                }
                
                if (!found) {
                    ESP_LOGW(TAG, "│   ⚠ Unknown Scene ID: %d", sceneId);
                    ESP_LOGI(TAG, "└─────────────────────────────────");
                    return ESP_FAIL;
                }
            }
        }
        
        else if (path.mCommandId == CMD_ID_GET_SCENE_MEMBERSHIP) {
            ESP_LOGI(TAG, "│   Type: GetSceneMembership");
            ESP_LOGI(TAG, "└─────────────────────────────────");
            
            // ✅ Command Response senden
            return ESP_OK;
        }
        
        return ESP_ERR_NOT_SUPPORTED;
    }
    #endif // CONFIG_ENABLE_SCENE_CLUSTER
    
    // ════════════════════════════════════════════════════════════════
    // Unbekannter Cluster
    // ════════════════════════════════════════════════════════════════
    
    ESP_LOGW(TAG, "│ ⚠ Unsupported cluster: 0x%04X", path.mClusterId);
    ESP_LOGI(TAG, "└─────────────────────────────────");
    
    return ESP_ERR_NOT_SUPPORTED;
}

void performCompleteFactoryReset() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "║           ⚠️  FACTORY RESET IN PROGRESS ⚠️              ║");
    ESP_LOGI(TAG, "║                                                           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // 1. SHUTTER STATE (KVS)
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Clearing Shutter State...");
    
    if (shutter_handle) {
        Preferences prefs;
        if (prefs.begin("shutter", false)) {
            prefs.clear();
            prefs.end();
            ESP_LOGI(TAG, "  ✓ Shutter KVS cleared");
        }
    }
    
    // ════════════════════════════════════════════════════════════════════
    // 2. BLE MANAGER (ShellyBLE)
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Clearing BLE Data...");
    
    {
        Preferences prefs;
        if (prefs.begin("ShellyBLE", false)) {
            prefs.clear();
            prefs.end();
            ESP_LOGI(TAG, "  ✓ ShellyBLE NVS cleared");
        }
    }
    
    // ════════════════════════════════════════════════════════════════════
    // 3. WEBUI AUTH
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Clearing WebUI Auth...");
    
    {
        Preferences prefs;
        if (prefs.begin("webui_auth", false)) {
            prefs.clear();
            prefs.end();
            ESP_LOGI(TAG, "  ✓ WebUI Auth NVS cleared");
        }
    }
    
    // ════════════════════════════════════════════════════════════════════
    // 4. MATTER SETTINGS (inkl. matter_enabled Flag!)
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Clearing Matter Settings...");
    
    {
        Preferences prefs;
        if (prefs.begin("matter", false)) {
            // ✅ WICHTIG: matter_enabled Flag explizit löschen!
            bool had_flag = prefs.getBool("matter_enabled", false);
            
            prefs.clear();
            prefs.end();
            
            ESP_LOGI(TAG, "  ✓ Matter NVS cleared");
            if (had_flag) {
                ESP_LOGI(TAG, "  ✓ 'matter_enabled' flag REMOVED");
                ESP_LOGI(TAG, "    → Matter will NOT auto-start after reboot");
            }
        }
    }
    
    // ════════════════════════════════════════════════════════════════════
    // 5. DEVICE NAMING
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Clearing Device Naming...");
    
    {
        Preferences prefs;
        if (prefs.begin("device_name", false)) {
            prefs.clear();
            prefs.end();
            ESP_LOGI(TAG, "  ✓ Device Naming NVS cleared");
        }
    }
    
    // ════════════════════════════════════════════════════════════════════
    // 6. WIFI CREDENTIALS
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Clearing WiFi Credentials...");
    
    {
        Preferences prefs;
        if (prefs.begin("wifi_creds", false)) {
            prefs.clear();
            prefs.end();
            ESP_LOGI(TAG, "  ✓ WiFi Credentials NVS cleared");
        }
    }
    
    // ════════════════════════════════════════════════════════════════════
    // 7. BLE SCAN STOPPEN (falls aktiv)
    // ════════════════════════════════════════════════════════════════════

    ESP_LOGI(TAG, "→ Stopping BLE Scan...");

    if (bleManager) {
        if (bleManager->isScanActive()) {
            ESP_LOGI(TAG, "  → BLE Scan is active, stopping...");
            bleManager->stopScan(true);  // Manual stop
            
            // Warte bis Scanner wirklich gestoppt ist
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        // Unpair device (löscht auch NVS)
        if (bleManager->isPaired()) {
            ESP_LOGI(TAG, "  → Unpairing BLE device...");
            bleManager->unpairDevice();
        }
        
        ESP_LOGI(TAG, "  ✓ BLE cleaned up");
    }

    ESP_LOGI(TAG, "");

    // ════════════════════════════════════════════════════════════════════
    // 8. MATTER STACK (WICHTIG!)
    // ════════════════════════════════════════════════════════════════════

    ESP_LOGI(TAG, "→ Performing Matter Factory Reset...");

    esp_matter::factory_reset();  // ← Löscht alle Matter-Daten & rebootet
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓✓✓ FACTORY RESET COMPLETE ✓✓✓");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "All data erased:");
    ESP_LOGI(TAG, "  • Shutter calibration & position");
    ESP_LOGI(TAG, "  • BLE sensor pairing");
    ESP_LOGI(TAG, "  • WebUI credentials");
    ESP_LOGI(TAG, "  • Matter commissioning");
    ESP_LOGI(TAG, "  • Matter auto-start flag");
    ESP_LOGI(TAG, "  • Device naming");
    ESP_LOGI(TAG, "  • WiFi credentials");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device will restart in 3 seconds...");
    ESP_LOGI(TAG, "Device will boot in BLE-ONLY mode (no Matter)");
    ESP_LOGI(TAG, "");
    
    // Wait before restart
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // esp_restart() wird durch factory_reset() automatisch aufgerufen
}


// ============================================================================
// SoftAP Stub
// ============================================================================

extern "C" {
    #include "esp_netif.h"
    esp_netif_t* esp_netif_create_default_wifi_ap(void) {
        return NULL;
    }
}