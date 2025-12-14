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

// ============================================================================
// Forward Declarations
// ============================================================================

static esp_err_t app_attribute_update_cb(callback_type_t type, uint16_t endpoint_id, 
                                        uint32_t cluster_id, uint32_t attribute_id, 
                                        esp_matter_attr_val_t *val, void *priv_data);
static esp_err_t app_command_cb(const ConcreteCommandPath &command_path, 
                                TLV::TLVReader &tlv_reader, void *priv_data);

// ============================================================================
// BLE Callback
// ============================================================================

void onWindowStateChanged(const String& address, bool isOpen) {
    ESP_LOGI(TAG, "BLE Sensor %s: Window is %s", 
             address.c_str(), isOpen ? "OPEN" : "CLOSED");
    
    shutter_driver_set_window_state(shutter_handle, isOpen);
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);
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

    // Custom Cluster
    cluster_t *custom_cluster = cluster::create(ep, CLUSTER_ID_ROLLERSHUTTER_CONFIG, 
                                                CLUSTER_FLAG_SERVER);
    if (custom_cluster) {
        bool inverted = shutter_driver_get_direction_inverted(shutter_handle);
        attribute::create(custom_cluster, ATTR_ID_DIRECTION_INVERTED, 
                        ATTRIBUTE_FLAG_WRITABLE, esp_matter_bool(inverted));
        
        #define DEVICE_IP_MAX_LENGTH 16
        
        attribute_t* ip_attr = attribute::create(custom_cluster, ATTR_ID_DEVICE_IP, 
                                                ATTRIBUTE_FLAG_NONE, 
                                                esp_matter_invalid(nullptr));
        
        if (ip_attr) {
            snprintf(device_ip_str, sizeof(device_ip_str), "0.0.0.0");
            
            esp_matter_attr_val_t ip_val = esp_matter_char_str(device_ip_str, DEVICE_IP_MAX_LENGTH);
            attribute::set_val(ip_attr, &ip_val);
            
            ESP_LOGI(TAG, "Device IP attribute initialized: %s", device_ip_str);
        }

        command::create(custom_cluster, CMD_ID_START_CALIBRATION, 
                    COMMAND_FLAG_ACCEPTED, app_command_cb);
        
        ESP_LOGI(TAG, "Custom cluster 0x%04X created", CLUSTER_ID_ROLLERSHUTTER_CONFIG);
    }

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
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nWiFi connected");
            snprintf(device_ip_str, sizeof(device_ip_str), "%s", WiFi.localIP().toString().c_str());
            
            esp_matter_attr_val_t ip_val = esp_matter_char_str(device_ip_str, strlen(device_ip_str));
            attribute::update(window_covering_endpoint_id, CLUSTER_ID_ROLLERSHUTTER_CONFIG, 
                            ATTR_ID_DEVICE_IP, &ip_val);
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

    // BLE Manager
    bleManager = new ShellyBLEManager();
    if (bleManager->begin()) {
        ESP_LOGI(TAG, "Shelly BLE Manager initialized");
        bleManager->setWindowStateCallback(onWindowStateChanged);
    } else {
        delete bleManager;
        bleManager = nullptr;
    }

    // Web UI
    webUI = new WebUIHandler(shutter_handle, bleManager);
    webUI->begin();
    ESP_LOGI(TAG, "Web UI started");

    ESP_LOGI(TAG, "=== System Ready ===");
}

// ============================================================================
// Loop
// ============================================================================

