#include <Arduino.h>
#include <WiFi.h>
#include <Matter.h>
#include <Preferences.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_bt.h>
#include <app/server/Server.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_cluster.h>
#include <esp_matter_feature.h> 

#include "credentials.h"
#include "rollershutter_driver.h"
#include "rollershutter.h"
#include "matter_cluster_defs.h"
#include "web_ui_handler.h"
#include "shelly_ble_manager.h"

using namespace chip;
using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::cluster;
using namespace esp_matter::command;
using namespace esp_matter::endpoint;

#define APP_VERSION "1.3.0"

static const char* TAG = "Main";

// ============================================================================
// Configuration
// ============================================================================

#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;
#endif

const uint16_t INSTALLED_OPEN_LIMIT_LIFT_CM = 0;
const uint16_t INSTALLED_CLOSED_LIMIT_LIFT_CM = 200;

// ============================================================================
// State Variables
// ============================================================================

static app_driver_handle_t shutter_handle = nullptr;
static WebUIHandler* webUI = nullptr;
static ShellyBLEManager* bleManager = nullptr;
static Preferences matterPref;

static char device_ip_str[16] = "0.0.0.0";
static bool hardware_initialized = false;
static TaskHandle_t loop_task_handle = nullptr;

static uint16_t window_covering_endpoint_id = 0;

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
    
    if (data.hasButtonEvent) {
        const char* eventName = (data.buttonEvent == BUTTON_SINGLE_PRESS) ? "SINGLE PRESS" : 
                               (data.buttonEvent == BUTTON_HOLD) ? "HOLD" : "UNKNOWN";
        ESP_LOGI(TAG, "  Button Event: %s", eventName);
    }

    // WebUI Update
    if (webUI) {
        webUI->broadcastSensorDataUpdate(address, data);
    }

    // Matter Update (nur wenn commissioned)
    if (contact_sensor_matter_enabled) {
        bool is_commissioned = Matter.isDeviceCommissioned() && 
                              (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0);
        
        if (is_commissioned) {
            // ✅ Contact Sensor Endpoint erstellen (NUR EINMAL!)
            if (!contact_sensor_endpoint_active) {
                ESP_LOGI(TAG, "→ Creating Contact Sensor endpoint...");
                node_t* node = node::get();
                if (node) {
                    createContactSensorEndpoint(node);
                }
            }
            
            // ✅ Power Source Endpoint erstellen (NUR EINMAL!)
            if (!power_source_endpoint_active) {  // ← WICHTIGE PRÜFUNG!
                ESP_LOGI(TAG, "→ Creating Power Source endpoint...");
                node_t* node = node::get();
                if (node) {
                    createPowerSourceEndpoint(node);
                }
            }
            
            // ✅ Attribute updaten (NUR wenn Endpoints aktiv sind)
            if (contact_sensor_endpoint_active && contact_sensor_endpoint_id != 0) {
                ESP_LOGD(TAG, "→ Updating Contact Sensor attributes...");
                
                // Contact State
                esp_matter_attr_val_t contact_val = esp_matter_bool(!data.windowOpen);
                attribute::update(contact_sensor_endpoint_id,
                                 chip::app::Clusters::BooleanState::Id,
                                 chip::app::Clusters::BooleanState::Attributes::StateValue::Id,
                                 &contact_val);
            }
            
            // ✅ Power Source Attribute updaten (NUR wenn Endpoint aktiv ist)
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
    
    // Rolladen-Logik (IMMER aktiv)
    shutter_driver_set_window_state(shutter_handle, data.windowOpen);
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
    
    matterPref.begin("matter", false);
    matterPref.putBool("cs_active", true);
    matterPref.end();
    
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
    
    attribute::create(ps_cluster,
                    chip::app::Clusters::PowerSource::Attributes::Description::Id,
                    ATTRIBUTE_FLAG_NONE,
                    esp_matter_char_str("Battery", 16));

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
    
    matterPref.begin("matter", false);
    matterPref.putBool("ps_active", true);
    matterPref.end();
    
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
    
    // ✅ Status aus Preferences löschen
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
    
    if (!Matter.isDeviceCommissioned()) {
        ESP_LOGW(TAG, "⚠ Matter not commissioned - endpoints will be created after commissioning");
    }
    
    contact_sensor_matter_enabled = true;
    
    // State speichern
    matterPref.begin("matter", false);
    matterPref.putBool("cs_matter_en", true);
    matterPref.end();
    
    // Endpoints erstellen wenn möglich
    if (Matter.isDeviceCommissioned() && bleManager && bleManager->isPaired()) {
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

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);

    esp_log_level_set("*", ESP_LOG_INFO);           // Global: INFO

    esp_log_level_set("chip[DL]", ESP_LOG_WARN);    // Matter: WARN
    esp_log_level_set("chip[DMG]", ESP_LOG_ERROR);  
    esp_log_level_set("chip[SC]", ESP_LOG_ERROR);
    esp_log_level_set("esp_matter_attribute", ESP_LOG_ERROR);
    esp_log_level_set("esp_matter_command", ESP_LOG_INFO);
    esp_log_level_set("esp_matter_cluster", ESP_LOG_WARN);
    esp_log_level_set("BLEAutoStart", ESP_LOG_NONE); // BLE: INFO
    esp_log_level_set("ShellyBLE", ESP_LOG_NONE);   // BLE: INFO
    esp_log_level_set("BLESimple", ESP_LOG_NONE);   // BLE: INFO
    esp_log_level_set("NimBLE", ESP_LOG_NONE);      // NimBLE: INFO
    esp_log_level_set("wifi", ESP_LOG_NONE);       // WiFi: ERROR
    esp_log_level_set("Shutter", ESP_LOG_INFO);     // Shutter: DEBUG
    esp_log_level_set("Main", ESP_LOG_INFO);        // Main: DEBUG
    esp_log_level_set("WebUI", ESP_LOG_NONE);       // WebUI: DEBUG

    ESP_LOGI(TAG, "=== BeltWinder Matter - Starting ===");

    #ifdef CONFIG_BT_ENABLED
        ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
        ESP_LOGI(TAG, "Classic BT memory released for BLE");
    #endif

    pinMode(CONFIG_PULSE_COUNTER_PIN, INPUT_PULLUP);
    pinMode(CONFIG_MOTOR_UP_PIN, INPUT_PULLUP);
    pinMode(CONFIG_MOTOR_DOWN_PIN, INPUT_PULLUP);
    pinMode(CONFIG_BUTTON_UP_PIN, OUTPUT);
    digitalWrite(CONFIG_BUTTON_UP_PIN, HIGH);
    pinMode(CONFIG_BUTTON_DOWN_PIN, OUTPUT);
    digitalWrite(CONFIG_BUTTON_DOWN_PIN, HIGH);
    ESP_LOGI(TAG, "GPIOs configured");

    loop_task_handle = xTaskGetCurrentTaskHandle();
    esp_task_wdt_add(loop_task_handle);

    // Shutter Init
    shutter_handle = (RollerShutter*)shutter_driver_init();
    if (!shutter_handle) {
        ESP_LOGE(TAG, "Failed to initialize shutter driver");
        return;
    }
    
    ((RollerShutter*)shutter_handle)->loadStateFromKVS();

    if (Matter.isDeviceCommissioned()) {
        ESP_LOGI(TAG, "Already commissioned. Initializing hardware NOW...");
        ((RollerShutter*)shutter_handle)->initHardware();
        hardware_initialized = true;
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Matter commissioned - BLE is free for our use");
        ESP_LOGI(TAG, "");
    } else {
        ESP_LOGI(TAG, "Not commissioned. Hardware will init after pairing.");
    }
    
    ESP_LOGI(TAG, "Shutter initialized");

    // ========================================================================
    // Matter Node & Window Covering Endpoint
    // ========================================================================
    
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, nullptr);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    // ========================================================================
    // Window Covering Endpoint
    // ========================================================================
    
    window_covering_device::config_t wc_config;
    wc_config.window_covering.type = 0;  // ROLLERSHADE
    wc_config.window_covering.feature_flags = 
        (uint32_t)chip::app::Clusters::WindowCovering::Feature::kLift |
        (uint32_t)chip::app::Clusters::WindowCovering::Feature::kPositionAwareLift |
        (uint32_t)chip::app::Clusters::WindowCovering::Feature::kAbsolutePosition;
    
    endpoint_t *ep = window_covering_device::create(node, &wc_config, 
                                                     ENDPOINT_FLAG_NONE, NULL);
    if (!ep) {
        ESP_LOGE(TAG, "Failed to create Window Covering endpoint");
        return;
    }

    window_covering_endpoint_id = endpoint::get_id(ep);
    endpoint::set_priv_data(window_covering_endpoint_id, shutter_handle);
    
    ESP_LOGI(TAG, "Window Covering endpoint created (ID: %d) with Lift feature", 
             window_covering_endpoint_id);

    // Installed Limits setzen
    cluster_t* wc_cluster = cluster::get(ep, chip::app::Clusters::WindowCovering::Id);
    if (wc_cluster) {
        esp_matter_attr_val_t open_limit = esp_matter_uint16(INSTALLED_OPEN_LIMIT_LIFT_CM);
        attribute::update(window_covering_endpoint_id, chip::app::Clusters::WindowCovering::Id,
                         chip::app::Clusters::WindowCovering::Attributes::InstalledOpenLimitLift::Id, 
                         &open_limit);
        
        esp_matter_attr_val_t closed_limit = esp_matter_uint16(INSTALLED_CLOSED_LIMIT_LIFT_CM);
        attribute::update(window_covering_endpoint_id, chip::app::Clusters::WindowCovering::Id,
                         chip::app::Clusters::WindowCovering::Attributes::InstalledClosedLimitLift::Id, 
                         &closed_limit);
        
        ESP_LOGI(TAG, "Installed limits: %d-%d cm", 
                 INSTALLED_OPEN_LIMIT_LIFT_CM, INSTALLED_CLOSED_LIMIT_LIFT_CM);
    }

    // ========================================================================
    // Custom Cluster - Device IP
    // ========================================================================

    cluster_t *custom_cluster = cluster::create(ep, CLUSTER_ID_ROLLERSHUTTER_CONFIG, 
                                                CLUSTER_FLAG_SERVER);
    if (custom_cluster) {
        // Direction Inverted
        bool inverted = shutter_driver_get_direction_inverted(shutter_handle);
        attribute::create(custom_cluster, ATTR_ID_DIRECTION_INVERTED, 
                        ATTRIBUTE_FLAG_WRITABLE, esp_matter_bool(inverted));
        
        // String vorbereiten
        snprintf(device_ip_str, sizeof(device_ip_str), "0.0.0.0");

        esp_matter_attr_val_t ip_val = esp_matter_char_str(device_ip_str, DEVICE_IP_MAX_LENGTH);
        
        // ✅ Attribut mit char_str UND fixed size erstellen
        attribute_t* ip_attr = attribute::create(
            custom_cluster, 
            ATTR_ID_DEVICE_IP, 
            ATTRIBUTE_FLAG_NONE,
            ip_val
        );

        if (ip_attr) {
            ESP_LOGI(TAG, "✓ Device IP attribute initialized: %s (buffer: %d bytes)", 
                     device_ip_str, DEVICE_IP_MAX_LENGTH);
            
            esp_matter_attr_val_t stored_val;
            esp_err_t err = attribute::get_val(ip_attr, &stored_val);
            
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "  → Stored type: %d", stored_val.type);
                if (stored_val.type == ESP_MATTER_VAL_TYPE_CHAR_STRING) {
                    ESP_LOGI(TAG, "  → Buffer size: %zu bytes", stored_val.val.a.s);
                    ESP_LOGI(TAG, "  → String: %s", (char*)stored_val.val.a.b);
                }
            }
        }

        command::create(custom_cluster, CMD_ID_START_CALIBRATION, 
                    COMMAND_FLAG_ACCEPTED, app_command_cb);
        
        ESP_LOGI(TAG, "Custom cluster 0x%04X created", CLUSTER_ID_ROLLERSHUTTER_CONFIG);
    }

    // ========================================================================
    // Contact Sensor & Power Source
    // ========================================================================
    
    // Contact Sensor Status laden
    matterPref.begin("matter", true);
    contact_sensor_matter_enabled = matterPref.getBool("cs_matter_en", false);
    bool cs_was_active = matterPref.getBool("cs_active", false);
    bool ps_was_active = matterPref.getBool("ps_active", false);
    matterPref.end();

    ESP_LOGI(TAG, "Contact Sensor Matter Status:");
    ESP_LOGI(TAG, "  User Enabled: %s", contact_sensor_matter_enabled ? "YES" : "NO");
    ESP_LOGI(TAG, "  Contact Sensor was active: %s", cs_was_active ? "YES" : "NO");
    ESP_LOGI(TAG, "  Power Source was active: %s", ps_was_active ? "YES" : "NO");

    // ✅ Endpoints JETZT erstellen wenn User enabled (unabhängig von BLE Pairing!)
    if (contact_sensor_matter_enabled && (cs_was_active || ps_was_active)) {
        ESP_LOGI(TAG, "→ Creating Contact Sensor endpoints NOW (before Matter start)...");
        
        if (cs_was_active) {
            createContactSensorEndpoint(node);
        }
        if (cs_was_active || ps_was_active) {
            createPowerSourceEndpoint(node);
        }
    }

    // Scenes Management Cluster
    //cluster::scenes_management::config_t scenes_config;
    //cluster::scenes_management::create(window_covering_endpoint_id, &scenes_config, CLUSTER_FLAG_SERVER);

    // ========================================================================
    // WiFi
    // ========================================================================
    
    #if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, password);
        
        Serial.print("Connecting to WiFi");
        uint8_t wifi_timeout = 0;
        while (WiFi.status() != WL_CONNECTED && wifi_timeout < 60) {
            delay(500);
            Serial.print(".");
            wifi_timeout++;
            esp_task_wdt_reset();
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nWiFi connected");
            snprintf(device_ip_str, sizeof(device_ip_str), "%s", WiFi.localIP().toString().c_str());
            
            // ✅ PRÜFE HIER - Verwendest du die manuelle Struktur?
            cluster_t* custom_cluster_local = cluster::get(window_covering_endpoint_id, 
                                                        CLUSTER_ID_ROLLERSHUTTER_CONFIG);
            
            if (custom_cluster_local) {
                attribute_t* ip_attr = attribute::get(custom_cluster_local, ATTR_ID_DEVICE_IP);
                
                if (ip_attr) {
                    // ✅ Diese Zeile sollte SO aussehen:
                    esp_matter_attr_val_t ip_val = esp_matter_char_str(device_ip_str, DEVICE_IP_MAX_LENGTH);
                    attribute::set_val(ip_attr, &ip_val);
                    
                    ESP_LOGI(TAG, "✓ Device IP updated: %s", device_ip_str);
                }
            }
        }

    #endif

    // ========================================================================
    // Matter Stack
    // ========================================================================
    
    ESP_ERROR_CHECK(esp_matter::start(nullptr));
    ESP_LOGI(TAG, "Matter stack started");

    bool commissioned = Matter.isDeviceCommissioned();
    bool hasFabrics = (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0);

    ESP_LOGI(TAG, "=== Matter Status Debug ===");
    ESP_LOGI(TAG, "Matter.isDeviceCommissioned() = %s", commissioned ? "true" : "false");
    ESP_LOGI(TAG, "Fabric Count = %d", chip::Server::GetInstance().GetFabricTable().FabricCount());

    String qrUrl = Matter.getOnboardingQRCodeUrl();
    String pairingCode = Matter.getManualPairingCode();

    ESP_LOGI(TAG, "QR URL length: %d", qrUrl.length());
    ESP_LOGI(TAG, "Pairing Code length: %d", pairingCode.length());

    if (qrUrl.length() > 0) {
        Serial.printf("\n=== Matter Pairing Information ===\n");
        Serial.printf("QR Code URL: %s\n", qrUrl.c_str());
        Serial.printf("Manual Pairing Code: %s\n", pairingCode.c_str());
        Serial.println("===================================\n");
    } else {
        ESP_LOGE(TAG, "QR Code and Pairing Code are EMPTY!");
        ESP_LOGE(TAG, "This should not happen. Checking Matter configuration...");
    }

    if (!hasFabrics) {
        commissioned = false;
    }

    if (!commissioned) {
        Serial.println("\n=== Matter Device NOT Commissioned ===");
        Serial.printf("Manual pairing code: %s\n", Matter.getManualPairingCode().c_str());
        Serial.printf("QR code URL: %s\n", Matter.getOnboardingQRCodeUrl().c_str());
        Serial.println("=====================================\n");
    } else {
        Serial.printf("\n=== Matter Device Commissioned (%d Fabrics) ===\n", 
                    chip::Server::GetInstance().GetFabricTable().FabricCount());
    }

    if (commissioned) {
        ESP_LOGI(TAG, "Already commissioned. Initializing hardware...");
        ((RollerShutter*)shutter_handle)->initHardware();
        hardware_initialized = true;
    }

    // ════════════════════════════════════════════════════════════════════
    // Shelly BLE Manager (LAZY INIT - BLE bleibt aus!)
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  INITIALIZING SHELLY BLE MANAGER  ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    bleManager = new ShellyBLEManager();
    bleManager->setSensorDataCallback(onBLESensorData);
    
    if (!bleManager->begin()) {
        ESP_LOGE(TAG, "✗ Shelly BLE Manager init failed");
        delete bleManager;
        bleManager = nullptr;
    } else {
        ESP_LOGI(TAG, "✓ Shelly BLE Manager initialized (lazy mode)");
        ESP_LOGI(TAG, "  BLE is NOT started yet");
        ESP_LOGI(TAG, "  Will start after boot if needed");
    }
    
    ESP_LOGI(TAG, "");
    
    // ══════════════════════════════════════════════════════════════════════
    // DELAYED BLE AUTO-START TASK (with manual init fallback)
    // ══════════════════════════════════════════════════════════════════════

    struct AutoStartParams {
        ShellyBLEManager** bleManagerPtr;
    };

    AutoStartParams* params = new AutoStartParams{&bleManager};

    xTaskCreate([](void* param) {
        const char* TASK_TAG = "BLEAutoStart";
        AutoStartParams* p = (AutoStartParams*)param;
        
        ESP_LOGI(TASK_TAG, "");
        ESP_LOGI(TASK_TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TASK_TAG, "║  DELAYED BLE AUTO-START CHECK     ║");
        ESP_LOGI(TASK_TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TASK_TAG, "");
        
        // ✅ STRATEGY: Wait for Matter, then try manual init if needed
        ESP_LOGI(TASK_TAG, "→ Waiting 8 seconds for Matter to settle...");
        ESP_LOGI(TASK_TAG, "");
        
        vTaskDelay(pdMS_TO_TICKS(8000));
        
        // ══════════════════════════════════════════════════════════════════
        // CHECK: Did Matter initialize NimBLE?
        // ══════════════════════════════════════════════════════════════════
        
        ESP_LOGI(TASK_TAG, "→ Checking NimBLE status...");
        
        bool nimble_ready = NimBLEDevice::isInitialized();
        
        if (nimble_ready) {
            ESP_LOGI(TASK_TAG, "✓ NimBLE was initialized by Matter");
        } else {
            ESP_LOGW(TASK_TAG, "⚠ NimBLE NOT initialized by Matter!");
            ESP_LOGI(TASK_TAG, "  → This is expected - Matter uses isolated BLE");
            ESP_LOGI(TASK_TAG, "  → We will initialize NimBLE ourselves");
        }
        
        ESP_LOGI(TASK_TAG, "");
        
        // ══════════════════════════════════════════════════════════════════
        // CHECK: Should we start BLE?
        // ══════════════════════════════════════════════════════════════════
        
        bool matter_commissioned = Matter.isDeviceCommissioned();
        uint8_t fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
        bool has_shelly = ShellyBLEManager::hasAnyPairedDevice();
        
        ESP_LOGI(TASK_TAG, "System Status:");
        ESP_LOGI(TASK_TAG, "  Matter Commissioned: %s", matter_commissioned ? "YES" : "NO");
        ESP_LOGI(TASK_TAG, "  Fabrics: %d", fabric_count);
        ESP_LOGI(TASK_TAG, "  Shelly Paired: %s", has_shelly ? "YES" : "NO");
        ESP_LOGI(TASK_TAG, "");
        
        ShellyBLEManager* ble = *(p->bleManagerPtr);
        
        if (!ble) {
            ESP_LOGE(TASK_TAG, "✗ BLE Manager is NULL - cannot start");
            delete p;
            vTaskDelete(NULL);
            return;
        }
        
        // ══════════════════════════════════════════════════════════════════
        // DECISION: Start BLE?
        // ══════════════════════════════════════════════════════════════════
        
        if (has_shelly) {
            ESP_LOGI(TASK_TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TASK_TAG, "║  ✅ AUTO-START BLE                ║");
            ESP_LOGI(TASK_TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TASK_TAG, "");
            ESP_LOGI(TASK_TAG, "Reason: Paired Shelly device found");
            ESP_LOGI(TASK_TAG, "");
            
            // ✅ Start BLE (will do manual init if needed)
            if (ble->ensureBLEStarted()) {
                ESP_LOGI(TASK_TAG, "✓ BLE started successfully");
                ESP_LOGI(TASK_TAG, "");
                
                // Check Continuous Scan Preference
                Preferences prefs;
                prefs.begin("ShellyBLE", true);
                bool should_scan = prefs.getBool("continuous_scan", true);
                prefs.end();
                
                if (should_scan) {
                    ESP_LOGI(TASK_TAG, "→ Starting Continuous Scan...");
                    ble->startContinuousScan();
                    ESP_LOGI(TASK_TAG, "✓ Continuous Scan active");
                } else {
                    ESP_LOGI(TASK_TAG, "ℹ Continuous Scan was disabled");
                    ESP_LOGI(TASK_TAG, "  NOT starting automatically");
                }
                
            } else {
                ESP_LOGE(TASK_TAG, "✗ Failed to start BLE");
                ESP_LOGE(TASK_TAG, "  Device will work WITHOUT BLE sensor");
            }
            
        } else {
            ESP_LOGI(TASK_TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TASK_TAG, "║  ℹ BLE STAYS IDLE                 ║");
            ESP_LOGI(TASK_TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TASK_TAG, "");
            ESP_LOGI(TASK_TAG, "Reason: No paired Shelly device");
            ESP_LOGI(TASK_TAG, "BLE scanner not started (saves resources)");
            ESP_LOGI(TASK_TAG, "");
        }
        
        ESP_LOGI(TASK_TAG, "");
        ESP_LOGI(TASK_TAG, "✓ Auto-start check complete");
        ESP_LOGI(TASK_TAG, "");
        
        delete p;
        vTaskDelete(NULL);
        
    }, "ble_autostart", 6144, params, 1, NULL);


    // Contact Sensor Status laden
    matterPref.begin("matter", true);
    contact_sensor_matter_enabled = matterPref.getBool("cs_matter_en", false);
    matterPref.end();

    ESP_LOGI(TAG, "Contact Sensor Matter Status:");
    ESP_LOGI(TAG, "  User Enabled: %s", contact_sensor_matter_enabled ? "YES" : "NO");
    ESP_LOGI(TAG, "  Contact Sensor was active: %s", cs_was_active ? "YES" : "NO");
    ESP_LOGI(TAG, "  Power Source was active: %s", ps_was_active ? "YES" : "NO");

    // ✅ NEUE LOGIK: Power Source sollte IMMER mit Contact Sensor erstellt werden
    if (contact_sensor_matter_enabled && cs_was_active) {
        ps_was_active = true;  // ← Force enable!
        ESP_LOGI(TAG, "  → Power Source will be created with Contact Sensor");
    }

    // Endpoints wiederherstellen wenn möglich
    if (contact_sensor_matter_enabled && 
        bleManager->isPaired() && 
        Matter.isDeviceCommissioned()) {
        
        ESP_LOGI(TAG, "→ Restoring Contact Sensor endpoints...");
        node_t* node = node::get();
        if (node) {
            if (cs_was_active) {
                createContactSensorEndpoint(node);
            }
            // ✅ Power Source IMMER wenn Contact Sensor aktiv ist
            if (cs_was_active || ps_was_active) {
                createPowerSourceEndpoint(node);
            }
        }
    } else {
        ESP_LOGI(TAG, "ℹ Contact Sensor endpoints will be created when:");
        ESP_LOGI(TAG, "  • Device is commissioned: %s", 
                Matter.isDeviceCommissioned() ? "✓" : "✗");
        ESP_LOGI(TAG, "  • BLE sensor paired: %s", 
                (bleManager && bleManager->isPaired()) ? "✓" : "✗");
    }

                 

    // ========================================================================
    // Web UI
    // ========================================================================

    webUI = new WebUIHandler(shutter_handle, bleManager);
    webUI->setRemoveContactSensorCallback(removeContactSensorEndpoint);
    webUI->begin();
    ESP_LOGI(TAG, "Web UI started");

    ESP_LOGI(TAG, "=== System Ready ===");
}

