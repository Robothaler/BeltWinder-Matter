#pragma once

#include <esp_http_server.h>
#include <vector>
#include "rollershutter_driver.h"
#include "shelly_ble_manager.h"

class WebUIHandler {
public:
    WebUIHandler(app_driver_handle_t h, ShellyBLEManager* ble);
    ~WebUIHandler();
    
    void begin();
    void broadcast_to_all_clients(const char* message);

private:
    app_driver_handle_t handle;
    ShellyBLEManager* bleManager;
    httpd_handle_t server;
    std::vector<int> active_clients;
    SemaphoreHandle_t client_mutex;
    
    void register_client(int fd);
    void unregister_client(int fd);
    
    // HTTP Handlers
    static esp_err_t root_handler(httpd_req_t *req);
    static esp_err_t ws_handler(httpd_req_t *req);
    
    // BLE Command Handlers
    void handle_ble_scan(httpd_req_t *req, int fd);
    void handle_ble_pair(httpd_req_t *req, int fd, const char* payload);
    void handle_ble_unpair(httpd_req_t *req, int fd, const char* payload);
    void handle_ble_status(httpd_req_t *req, int fd);
};