void loop() {
    esp_task_wdt_reset();

    static bool was_commissioned = false;
    bool has_fabrics = (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0);
    bool is_commissioned = Matter.isDeviceCommissioned() && has_fabrics;
    
    if (!was_commissioned && is_commissioned) {
        ESP_LOGI(TAG, "=== Commissioning Complete! ===");
        ESP_LOGI(TAG, "Fabrics: %d", chip::Server::GetInstance().GetFabricTable().FabricCount());
        ESP_LOGI(TAG, "Initializing hardware...");
        
        ((RollerShutter*)shutter_handle)->initHardware();
        hardware_initialized = true;
        
        Serial.println("\n✓ Matter Node commissioned and connected!");
    }
    was_commissioned = is_commissioned;

    if (is_commissioned && hardware_initialized) {
        shutter_driver_loop(shutter_handle);

        if (shutter_driver_is_position_changed(shutter_handle)) {
            uint8_t percent = shutter_driver_get_current_percent(shutter_handle);
            uint16_t pos_100ths = percent * 100;
            
            esp_matter_attr_val_t val = esp_matter_uint16(pos_100ths);
            attribute::update(window_covering_endpoint_id, chip::app::Clusters::WindowCovering::Id,
                             chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id, &val);
            attribute::update(window_covering_endpoint_id, chip::app::Clusters::WindowCovering::Id,
                             chip::app::Clusters::WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id, &val);
            
            ESP_LOGD(TAG, "Position updated: %d%%", percent);
        }
    }

    // IP Update
    static uint32_t last_ip_check = 0;
    if (WiFi.status() == WL_CONNECTED && millis() - last_ip_check >= 30000) {
        last_ip_check = millis();
        snprintf(device_ip_str, sizeof(device_ip_str), "%s", WiFi.localIP().toString().c_str());
        esp_matter_attr_val_t ip_val = esp_matter_char_str(device_ip_str, strlen(device_ip_str));
        attribute::update(window_covering_endpoint_id, CLUSTER_ID_ROLLERSHUTTER_CONFIG, 
                        ATTR_ID_DEVICE_IP, &ip_val);
    }

    // Web UI
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

    // BLE Manager
    if (bleManager) {
        bleManager->loop();
    }

    // Heap Monitor
    static uint32_t last_mem_check = 0;
    if (millis() - last_mem_check >= 30000) {
        last_mem_check = millis();
        ESP_LOGI(TAG, "Free heap: %u bytes", esp_get_free_heap_size());
    }

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
    // Custom Cluster - Calibration Command
    if (path.mClusterId == CLUSTER_ID_ROLLERSHUTTER_CONFIG && 
        path.mCommandId == CMD_ID_START_CALIBRATION) {
        ESP_LOGI(TAG, "Calibration command received via Matter");
        shutter_driver_start_calibration(shutter_handle);
        return ESP_OK;
    }
    
    // Window Covering Commands
    if (path.mClusterId == chip::app::Clusters::WindowCovering::Id) {
        switch (path.mCommandId) {
            case chip::app::Clusters::WindowCovering::Commands::UpOrOpen::Id:
                ESP_LOGI(TAG, "Matter: UpOrOpen command");
                shutter_driver_go_to_lift_percent(shutter_handle, 0);
                break;
                
            case chip::app::Clusters::WindowCovering::Commands::DownOrClose::Id:
                ESP_LOGI(TAG, "Matter: DownOrClose command");
                shutter_driver_go_to_lift_percent(shutter_handle, 100);
                break;
                
            case chip::app::Clusters::WindowCovering::Commands::StopMotion::Id:
                ESP_LOGI(TAG, "Matter: StopMotion command");
                shutter_driver_stop_motion(shutter_handle);
                break;
                
            case chip::app::Clusters::WindowCovering::Commands::GoToLiftPercentage::Id: {
                // read command payload
                chip::app::Clusters::WindowCovering::Commands::GoToLiftPercentage::DecodableType cmd;
                if (chip::app::DataModel::Decode(reader, cmd) == CHIP_NO_ERROR) {
                    uint8_t percent = cmd.liftPercent100thsValue / 100;
                    ESP_LOGI(TAG, "Matter: GoToLiftPercentage %d%%", percent);
                    shutter_driver_go_to_lift_percent(shutter_handle, percent);
                }
                break;
            }
        }
        return ESP_OK;
    }
    
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

