#pragma once

#include <Arduino.h>
#include <esp_http_server.h>
#include "rollershutter_driver.h"
#include "shelly_ble_manager.h"
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

typedef void (*endpoint_callback_t)();

class WebUIHandler {
public:
    WebUIHandler(app_driver_handle_t h, ShellyBLEManager* ble);
    ~WebUIHandler();
    
    void begin();
    void broadcast_to_all_clients(const char* message);
    
    // ✓ Client Management
    void cleanup_idle_clients();
    void remove_client(int fd);
    int get_client_count() const { return active_clients.size(); }

    void setRemoveContactSensorCallback(endpoint_callback_t cb) {
        remove_contact_sensor_callback = cb;
    }

    void broadcastBLEStateChange(ShellyBLEManager::DeviceState oldState, 
                                  ShellyBLEManager::DeviceState newState);
    
    void broadcastSensorDataUpdate(const String& address, 
                                    const ShellyBLESensorData& data);
    void sendModalClose(int fd, const char* modal_id);
    

private:
    app_driver_handle_t handle;
    ShellyBLEManager* bleManager;
    httpd_handle_t server;
    SemaphoreHandle_t client_mutex;
    
    // ✓ NEU: Client mit Timestamp
    struct ClientInfo {
        int fd;
        uint32_t last_activity;
    };

    endpoint_callback_t remove_contact_sensor_callback = nullptr;
    
    std::vector<ClientInfo> active_clients;
    
    static const int MAX_CLIENTS = 5;
    static const uint32_t WS_TIMEOUT_MS = 60000;  // 60 Sekunden
    
    void register_client(int fd);
    void unregister_client(int fd);
    
    static esp_err_t root_handler(httpd_req_t *req);
    static esp_err_t ws_handler(httpd_req_t *req);
};
