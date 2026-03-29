// web_ui_handler.h

#ifndef WEB_UI_HANDLER_H
#define WEB_UI_HANDLER_H

#pragma once

#include <Arduino.h>
#include <esp_http_server.h>
#include <ESPmDNS.h>
#include "rollershutter_driver.h"
#include "shelly_ble_manager.h"
#include <vector>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

typedef void (*endpoint_callback_t)();

// ════════════════════════════════════════════════════════════════════════
// MATTER START RESULT STRUCTURE
// ════════════════════════════════════════════════════════════════════════

struct MatterStartResult {
    bool success;
    bool already_commissioned;
    bool reboot_triggered;
    String qr_url;
    String pairing_code;
    String error_message;
};

MatterStartResult initializeAndStartMatter();

// ════════════════════════════════════════════════════════════════════════
// DEVICE DISCOVERY
// ════════════════════════════════════════════════════════════════════════

#define MAX_DISCOVERED_DEVICES 10

struct DiscoveredDevice {
    char hostname[64];
    char ip[16];
    char room[32];
    char type[16];
    int8_t rssi;
    bool valid;
};

// ════════════════════════════════════════════════════════════════════════
// WEB UI HANDLER CLASS
// ════════════════════════════════════════════════════════════════════════

class WebUIHandler {
public:
    WebUIHandler(app_driver_handle_t h, ShellyBLEManager* ble);
    ~WebUIHandler();
    
    void begin();
    void broadcast_to_all_clients(const char* message);
    static esp_err_t handle_start_matter(httpd_req_t *req);
    static esp_err_t handle_matter_status(httpd_req_t *req);
    
    // Client Management
    void disconnect_all_clients();
    void cleanup_idle_clients();
    void remove_client(int fd);
    int get_client_count() const { return active_clients.size(); }

    void setRemoveContactSensorCallback(endpoint_callback_t cb) {
        remove_contact_sensor_callback = cb;
    }

    void broadcastBLEStateChange(ShellyBLEManager::DeviceState oldState,
                                  ShellyBLEManager::DeviceState newState);
    // Broadcast full ble_status to ALL connected WebSocket clients.
    // Call after any BLE state change (pair, unpair, encryption, scan state).
    void broadcastBLEStatus();

    void broadcastSensorDataUpdate(const String& address,
                                    const ShellyBLESensorData& data);
    void sendModalClose(int fd, const char* modal_id);
    void logMemoryStats(const char* location);

    static esp_err_t drift_stats_handler(httpd_req_t *req);
    static esp_err_t drift_reset_handler(httpd_req_t *req);
    
    static int discoverDevices(DiscoveredDevice* devices, int max_devices);
    void broadcastDiscoveredDevices();

private:
    app_driver_handle_t handle;
    ShellyBLEManager* bleManager;
    httpd_handle_t server;
    SemaphoreHandle_t client_mutex;
    
    struct ClientInfo {
        int fd;
        uint32_t last_activity;
    };

    endpoint_callback_t remove_contact_sensor_callback = nullptr;
    
    std::vector<ClientInfo> active_clients;
    
    static const int MAX_CLIENTS = 3;
    static const uint32_t WS_TIMEOUT_MS = 60000;
    
    void register_client(int fd);
    void unregister_client(int fd);

    bool check_basic_auth(httpd_req_t *req);
    
    static esp_err_t root_handler(httpd_req_t *req);
    static esp_err_t ws_handler(httpd_req_t *req);

    struct WSMessageBuffer {
        char* buffer;
        size_t size;
        
        WSMessageBuffer(const char* msg) {
            size = strlen(msg) + 1;
            buffer = (char*)malloc(size);
            if (buffer) {
                strcpy(buffer, msg);
            }
        }
        
        ~WSMessageBuffer() {
            if (buffer) {
                free(buffer);
            }
        }
        
        WSMessageBuffer(const WSMessageBuffer&) = delete;
        WSMessageBuffer& operator=(const WSMessageBuffer&) = delete;
    };
};

#endif // WEB_UI_HANDLER_H