// ============================================================================
// Loop
// ============================================================================

void loop() {
    esp_task_wdt_reset();

    // Commissioning Check
    static bool was_commissioned = false;
    bool has_fabrics = (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0);
    bool is_commissioned = Matter.isDeviceCommissioned() && has_fabrics;
    
    if (!was_commissioned && is_commissioned && !hardware_initialized) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "█████████████████████████████████████████████████████████");
        ESP_LOGI(TAG, "█                                                       █");
        ESP_LOGI(TAG, "█           ✓ COMMISSIONING COMPLETE!                  █");
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

    // Shutter Control Loop
    if (is_commissioned && hardware_initialized) {
        shutter_driver_loop(shutter_handle);

        // Position Update
        if (shutter_driver_is_position_changed(shutter_handle)) {
            uint8_t percent = shutter_driver_get_current_percent(shutter_handle);
            uint16_t pos_100ths = percent * 100;
            
            ESP_LOGD(TAG, "Position changed: %d%% (%d/10000)", percent, pos_100ths);
            
            esp_matter_attr_val_t val = esp_matter_uint16(pos_100ths);
            attribute::update(window_covering_endpoint_id, chip::app::Clusters::WindowCovering::Id,
                             chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id, &val);
            attribute::update(window_covering_endpoint_id, chip::app::Clusters::WindowCovering::Id,
                             chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id, &val);
        }
    }

    // IP Address Update
    static uint32_t last_ip_check = 0;
    if (WiFi.status() == WL_CONNECTED && millis() - last_ip_check >= 30000) {
        last_ip_check = millis();
        String new_ip = WiFi.localIP().toString();
        
        if (strcmp(device_ip_str, new_ip.c_str()) != 0) {
            ESP_LOGI(TAG, "IP changed: %s → %s", device_ip_str, new_ip.c_str());
            snprintf(device_ip_str, sizeof(device_ip_str), "%s", new_ip.c_str());
            
            // ✅ LÖSUNG: Verwende set_val() statt update()
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


    // Web UI Updates
    static uint32_t last_web = 0;
    if (millis() - last_web >= 500) {
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

    // WebSocket Cleanup every 3 Seconds
    static uint32_t last_ws_cleanup = 0;
    if (millis() - last_ws_cleanup >= 3000) {
        last_ws_cleanup = millis();
        if (webUI) {
            webUI->cleanup_idle_clients();
        }
    }
    
    // Memory-Status every 30 Seconds
    static uint32_t last_mem_check = 0;
    if (millis() - last_mem_check >= 300000) {  // ← 2 Minuten
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

    // ========================================================================
    // Keep-Alive: Regelmäßig Attribute updaten (alle 30 Sekunden)
    // ========================================================================

    static uint32_t last_keepalive = 0;
    if (millis() - last_keepalive >= 30000) {
        last_keepalive = millis();
        
        bool updates_sent = false;
        
        // Contact Sensor Keep-Alive
        if (contact_sensor_endpoint_active && contact_sensor_endpoint_id != 0) {
            ShellyBLESensorData data;
            if (bleManager && bleManager->getSensorData(data) && data.dataValid) {
                // ✅ Contact State
                esp_matter_attr_val_t contact_val = esp_matter_bool(!data.windowOpen);
                esp_err_t ret = attribute::update(contact_sensor_endpoint_id,
                                                chip::app::Clusters::BooleanState::Id,
                                                chip::app::Clusters::BooleanState::Attributes::StateValue::Id,
                                                &contact_val);
                
                if (ret == ESP_OK) {
                    updates_sent = true;
                }
            }
        }
        
        // Power Source Keep-Alive
        if (power_source_endpoint_active && power_source_endpoint_id != 0) {
            ShellyBLESensorData data;
            if (bleManager && bleManager->getSensorData(data) && data.dataValid) {
                // ✅ Battery Percentage
                esp_matter_attr_val_t battery_val = esp_matter_nullable_uint8(data.battery * 2);
                esp_err_t ret = attribute::update(power_source_endpoint_id,
                                                chip::app::Clusters::PowerSource::Id,
                                                chip::app::Clusters::PowerSource::Attributes::BatPercentRemaining::Id,
                                                &battery_val);
                
                if (ret == ESP_OK) {
                    updates_sent = true;
                }
            }
        }
        
        if (updates_sent) {
            ESP_LOGD(TAG, "✓ Keep-alive updates sent to controller");
        }
    }



    /*
    // Periodic Status Report (every 5 minutes)
    static uint32_t last_status_report = 0;
    if (millis() - last_status_report >= 300000) {  // 5 minutes
        last_status_report = millis();
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "PERIODIC STATUS REPORT");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Uptime: %llu seconds", esp_timer_get_time() / 1000000);
        ESP_LOGI(TAG, "Free heap: %u bytes (%.1f KB)", 
                 esp_get_free_heap_size(), esp_get_free_heap_size() / 1024.0f);
        ESP_LOGI(TAG, "Min free heap: %u bytes (%.1f KB)", 
                 esp_get_minimum_free_heap_size(), esp_get_minimum_free_heap_size() / 1024.0f);
        
        if (WiFi.status() == WL_CONNECTED) {
            ESP_LOGI(TAG, "WiFi: Connected (%d dBm)", WiFi.RSSI());
                        ESP_LOGI(TAG, "IP: %s", device_ip_str);
        } else {
            ESP_LOGW(TAG, "WiFi: Disconnected");
        }
        
        ESP_LOGI(TAG, "Matter: %s (%d fabrics)", 
                 is_commissioned ? "Commissioned" : "Not commissioned",
                 chip::Server::GetInstance().GetFabricTable().FabricCount());
        
        ESP_LOGI(TAG, "Shutter:");
        ESP_LOGI(TAG, "  Position: %d%%", shutter_driver_get_current_percent(shutter_handle));
        ESP_LOGI(TAG, "  Calibrated: %s", shutter_driver_is_calibrated(shutter_handle) ? "Yes" : "No");
        ESP_LOGI(TAG, "  Direction: %s", 
                 shutter_driver_get_direction_inverted(shutter_handle) ? "Inverted" : "Normal");
        ESP_LOGI(TAG, "  State: %s", 
                 shutter_driver_get_current_state(shutter_handle) == RollerShutter::State::MOVING_UP ? "Moving UP" :
                 shutter_driver_get_current_state(shutter_handle) == RollerShutter::State::MOVING_DOWN ? "Moving DOWN" :
                 shutter_driver_get_current_state(shutter_handle) == RollerShutter::State::CALIBRATING_UP ? "Calibrating UP" :
                 shutter_driver_get_current_state(shutter_handle) == RollerShutter::State::CALIBRATING_DOWN ? "Calibrating DOWN" :
                 "Stopped");
        
        if (bleManager) {
            if (bleManager->isPaired()) {
                PairedShellyDevice paired = bleManager->getPairedDevice();
                ESP_LOGI(TAG, "BLE Sensor: Paired");
                ESP_LOGI(TAG, "  Device: %s (%s)", paired.name.c_str(), paired.address.c_str());
                
                ShellyBLESensorData data;
                if (bleManager->getSensorData(data)) {
                    uint32_t seconds_ago = (millis() - data.lastUpdate) / 1000;
                    ESP_LOGI(TAG, "  Contact: %s", data.windowOpen ? "OPEN" : "CLOSED");
                    ESP_LOGI(TAG, "  Battery: %d%%", data.battery);
                    ESP_LOGI(TAG, "  Illuminance: %u lux", data.illuminance);
                    ESP_LOGI(TAG, "  RSSI: %d dBm", data.rssi);
                    ESP_LOGI(TAG, "  Last update: %u seconds ago", seconds_ago);
                } else {
                    ESP_LOGW(TAG, "  No sensor data available yet");
                }
            } else {
                ESP_LOGI(TAG, "BLE Sensor: Not paired");
            }
        } else {
            ESP_LOGW(TAG, "BLE Manager: Not initialized");
        }
        
        if (webUI) {
            ESP_LOGI(TAG, "Web UI: Active (clients: %d)", webUI->get_client_count());
        }
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "");   
    }
        */

    delay(1);
}

// ============================================================================
// Callbacks
// ============================================================================

static esp_err_t app_attribute_update_cb(callback_type_t type, uint16_t endpoint_id, 
                                        uint32_t cluster_id, uint32_t attribute_id, 
                                        esp_matter_attr_val_t *val, void *priv) {
    if (type != PRE_UPDATE || endpoint_id != window_covering_endpoint_id) {
        return ESP_OK;
    }

    // Window Covering Commands
    if (cluster_id == chip::app::Clusters::WindowCovering::Id) {
                if (attribute_id == chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id) {
            uint8_t percent = val->val.u16 / 100;
            ESP_LOGI(TAG, "Matter command: Move to %d%%", percent);
            shutter_driver_go_to_lift_percent(shutter_handle, percent);
        }
    }
    
    // Custom Cluster - Direction Inverted
    if (cluster_id == CLUSTER_ID_ROLLERSHUTTER_CONFIG && 
        attribute_id == ATTR_ID_DIRECTION_INVERTED) {
        ESP_LOGI(TAG, "Matter: Set direction inverted = %s", val->val.b ? "true" : "false");
        shutter_driver_set_direction(shutter_handle, val->val.b);
    }
    
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
    
    // Custom Cluster - Calibration Command
    if (path.mClusterId == CLUSTER_ID_ROLLERSHUTTER_CONFIG && 
        path.mCommandId == CMD_ID_START_CALIBRATION) {
        ESP_LOGI(TAG, "│ → Custom: START_CALIBRATION");
        ESP_LOGI(TAG, "└─────────────────────────────────");
        shutter_driver_start_calibration(shutter_handle);
        return ESP_OK;
    }
    
    // Window Covering Commands
    if (path.mClusterId == chip::app::Clusters::WindowCovering::Id) {
        ESP_LOGI(TAG, "│ → Window Covering Command");
        
        switch (path.mCommandId) {
            case chip::app::Clusters::WindowCovering::Commands::UpOrOpen::Id:
                ESP_LOGI(TAG, "│   Type: UpOrOpen");
                ESP_LOGI(TAG, "└─────────────────────────────────");
                shutter_driver_go_to_lift_percent(shutter_handle, 0);
                break;
                
            case chip::app::Clusters::WindowCovering::Commands::DownOrClose::Id:
                ESP_LOGI(TAG, "│   Type: DownOrClose");
                ESP_LOGI(TAG, "└─────────────────────────────────");
                shutter_driver_go_to_lift_percent(shutter_handle, 100);
                break;
                
            case chip::app::Clusters::WindowCovering::Commands::StopMotion::Id:
                ESP_LOGI(TAG, "│   Type: StopMotion");
                ESP_LOGI(TAG, "└─────────────────────────────────");
                shutter_driver_stop_motion(shutter_handle);
                break;
                
            case chip::app::Clusters::WindowCovering::Commands::GoToLiftPercentage::Id: {
                chip::app::Clusters::WindowCovering::Commands::GoToLiftPercentage::DecodableType cmd;
                if (chip::app::DataModel::Decode(reader, cmd) == CHIP_NO_ERROR) {
                    uint8_t percent = cmd.liftPercent100thsValue / 100;
                    ESP_LOGI(TAG, "│   Type: GoToLiftPercentage");
                    ESP_LOGI(TAG, "│   Target: %d%%", percent);
                    ESP_LOGI(TAG, "└─────────────────────────────────");
                    shutter_driver_go_to_lift_percent(shutter_handle, percent);
                } else {
                    ESP_LOGE(TAG, "│   ✗ Failed to decode command payload");
                    ESP_LOGI(TAG, "└─────────────────────────────────");
                }
                break;
            }
            
            default:
                ESP_LOGW(TAG, "│   ⚠ Unknown command ID: 0x%04X", path.mCommandId);
                ESP_LOGI(TAG, "└─────────────────────────────────");
                break;
        }
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "│ ⚠ Unsupported cluster: 0x%04X", path.mClusterId);
    ESP_LOGI(TAG, "└─────────────────────────────────");
    
    return ESP_ERR_NOT_SUPPORTED;
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