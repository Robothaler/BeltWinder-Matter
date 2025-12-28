#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <app/server/Server.h>
#include <Matter.h>
#include <Preferences.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "web_ui_handler.h"
#include "device_naming.h"

extern class DeviceNaming* deviceNaming;

static const char* TAG = "WebUI";

struct BLETaskParams {
    WebUIHandler* handler;
    int fd;
    String address;
    uint32_t passkey;
};

// ============================================================================
// GZIP Compressed HTML (Auto-Generated)
// ============================================================================

#ifdef USE_GZIP_UI
    #include "index_html_gz.h"
    #define SERVE_COMPRESSED_HTML 1
#else
    #warning "Building without GZIP compression - UI will be large!"
    
    // Fallback: Lade HTML aus separater Datei zur Compile-Time
    // (wird normalerweise nicht verwendet)
    static const char index_html[] PROGMEM = 
        "<!DOCTYPE html><html><body>"
        "<h1>UI not available - please enable USE_GZIP_UI</h1>"
        "</body></html>";
#endif

// ============================================================================
// Static Close Callback
// ============================================================================

static void ws_close_callback(httpd_handle_t hd, int sockfd) {
    ESP_LOGI("WebUI", "Socket %d closed by HTTP server", sockfd);
}

// ============================================================================
// WebUIHandler Implementation
// ============================================================================

WebUIHandler::WebUIHandler(app_driver_handle_t h, ShellyBLEManager* ble) 
        : handle(h), bleManager(ble), server(nullptr) {
    client_mutex = xSemaphoreCreateMutex();
    if (!client_mutex) {
        ESP_LOGE(TAG, "Failed to create client mutex");
    }
}

WebUIHandler::~WebUIHandler() {
    if (server) {
        httpd_stop(server);
    }
    if (client_mutex) {
        vSemaphoreDelete(client_mutex);
    }
}

void WebUIHandler::begin() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    
    // Socket-Handling optimieren
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 8192;
    cfg.ctrl_port = 32768;
    cfg.close_fn = nullptr;
    cfg.uri_match_fn = nullptr;
    cfg.keep_alive_enable = false;
    cfg.keep_alive_idle = 0;
    cfg.keep_alive_interval = 0;
    cfg.keep_alive_count = 0;

    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t ws = {
            .uri          = "/ws",
            .method       = HTTP_GET,
            .handler      = ws_handler,
            .user_ctx     = this,
            .is_websocket = true,
            .handle_ws_control_frames = true
        };
        httpd_register_uri_handler(server, &ws);
        
        ESP_LOGI(TAG, "HTTP server started");
        ESP_LOGI(TAG, "  Max open sockets: %d", cfg.max_open_sockets);
        ESP_LOGI(TAG, "  LRU purge: %s", cfg.lru_purge_enable ? "enabled" : "disabled");
        
        #ifdef SERVE_COMPRESSED_HTML
            ESP_LOGI(TAG, "  Web-UI: GZIP compressed (%d bytes)", index_html_gz_len);
        #else
            ESP_LOGI(TAG, "  Web-UI: Uncompressed (fallback mode)");
        #endif
        
        // BLE Callbacks registrieren
        if (bleManager) {
            ESP_LOGI(TAG, "═══════════════════════════════════");
            ESP_LOGI(TAG, "REGISTERING BLE CALLBACKS");
            
            bleManager->setStateChangeCallback([this](auto oldState, auto newState) {
                broadcastBLEStateChange(oldState, newState);
            });
             
            ESP_LOGI(TAG, "✓ BLE State Callback registered");
            ESP_LOGI(TAG, "ℹ Sensor Data forwarded via Main Loop");
            ESP_LOGI(TAG, "═══════════════════════════════════");
        }
    }
}

// ============================================================================
// HTTP Root Handler - Serves Web-UI
// ============================================================================

esp_err_t WebUIHandler::root_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Serving Web-UI to client: %s", 
             req->user_ctx ? "authenticated" : "anonymous");
    
    #ifdef SERVE_COMPRESSED_HTML
        // ====================================================================
        // Serve GZIP Compressed HTML
        // ====================================================================
        
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        
        // Cache für 1 Stunde (spart Bandwidth)
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
        
        // ETag für Cache-Validation (optional)
        char etag[32];
        snprintf(etag, sizeof(etag), "\"%08x\"", index_html_gz_len);
        httpd_resp_set_hdr(req, "ETag", etag);
        
        esp_err_t ret = httpd_resp_send(req, 
                                       (const char*)index_html_gz, 
                                       index_html_gz_len);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✓ Served compressed UI (%d bytes)", index_html_gz_len);
        } else {
            ESP_LOGE(TAG, "✗ Failed to serve UI: %s", esp_err_to_name(ret));
        }
        
        return ret;
        
    #else
        // ====================================================================
        // Fallback: Uncompressed HTML
        // ====================================================================
        
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
        
        ESP_LOGW(TAG, "⚠ Served uncompressed UI (USE_GZIP_UI not defined)");
        
        return ESP_OK;
    #endif
}

// ============================================================================
// WebSocket Handler
// ============================================================================

esp_err_t WebUIHandler::ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        WebUIHandler* self = (WebUIHandler*)req->user_ctx;
        int fd = httpd_req_to_sockfd(req);
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  WEBSOCKET CONNECTION ESTABLISHED ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "Socket FD: %d", fd);
        
        // IP-Adresse extrahieren
        struct sockaddr_in6 addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            char ip_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &addr.sin6_addr, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Client IP: %s", ip_str);
        }
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        
        self->register_client(fd);
        return ESP_OK;
    }

    WebUIHandler* self = (WebUIHandler*)req->user_ctx;
    int fd = httpd_req_to_sockfd(req);

    // Frame empfangen
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket: Failed to receive frame header");
        return ret;
    }
    
    // WICHTIG: Close Frame behandeln
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket: Close frame received");
        WebUIHandler* self = (WebUIHandler*)req->user_ctx;
        int fd = httpd_req_to_sockfd(req);
        self->unregister_client(fd);
        return ESP_OK;
    }
    
    // Ping/Pong frames ignorieren
    if (ws_pkt.type == HTTPD_WS_TYPE_PING || 
        ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        return ESP_OK;
    }
    
    // Nur Text-Frames verarbeiten
    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        ESP_LOGW(TAG, "WebSocket: Ignoring non-text frame type: %d", ws_pkt.type);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "WebSocket: Receiving text frame...");
    
    if (ws_pkt.len == 0 || ws_pkt.len > 512) {
        ESP_LOGE(TAG, "WebSocket: Invalid frame length: %d", ws_pkt.len);
        return ESP_ERR_INVALID_SIZE;
    }
    
    uint8_t* buf = (uint8_t*)malloc(ws_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "WebSocket: Failed to allocate buffer");
        return ESP_ERR_NO_MEM;
    }
    
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket: Failed to receive frame payload: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }
    
    buf[ws_pkt.len] = '\0';
    char* cmd = (char*)buf;

    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "COMMAND DEBUG");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Command: %s", cmd);
    ESP_LOGI(TAG, "Length: %d", strlen(cmd));

    // ========================================================================
    // Command Handling (ERWEITERT MIT DEBUG)
    // ========================================================================

    // Shutter Commands
    if (strcmp(cmd, "up") == 0) {
        ESP_LOGI(TAG, "→ Command: UP (move to 0%%)");
        esp_err_t result = shutter_driver_go_to_lift_percent(self->handle, 0);
        ESP_LOGI(TAG, "← Result: %s", result == ESP_OK ? "SUCCESS" : "FAILED");
    } 
    else if (strcmp(cmd, "down") == 0) {
        ESP_LOGI(TAG, "→ Command: DOWN (move to 100%%)");
        esp_err_t result = shutter_driver_go_to_lift_percent(self->handle, 100);
        ESP_LOGI(TAG, "← Result: %s", result == ESP_OK ? "SUCCESS" : "FAILED");
    }
    else if (strncmp(cmd, "pos:", 4) == 0) {
        int target_pos = atoi(cmd + 4);
        if (target_pos < 0) target_pos = 0;
        if (target_pos > 100) target_pos = 100;
        ESP_LOGI(TAG, "→ Command: SLIDER POSITION (move to %d%%)", target_pos);
        esp_err_t result = shutter_driver_go_to_lift_percent(self->handle, target_pos);
        ESP_LOGI(TAG, "← Result: %s", result == ESP_OK ? "SUCCESS" : "FAILED");
    }
    else if (strcmp(cmd, "stop") == 0) {
        ESP_LOGI(TAG, "→ Command: STOP");
        esp_err_t result = shutter_driver_stop_motion(self->handle);
        uint8_t current_pos = shutter_driver_get_current_percent(self->handle);
        ESP_LOGI(TAG, "← Stopped at %d%% | Result: %s", current_pos, 
                result == ESP_OK ? "SUCCESS" : "FAILED");
    } 
    else if (strcmp(cmd, "calibrate") == 0) {
        ESP_LOGI(TAG, "→ Command: START CALIBRATION");
        bool was_calibrated = shutter_driver_is_calibrated(self->handle);
        esp_err_t result = shutter_driver_start_calibration(self->handle);
        ESP_LOGI(TAG, "← Calibration started | Previously calibrated: %s | Result: %s",
                was_calibrated ? "YES" : "NO",
                result == ESP_OK ? "SUCCESS" : "FAILED");
    } 
    else if (strcmp(cmd, "invert_on") == 0) {
        ESP_LOGI(TAG, "→ Command: INVERT DIRECTION → ON");
        shutter_driver_set_direction(self->handle, true);
        bool current = shutter_driver_get_direction_inverted(self->handle);
        ESP_LOGI(TAG, "← Direction now: %s", current ? "INVERTED ✓" : "NORMAL (ERROR!)");
    }
    else if (strcmp(cmd, "invert_off") == 0) {
        ESP_LOGI(TAG, "→ Command: INVERT DIRECTION → OFF");
        shutter_driver_set_direction(self->handle, false);
        bool current = shutter_driver_get_direction_inverted(self->handle);
        ESP_LOGI(TAG, "← Direction now: %s", current ? "INVERTED (ERROR!)" : "NORMAL ✓");
    }
    else if (strcmp(cmd, "reset") == 0) {
        ESP_LOGW(TAG, "=== Factory Reset Initiated via WebUI ===");
        
        // send confirmation message to client
        const char* confirm_msg = "{\"type\":\"info\",\"message\":\"Resetting device...\"}";
        httpd_ws_frame_t confirm_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)confirm_msg,
            .len = strlen(confirm_msg)
        };
        httpd_ws_send_frame_async(req->handle, fd, &confirm_frame);
        
        // wait a moment to ensure message is sent
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        esp_matter::factory_reset();
        
        // will never reach here (factory_reset() makes esp_restart())
    }
    else if (strcmp(cmd, "get_device_name") == 0) {
    ESP_LOGI(TAG, "WebSocket: Get device name requested");
    
    extern DeviceNaming* deviceNaming;
    if (!deviceNaming) {
        const char* error = "{\"type\":\"error\",\"message\":\"Device naming not initialized\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)error,
            .len = strlen(error)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        free(buf);
        return ESP_OK;
    }
    
    DeviceNaming::DeviceName names = deviceNaming->getNames();
    
    char json_buf[512];
    snprintf(json_buf, sizeof(json_buf),
            "{\"type\":\"device_name\","
            "\"room\":\"%s\","
            "\"type\":\"%s\","
            "\"position\":\"%s\","
            "\"hostname\":\"%s\","
            "\"matterName\":\"%s\"}",
            names.room.c_str(),
            names.type.c_str(),
            names.position.c_str(),
            names.hostname.c_str(),
            names.matterName.c_str());
    
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)json_buf,
        .len = strlen(json_buf)
    };
    httpd_ws_send_frame_async(req->handle, fd, &frame);
}

else if (strncmp(cmd, "{\"cmd\":\"save_device_name\"", 26) == 0) {
    ESP_LOGI(TAG, "WebSocket: Save device name requested");
    
    extern DeviceNaming* deviceNaming;
    if (!deviceNaming) {
        const char* error = "{\"type\":\"error\",\"message\":\"Device naming not initialized\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)error,
            .len = strlen(error)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        free(buf);
        return ESP_OK;
    }
    
    // Parse JSON
    String json = String(cmd);
    
    int roomStart = json.indexOf("\"room\":\"") + 8;
    int roomEnd = json.indexOf("\"", roomStart);
    String room = json.substring(roomStart, roomEnd);
    
    int typeStart = json.indexOf("\"type\":\"") + 8;
    int typeEnd = json.indexOf("\"", typeStart);
    String type = json.substring(typeStart, typeEnd);
    
    int posStart = json.indexOf("\"position\":\"") + 12;
    int posEnd = json.indexOf("\"", posStart);
    String position = json.substring(posStart, posEnd);
    
    ESP_LOGI(TAG, "  Room: %s", room.c_str());
    ESP_LOGI(TAG, "  Type: %s", type.c_str());
    ESP_LOGI(TAG, "  Position: %s", position.c_str());
    
    // Validate and save
    if (!deviceNaming->save(room, type, position)) {
        const char* error = "{\"type\":\"error\",\"message\":\"Invalid device name parameters\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)error,
            .len = strlen(error)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        free(buf);
        return ESP_OK;
    }
    
    // Apply to system (mDNS + Matter)
    deviceNaming->apply();
    
    // Get updated names
    DeviceNaming::DeviceName names = deviceNaming->getNames();
    
    // Send confirmation
    char success_msg[512];
    snprintf(success_msg, sizeof(success_msg),
            "{\"type\":\"device_name_saved\","
            "\"hostname\":\"%s\","
            "\"matterName\":\"%s\"}",
            names.hostname.c_str(),
            names.matterName.c_str());
    
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)success_msg,
        .len = strlen(success_msg)
    };
    httpd_ws_send_frame_async(req->handle, fd, &frame);
    
    ESP_LOGI(TAG, "✓ Device name saved and applied");
}
    else if (strcmp(cmd, "status") == 0) {
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf), 
                 "{\"type\":\"status\",\"pos\":%d,\"cal\":%s,\"inv\":%s}",
                 shutter_driver_get_current_percent(self->handle),
                 shutter_driver_is_calibrated(self->handle) ? "true" : "false",
                 shutter_driver_get_direction_inverted(self->handle) ? "true" : "false");
        
        httpd_ws_frame_t status_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)status_buf,
            .len = strlen(status_buf)
        };
        httpd_ws_send_frame_async(req->handle, fd, &status_frame);
    }
    else if (strcmp(cmd, "matter_status") == 0) {
        uint8_t fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
        bool commissioned = Matter.isDeviceCommissioned() && (fabric_count > 0);
        
        String qrUrl = "";
        String qrImageUrl = "";
        String pairingCode = "";
        
        ESP_LOGI(TAG, "=== matter_status Command ===");
        ESP_LOGI(TAG, "Commissioned: %s", commissioned ? "true" : "false");
        ESP_LOGI(TAG, "Fabrics: %d", fabric_count);
        
        if (!commissioned) {
            String fullUrl = Matter.getOnboardingQRCodeUrl();
            pairingCode = Matter.getManualPairingCode();
            
            ESP_LOGI(TAG, "Full URL: %s", fullUrl.c_str());
            ESP_LOGI(TAG, "Pairing Code: %s", pairingCode.c_str());
            
            qrUrl = fullUrl;
            
            int dataIdx = fullUrl.indexOf("data=");
            if (dataIdx > 0) {
                String payload = fullUrl.substring(dataIdx + 5);
                
                qrImageUrl = "https://quickchart.io/qr?text=" + payload + "&size=300";
                
                ESP_LOGI(TAG, "Payload: %s", payload.c_str());
                ESP_LOGI(TAG, "Image URL: %s", qrImageUrl.c_str());
            } else {
                ESP_LOGW(TAG, "Could not find 'data=' in URL!");
            }
        }
        
        char matter_buf[768];
        int len = snprintf(matter_buf, sizeof(matter_buf),
                          "{\"type\":\"matter_status\","
                          "\"commissioned\":%s,"
                          "\"fabrics\":%d,"
                          "\"qr_url\":\"%s\","
                          "\"qr_image\":\"%s\","
                          "\"pairing_code\":\"%s\"}",
                          commissioned ? "true" : "false",
                          fabric_count,
                          qrUrl.c_str(),
                          qrImageUrl.c_str(),
                          pairingCode.c_str());
        
        ESP_LOGI(TAG, "Sending JSON (%d bytes): %s", len, matter_buf);
        
        httpd_ws_frame_t matter_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)matter_buf,
            .len = (size_t)len
        };
        
        esp_err_t ret = httpd_ws_send_frame_async(req->handle, fd, &matter_frame);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send matter_status: %s", esp_err_to_name(ret));
        }
    }
    else if (strcmp(cmd, "info") == 0) {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char chipid[18];
        snprintf(chipid, 18, "%02X:%02X:%02X:%02X:%02X:%02X", 
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
        const esp_app_desc_t* app = esp_app_get_description();
        
        const char* reset_reason_str;
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:   reset_reason_str = "Power On"; break;
            case ESP_RST_SW:        reset_reason_str = "Software Reset"; break;
            case ESP_RST_PANIC:     reset_reason_str = "Exception/Panic"; break;
            case ESP_RST_INT_WDT:   reset_reason_str = "Interrupt WDT"; break;
            case ESP_RST_TASK_WDT:  reset_reason_str = "Task WDT"; break;
            case ESP_RST_WDT:       reset_reason_str = "Other WDT"; break;
            case ESP_RST_DEEPSLEEP: reset_reason_str = "Deep Sleep"; break;
            case ESP_RST_BROWNOUT:  reset_reason_str = "Brownout"; break;
            default:                reset_reason_str = "Unknown"; break;
        }
        
        uint32_t flash_size = 0;
        esp_flash_get_size(NULL, &flash_size);
        
        char info_buf[512];
        snprintf(info_buf, sizeof(info_buf),
                 "{\"type\":\"info\","
                 "\"chip\":\"%s\","
                 "\"uptime\":%llu,"
                 "\"heap\":%u,"
                 "\"minheap\":%u,"
                 "\"flash\":%u,"
                 "\"ver\":\"%s\","
                 "\"reset\":\"%s\"}",
                 chipid,
                 esp_timer_get_time() / 1000000,
                 esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size(),
                 flash_size,
                 app->version,
                 reset_reason_str);
        
        httpd_ws_frame_t info_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)info_buf,
            .len = strlen(info_buf)
        };
        httpd_ws_send_frame_async(req->handle, fd, &info_frame);
    }
    
    // ============================================================================
    // BLE Commands
    // ============================================================================

    else if (strcmp(cmd, "ble_scan") == 0) {
        if (self->bleManager) {
            ESP_LOGI(TAG, "═══════════════════════════════════");
            ESP_LOGI(TAG, "WebSocket: BLE DISCOVERY SCAN");
            ESP_LOGI(TAG, "═══════════════════════════════════");
            
            // Ensure BLE is started BEFORE scanning
            if (!self->bleManager->isBLEStarted()) {
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "→ BLE not started yet");
                ESP_LOGI(TAG, "  Starting BLE for discovery scan...");
                
                if (!self->bleManager->ensureBLEStarted()) {
                    ESP_LOGE(TAG, "✗ Failed to start BLE");
                    
                    const char* error = "{\"type\":\"error\",\"message\":\"Failed to start BLE\"}";
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(req->handle, fd, &frame);
                    
                    free(buf);
                    return ESP_OK;
                }
                
                ESP_LOGI(TAG, "✓ BLE started successfully");
                ESP_LOGI(TAG, "");
                
                // Kurze Pause für BLE Stack
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            
            ESP_LOGI(TAG, "Starting 10-second discovery scan...");
            ESP_LOGI(TAG, "Will stop on first Shelly BLU Door/Window found!");
            
            // Start Scan
            self->bleManager->startScan(10, true);

            // Task-Parameter für Scan-Monitoring
            struct ScanMonitorParams {
                WebUIHandler* handler;
                ShellyBLEManager* bleManager;
            };
            
            ScanMonitorParams* params = new ScanMonitorParams{
                self,
                self->bleManager
            };
            
            // Memory Stats VOR Task-Erstellung
            self->logMemoryStats("Before BLE Scan Monitor Task");
            
            xTaskCreate([](void *param) {
                // RAII-Pattern
                std::unique_ptr<ScanMonitorParams> p(
                    static_cast<ScanMonitorParams*>(param)
                );
                
                ESP_LOGI(TAG, "📡 Scan monitor task started");
                
                // Watchdog entfernen (Scan kann bis zu 12 Sekunden dauern)
                esp_task_wdt_delete(NULL);
                
                // Memory Stats VOR Scan
                p->handler->logMemoryStats("Scan Monitor Start");

                // Wait for scan completion
                uint32_t elapsed = 0;
                const uint32_t max_duration = 12000;
                
                while (elapsed < max_duration) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    elapsed += 100;
                    
                    if (p->bleManager && !p->bleManager->isScanActive()) {
                        ESP_LOGI(TAG, "✓ Scan ended at %u ms", elapsed);
                        break;
                    }
                }
                
                // Memory Stats NACH Scan
                p->handler->logMemoryStats("Scan Monitor End");

                // Send completion
                const char *complete_msg = "{\"type\":\"ble_scan_complete\"}";
                p->handler->broadcast_to_all_clients(complete_msg);
                ESP_LOGI(TAG, "✓ Scan complete sent");

                vTaskDelay(pdMS_TO_TICKS(200));

                // Send devices
                if (p->bleManager) {
                    std::vector<ShellyBLEDevice> discovered = p->bleManager->getDiscoveredDevices();

                    if (discovered.size() > 0) {
                        // Lokaler Buffer (kein static!)
                        char json_buf[2048];
                        
                        int offset = snprintf(json_buf, sizeof(json_buf),
                                            "{\"type\":\"ble_discovered\",\"devices\":[");

                        for (size_t i = 0; i < discovered.size() && i < 10; i++) {
                            offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                                            "%s{\"name\":\"%s\",\"address\":\"%s\",\"rssi\":%d,\"encrypted\":%s}",
                                            i > 0 ? "," : "",
                                            discovered[i].name.c_str(),
                                            discovered[i].address.c_str(),
                                            discovered[i].rssi,
                                            discovered[i].isEncrypted ? "true" : "false");
                        }
                        snprintf(json_buf + offset, sizeof(json_buf) - offset, "]}");

                        p->handler->broadcast_to_all_clients(json_buf);
                        ESP_LOGI(TAG, "✓ Sent %d devices", discovered.size());
                    } else {
                        const char *empty = "{\"type\":\"ble_discovered\",\"devices\":[]}";
                        p->handler->broadcast_to_all_clients(empty);
                        ESP_LOGI(TAG, "ℹ No devices found");
                    }
                }
                
                // High Water Mark Logging
                UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
                ESP_LOGI(TAG, "Task Stack High Water Mark: %u bytes", 
                        highWater * sizeof(StackType_t));
                
                if (highWater < 512) {
                    ESP_LOGW(TAG, "⚠️ Stack critically low! Consider increasing size.");
                }

                ESP_LOGI(TAG, "✓ Scan monitor task complete");
                
                // unique_ptr wird automatisch freigegeben
                vTaskDelete(NULL);
                
            }, "ble_scan_mon", 4096, params, 1, NULL);  // Stack: 4KB (kleiner Task)
        }
    }



    else if (strcmp(cmd, "ble_status") == 0) {
    if (self->bleManager) {
        ESP_LOGI(TAG, "BLE status requested");
        
        // Discovery List
        std::vector<ShellyBLEDevice> discovered = self->bleManager->getDiscoveredDevices();
        
        char json_buf[2048];
        int offset = snprintf(json_buf, sizeof(json_buf), 
                              "{\"type\":\"ble_discovered\",\"devices\":[");
        
        for (size_t i = 0; i < discovered.size(); i++) {
            offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                               "%s{\"name\":\"%s\",\"address\":\"%s\",\"rssi\":%d,\"encrypted\":%s}",
                               i > 0 ? "," : "",
                               discovered[i].name.c_str(),
                               discovered[i].address.c_str(),
                               discovered[i].rssi,
                               discovered[i].isEncrypted ? "true" : "false");
        }
        snprintf(json_buf + offset, sizeof(json_buf) - offset, "]}");
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)json_buf,
            .len = strlen(json_buf)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // Paired Device Status
        bool paired = self->bleManager->isPaired();
        
        ShellyBLEManager::DeviceState state = self->bleManager->getDeviceState();
        const char* stateStr = "not_paired";
        
        if (state == ShellyBLEManager::STATE_CONNECTED_UNENCRYPTED) {
            stateStr = "connected_unencrypted";
        } else if (state == ShellyBLEManager::STATE_CONNECTED_ENCRYPTED) {
            stateStr = "connected_encrypted";
        }
        
        if (paired) {
            PairedShellyDevice device = self->bleManager->getPairedDevice();
            ShellyBLESensorData sensorData;
            bool hasData = self->bleManager->getSensorData(sensorData);

            // Passkey und Bindkey aus NVS laden
            String passkey = "Not set";
            String bindkey = device.bindkey;

            Preferences prefs;
            if (prefs.begin("ShellyBLE", true)) {
                uint32_t stored_passkey = prefs.getUInt("passkey", 0);
                if (stored_passkey > 0) {
                    char pk_buf[8];
                    snprintf(pk_buf, sizeof(pk_buf), "%06u", stored_passkey);
                    passkey = String(pk_buf);
                }
                prefs.end();
            }
            
            if (bindkey.length() == 0) {
                if (prefs.begin("ShellyBLE", true)) {
                    bindkey = prefs.getString("bindkey", "");
                    prefs.end();
                }
            }

            bool continuousScanActive = self->bleManager->isScanActive();
            
            offset = snprintf(json_buf, sizeof(json_buf),
                             "{\"type\":\"ble_status\","
                             "\"paired\":true,"
                             "\"state\":\"%s\","
                             "\"name\":\"%s\","
                             "\"address\":\"%s\","
                             "\"passkey\":\"%s\","
                             "\"bindkey\":\"%s\","
                             "\"continuous_scan_active\":%s,"
                             "\"sensor_data\":{",
                             stateStr,
                             device.name.c_str(),
                             device.address.c_str(),
                             passkey.c_str(),
                             bindkey.c_str(),
                             continuousScanActive ? "true" : "false");
            
            if (hasData) {
                // Berechne "seconds_ago" im Backend!
                uint32_t currentMillis = millis();
                uint32_t secondsAgo = 0;
                bool timeValid = false;
                
                if (sensorData.lastUpdate > 0) {
                    if (currentMillis >= sensorData.lastUpdate) {
                        secondsAgo = (currentMillis - sensorData.lastUpdate) / 1000;
                        timeValid = true;
                    } else {
                        // Overflow
                        uint32_t millisToOverflow = (0xFFFFFFFF - sensorData.lastUpdate);
                        secondsAgo = (millisToOverflow + currentMillis) / 1000;
                        timeValid = true;
                    }
                    
                    // Sanity check
                    if (secondsAgo > 86400) {
                        ESP_LOGW(TAG, "⚠ Data older than 24 hours");
                        timeValid = false;
                    }
                } else {
                    timeValid = false;
                }
                
                // Sende entweder validen Wert oder -1
                int32_t secondsAgoToSend = timeValid ? (int32_t)secondsAgo : -1;
                
                offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                                  "\"valid\":true,"
                                  "\"packet_id\":%d,"
                                  "\"window_open\":%s,"
                                  "\"battery\":%d,"
                                  "\"illuminance\":%u,"
                                  "\"rotation\":%d,"
                                  "\"rssi\":%d,"
                                  "\"has_button_event\":%s,"
                                  "\"button_event\":%d,"
                                  "\"seconds_ago\":%d",
                                  sensorData.packetId,
                                  sensorData.windowOpen ? "true" : "false",
                                  sensorData.battery,
                                  sensorData.illuminance,
                                  sensorData.rotation,
                                  sensorData.rssi,
                                  sensorData.hasButtonEvent ? "true" : "false",
                                  (int)sensorData.buttonEvent,
                                  secondsAgoToSend);
            } else {
                offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                                  "\"valid\":false");
            }
            
            snprintf(json_buf + offset, sizeof(json_buf) - offset, "}}");
        } else {
            bool isScanActive = self->bleManager ? self->bleManager->isScanActive() : false;
        snprintf(json_buf, sizeof(json_buf),
                "{\"type\":\"ble_status\","
                "\"paired\":false,"
                "\"state\":\"%s\","
                "\"continuous_scan_active\":%s}",
                stateStr,
                isScanActive ? "true" : "false");
        }
        
        frame.payload = (uint8_t*)json_buf;
        frame.len = strlen(json_buf);
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
}

// ════════════════════════════════════════════════════════════════════════
// BLE SMART CONNECT COMMAND (3-in-1 Workflow)
// ════════════════════════════════════════════════════════════════════════

else if (strncmp(cmd, "{\"cmd\":\"ble_smart_connect\"", 26) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        // Parse address
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        // Parse passkey
        int passkeyStart = json.indexOf("\"passkey\":") + 10;
        int passkeyEnd = json.indexOf(",", passkeyStart);
        if (passkeyEnd == -1) passkeyEnd = json.indexOf("}", passkeyStart);
        String passkeyStr = json.substring(passkeyStart, passkeyEnd);
        uint32_t passkey = passkeyStr.toInt();
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "SMART CONNECT");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Mode: %s", passkey > 0 ? "ENCRYPTED" : "UNENCRYPTED");
        if (passkey > 0) {
            ESP_LOGI(TAG, "Passkey: %06u", passkey);
        }
        
        // Button-Press Anleitung VORHER senden
        const char* instructions = 
            "{\"type\":\"info\",\"message\":\"<strong>📋 GET READY!</strong><br><br>"
            "<strong>RIGHT NOW:</strong><br>"
            "1. Press and HOLD the button on the device<br>"
            "2. Keep holding... (count to 15)<br>"
            "3. LED should flash rapidly<br><br>"
            "Starting connection in 5 seconds...<br>"
            "Keep holding the button!\"}";
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)instructions,
            .len = strlen(instructions)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // Gib User 5 Sekunden
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // Task-Parameter mit std::unique_ptr
        struct SmartConnectParams {
            ShellyBLEManager* bleManager;
            WebUIHandler* handler;
            int fd;
            String address;
            uint32_t passkey;
        };
        
        SmartConnectParams* params = new SmartConnectParams{
            self->bleManager,
            self,
            fd,
            address,
            passkey
        };
        
        // Memory Stats VOR Task-Erstellung
        self->logMemoryStats("Before Smart Connect Task");
        
        xTaskCreate([](void* pvParameters) {
            // RAII-Pattern: unique_ptr garantiert Freigabe!
            std::unique_ptr<SmartConnectParams> p(
                static_cast<SmartConnectParams*>(pvParameters)
            );
            
            ESP_LOGI(TAG, "🚀 Smart Connect Task started");
            ESP_LOGI(TAG, "   Address: %s", p->address.c_str());
            ESP_LOGI(TAG, "   Passkey: %s", p->passkey > 0 ? "SET" : "NONE");
            
            // Watchdog entfernen (kann lange dauern)
            esp_task_wdt_delete(NULL);
            
            // Memory Stats VOR Operation
            p->handler->logMemoryStats("Smart Connect Task Start");
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Pre-connection Scanner Status:");
            if (p->bleManager->isScanActive()) {
                ESP_LOGW(TAG, "  ⚠ Scanner is ACTIVE - will be stopped by connectDevice()");
            } else {
                ESP_LOGI(TAG, "  ✓ Scanner is IDLE - ready for GATT connection");
            }
            ESP_LOGI(TAG, "");
            
            // Smart Connect aufrufen (stoppt Scanner automatisch)
            bool success = p->bleManager->smartConnectDevice(p->address, p->passkey);
            
            // Memory Stats NACH Operation
            p->handler->logMemoryStats("Smart Connect Task End");
            
            if (success) {
                PairedShellyDevice device = p->bleManager->getPairedDevice();
                
                char success_msg[1024];
                
                if (p->passkey > 0) {
                    // Encrypted Mode Success
                    snprintf(success_msg, sizeof(success_msg),
                            "{\"type\":\"success\",\"message\":\"<strong>Encrypted Connection Complete!</strong><br><br>"
                            "Your device is now:<br>"
                            "✓ Bonded (trusted connection)<br>"
                            "✓ Encrypted (passkey: %06u)<br>"
                            "✓ Bindkey received: %s<br><br>"
                            "<strong>⚠️ SAVE YOUR CREDENTIALS!</strong><br>"
                            "You will need them for future connections.<br><br>"
                            "Continuous scan is now active.\"}",
                            p->passkey,
                            device.bindkey.c_str());
                } else {
                    // Unencrypted Mode Success
                    snprintf(success_msg, sizeof(success_msg),
                            "{\"type\":\"success\",\"message\":\"<strong>✓ Device Connected!</strong><br><br>"
                            "The device is bonded but NOT encrypted yet.<br><br>"
                            "You can enable encryption later via the UI.<br><br>"
                            "Continuous scan is now active.\"}");
                }
                
                // Sende Success Message
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)success_msg,
                        .len = strlen(success_msg)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                // Modal schließen
                vTaskDelay(pdMS_TO_TICKS(2000));
                p->handler->sendModalClose(p->fd, "ble-connect-modal");
                
                ESP_LOGI(TAG, "✓ Smart Connect successful");
                
                // Status-Update senden
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                const char* stateStr = (p->passkey > 0) ? "connected_encrypted" : "connected_unencrypted";
                
                char status_buf[512];
                snprintf(status_buf, sizeof(status_buf),
                        "{\"type\":\"ble_status\","
                        "\"paired\":true,"
                        "\"state\":\"%s\","
                        "\"name\":\"%s\","
                        "\"address\":\"%s\","
                        "\"passkey\":\"%06u\","
                        "\"bindkey\":\"%s\","
                        "\"sensor_data\":{\"valid\":false}}",
                        stateStr,
                        device.name.c_str(),
                        device.address.c_str(),
                        p->passkey,
                        device.bindkey.c_str());
                
                p->handler->broadcast_to_all_clients(status_buf);
                
            } else {
                // Fehler
                const char* error = 
                    "{\"type\":\"error\",\"message\":\"<strong>✗ Connection Failed</strong><br><br>"
                    "<strong>Most likely causes:</strong><br><br>"
                    "1️⃣ <strong>Button not held long enough</strong><br>"
                    "   → Must hold for FULL 15 seconds<br>"
                    "   → LED must flash RAPIDLY<br><br>"
                    "2️⃣ <strong>Device too far away</strong><br>"
                    "   → Move within 2 meters<br><br>"
                    "3️⃣ <strong>Wrong passkey</strong> (if encrypted)<br>"
                    "   → Try factory reset first<br><br>"
                    "<strong>Try again!</strong>\"}";
                
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                ESP_LOGE(TAG, "✗ Smart Connect failed");
            }
            
            // High Water Mark Logging
            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack High Water Mark: %u bytes", 
                     highWater * sizeof(StackType_t));
            
            if (highWater < 512) {
                ESP_LOGW(TAG, "⚠️ Stack critically low! Consider increasing size.");
            }
            
            ESP_LOGI(TAG, "✓ Smart Connect Task completed");
            
            // unique_ptr wird hier automatisch freigegeben!
            vTaskDelete(NULL);
            
        }, "ble_smart", 8192, params, 5, NULL);  // Stack: 8KB
        
        ESP_LOGI(TAG, "✓ Smart Connect task created");
    }
}

// ============================================================================
// BLE Connect Command (Phase 1) - Mit Smart Detection
// ============================================================================
else if (strncmp(cmd, "{\"cmd\":\"ble_connect\"", 20) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "BLE CONNECT (Phase 1: Bonding)");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        
        // Sende Anweisungen VOR dem Connect-Versuch
        const char* instructions = 
            "{\"type\":\"info\",\"message\":\"<strong>📋 GET READY!</strong><br><br>"
            "<strong>RIGHT NOW:</strong><br>"
            "1. Press and HOLD the button on the device<br>"
            "2. Keep holding... (count to 15)<br>"
            "3. LED should flash rapidly<br><br>"
            "Starting connection in 5 seconds...<br>"
            "Keep holding the button!\"}";
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)instructions,
            .len = strlen(instructions)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // Gib User 5 Sekunden zum Lesen + Button drücken
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // Jetzt verbinden
        if (self->bleManager->connectDevice(address)) {
            const char* success = 
                "{\"type\":\"success\",\"message\":\"<strong>✓ Bonding Complete!</strong><br><br>"
                "The device is now bonded and ready.<br><br>"
                "<strong>Connection is ACTIVE</strong><br><br>"
                "Next steps:<br>"
                "• Click 'Enable Encryption' to set passkey<br>"
                "• NO button press needed for encryption!<br><br>"
                "Note: Device is bonded but NOT encrypted yet.\"}";
            
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);

            vTaskDelay(pdMS_TO_TICKS(2000));
            self->sendModalClose(fd, "ble-connect-modal");
            
            ESP_LOGI(TAG, "✓ Bonding successful");
            
            // Status-Update
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            PairedShellyDevice device = self->bleManager->getPairedDevice();
            
            char status_buf[512];
            snprintf(status_buf, sizeof(status_buf),
                    "{\"type\":\"ble_status\","
                    "\"paired\":true,"
                    "\"state\":\"connected_unencrypted\","
                    "\"name\":\"%s\","
                    "\"address\":\"%s\","
                    "\"sensor_data\":{\"valid\":false}}",
                    device.name.c_str(),
                    device.address.c_str());
            
            self->broadcast_to_all_clients(status_buf);
            
        } else {
            // BESSERE ERROR MESSAGE mit Troubleshooting
            const char* error = 
                "{\"type\":\"error\",\"message\":\"<strong>✗ Bonding Failed</strong><br><br>"
                "<strong>Most likely causes:</strong><br><br>"
                "1️⃣ <strong>Button not held long enough</strong><br>"
                "   → Must hold for FULL 15 seconds<br>"
                "   → LED must flash RAPIDLY (not slowly)<br><br>"
                "2️⃣ <strong>Device too far away</strong><br>"
                "   → Move device within 2 meters of ESP32<br><br>"
                "3️⃣ <strong>Device already bonded elsewhere</strong><br>"
                "   → Reset device first (hold button 30+ seconds)<br><br>"
                "4️⃣ <strong>Wrong address type</strong><br>"
                "   → Try scanning again<br><br>"
                "<strong>Try again and follow timing exactly!</strong>\"}";
            
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGE(TAG, "✗ Bonding failed");
        }
    }
}

else if (strncmp(cmd, "{\"cmd\":\"ble_encrypt\"", 20) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        // 1. Parameter manuell extrahieren (wie bei ble_connect)
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);

        int passStart = json.indexOf("\"passkey\":") + 10;
        // Suche das Ende der Zahl (entweder Komma oder schließende Klammer)
        int passEnd = json.indexOf(",", passStart);
        if (passEnd == -1) passEnd = json.indexOf("}", passStart);
        uint32_t passkey = json.substring(passStart, passEnd).toInt();

        // 2. Task-Parameter vorbereiten
        BLETaskParams* params = new BLETaskParams{self, fd, address, passkey};

        xTaskCreate([](void* pvParameters) {
            BLETaskParams* p = (BLETaskParams*)pvParameters;
            
            ESP_LOGI(TAG, "Starting Encryption Task for %s with Passkey %u", p->address.c_str(), p->passkey);

            // Info an UI senden
            const char* info = "{\"type\":\"info\",\"message\":\"Enabling encryption... Device will reboot.\"}";
            if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000))) {
                httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)info, .len = strlen(info) };
                httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                xSemaphoreGive(p->handler->client_mutex);
            }

            // 3. Die eigentliche Verschlüsselung im Manager aufrufen
            // Hier nutzen wir direkt den bleManager aus deinem 'self' (p->handler)
            if (p->handler->bleManager->enableEncryption(p->address, p->passkey)) {
                
                const char* success = "{\"type\":\"info\",\"message\":\"Encryption successful!\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000))) {
                    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)success, .len = strlen(success) };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }

                // UI Status-Update senden
                PairedShellyDevice dev = p->handler->bleManager->getPairedDevice();
                char json_buf[1024];
                snprintf(json_buf, sizeof(json_buf),
                        "{\"type\":\"ble_status\","
                        "\"paired\":true,"
                        "\"state\":\"connected_encrypted\"," 
                        "\"name\":\"%s\","
                        "\"address\":\"%s\","
                        "\"sensor_data\":{\"valid\":false}}",
                        dev.name.c_str(), dev.address.c_str());
                        
                p->handler->broadcast_to_all_clients(json_buf);
            } else {
                const char* error = "{\"type\":\"error\",\"message\":\"Encryption failed. Check passkey!\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000))) {
                    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)error, .len = strlen(error) };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
            }

            delete p;
            vTaskDelete(NULL);

            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack (ble_enc_task) usage: %u bytes free", highWater * sizeof(StackType_t));

        }, "ble_enc_task", 6144, params, 1, NULL);
    }
}

// ════════════════════════════════════════════════════════════════════════
// BLE ENABLE ENCRYPTION (Phase 2)
// ════════════════════════════════════════════════════════════════════════

else if (strncmp(cmd, "{\"cmd\":\"ble_enable_encryption\"", 30) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        int passkeyStart = json.indexOf("\"passkey\":") + 10;
        int passkeyEnd = json.indexOf(",", passkeyStart);
        if (passkeyEnd == -1) passkeyEnd = json.indexOf("}", passkeyStart);
        String passkeyStr = json.substring(passkeyStart, passkeyEnd);
        uint32_t passkey = passkeyStr.toInt();
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "BLE ENABLE ENCRYPTION (Phase 2)");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Passkey: %06u", passkey);
        
        // Info-Message an Client senden
        const char* info = 
            "{\"type\":\"info\",\"message\":\"<strong>🔐 Phase 2: Enabling Encryption</strong><br><br>"
            "Using ACTIVE connection from Phase 1.<br>"
            "<strong>NO button press needed!</strong><br><br>"
            "Writing passkey and reading bindkey...\"}";
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)info,
            .len = strlen(info)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // Task-Parameter für non-blocking Execution
        struct EncryptionTaskParams {
            ShellyBLEManager* bleManager;
            WebUIHandler* handler;
            int fd;
            String address;
            uint32_t passkey;
        };
        
        EncryptionTaskParams* params = new EncryptionTaskParams{
            self->bleManager,
            self,
            fd,
            address,
            passkey
        };
        
        // Memory Stats VOR Task-Erstellung
        self->logMemoryStats("Before Enable Encryption Task");
        
        // Starte separaten Task für nicht-blockierende Ausführung!
        xTaskCreate([](void* pvParameters) {
            // RAII-Pattern
            std::unique_ptr<EncryptionTaskParams> p(
                static_cast<EncryptionTaskParams*>(pvParameters)
            );
            
            ESP_LOGI(TAG, "🔐 Encryption Task started for %s", p->address.c_str());
            
            // Watchdog entfernen (kann bis zu 60s dauern)
            esp_task_wdt_delete(NULL);
            
            // Memory Stats VOR Operation
            p->handler->logMemoryStats("Enable Encryption Start");
            
            // Enable Encryption (mit internen Watchdog-Resets)
            bool success = p->bleManager->enableEncryption(p->address, p->passkey);
            
            // Memory Stats NACH Operation
            p->handler->logMemoryStats("Enable Encryption End");
            
            if (success) {
                // Hole Device-Info für Success-Message
                PairedShellyDevice device = p->bleManager->getPairedDevice();
                
                char success_msg[768];
                snprintf(success_msg, sizeof(success_msg),
                        "{\"type\":\"success\",\"message\":\"<strong>Encryption Enabled!</strong><br><br>"
                        "Your device is now securely encrypted.<br><br>"
                        "<strong>🔑 Bindkey:</strong> %s<br><br>"
                        "⚠️ <strong>SAVE THIS BINDKEY!</strong><br>"
                        "You will need it for:<br>"
                        "• Re-pairing after factory reset<br>"
                        "• Integration with other systems<br>"
                        "• Backup and restore<br><br>"
                        "Continuous scan will now pick up sensor data...\"}",
                        device.bindkey.c_str());
                
                                // Sende Erfolgs-Nachricht
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)success_msg,
                        .len = strlen(success_msg)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                ESP_LOGI(TAG, "✓ Encryption enabled successfully");

                vTaskDelay(pdMS_TO_TICKS(3000));
    
                char close_msg[128];
                snprintf(close_msg, sizeof(close_msg),
                        "{\"type\":\"modal_close\",\"modal_id\":\"enable-encryption-modal\"}");
                
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)close_msg,
                        .len = strlen(close_msg)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                // Status-Update nach kurzer Verzögerung
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                char status_buf[512];
                snprintf(status_buf, sizeof(status_buf),
                        "{\"type\":\"ble_status\","
                        "\"paired\":true,"
                        "\"state\":\"connected_encrypted\","
                        "\"name\":\"%s\","
                        "\"address\":\"%s\","
                        "\"sensor_data\":{\"valid\":false}}",
                        device.name.c_str(),
                        device.address.c_str());
                
                p->handler->broadcast_to_all_clients(status_buf);
                
                // Continuous Scan starten
                ESP_LOGI(TAG, "→ Starting continuous scan for sensor data...");
                p->bleManager->startContinuousScan();
                
            } else {
                const char* error = 
                    "{\"type\":\"error\",\"message\":\"<strong>✗ Encryption Failed</strong><br><br>"
                    "Could not enable encryption.<br><br>"
                    "Possible reasons:<br>"
                    "• Wrong passkey<br>"
                    "• Device rejected passkey<br>"
                    "• Connection timeout<br>"
                    "• Bindkey not found in NVS<br><br>"
                    "Please try again or re-pair the device.\"}";
                
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                ESP_LOGE(TAG, "✗ Encryption failed");
            }
            
            // High Water Mark Logging
            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack High Water Mark: %u bytes", 
                     highWater * sizeof(StackType_t));
            
            if (highWater < 512) {
                ESP_LOGW(TAG, "⚠️ Stack critically low! Consider increasing size.");
            }
            
            ESP_LOGI(TAG, "✓ Encryption task completed");
            
            // unique_ptr wird automatisch freigegeben
            vTaskDelete(NULL);
            
        }, "ble_enc", 8192, params, 5, NULL);  // Stack: 8KB
        
        ESP_LOGI(TAG, "✓ Encryption task created");
    }
}

// ════════════════════════════════════════════════════════════════════════
// BLE PAIR ALREADY-ENCRYPTED DEVICE (Passkey + Bindkey Known)
// ════════════════════════════════════════════════════════════════════════

else if (strncmp(cmd, "{\"cmd\":\"ble_pair_encrypted_known\"", 33) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        // Parse address
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        // Parse passkey
        int passkeyStart = json.indexOf("\"passkey\":") + 10;
        int passkeyEnd = json.indexOf(",", passkeyStart);
        String passkeyStr = json.substring(passkeyStart, passkeyEnd);
        uint32_t passkey = passkeyStr.toInt();
        
        // Parse bindkey
        int bindkeyStart = json.indexOf("\"bindkey\":\"") + 11;
        int bindkeyEnd = json.indexOf("\"", bindkeyStart);
        String bindkey = json.substring(bindkeyStart, bindkeyEnd);
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "PAIR ALREADY-ENCRYPTED DEVICE");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Passkey: %06u", passkey);
        ESP_LOGI(TAG, "Bindkey: %s", bindkey.c_str());
        ESP_LOGI(TAG, "");
        
        // Validate inputs
        if (bindkey.length() != 32) {
            ESP_LOGE(TAG, "✗ Invalid bindkey length: %d (expected 32)", bindkey.length());
            
            const char* error = "{\"type\":\"error\",\"message\":\"Invalid bindkey length\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            free(buf);
            return ESP_OK;
        }
        
        // Validate hex characters
        for (int i = 0; i < 32; i++) {
            char c = bindkey[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                ESP_LOGE(TAG, "✗ Invalid bindkey character at position %d: '%c'", i, c);
                
                const char* error = "{\"type\":\"error\",\"message\":\"Bindkey must contain only hex characters (0-9, a-f)\"}";
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)error,
                    .len = strlen(error)
                };
                httpd_ws_send_frame_async(req->handle, fd, &frame);
                
                free(buf);
                return ESP_OK;
            }
        }
        
        ESP_LOGI(TAG, "✓ Input validation passed");
        ESP_LOGI(TAG, "");
        
        // Info-Message an Client
        const char* info = 
            "{\"type\":\"info\",\"message\":\"<strong>🔐 Pairing with Encrypted Device</strong><br><br>"
            "Establishing secure connection...<br>"
            "This will:<br>"
            "• Bond with the device (no button press needed)<br>"
            "• Store passkey and bindkey<br>"
            "• Start decrypting broadcasts<br>"
            "• Begin continuous scanning\"}";
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)info,
            .len = strlen(info)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // Task-Parameter vorbereiten
        struct EncryptedKnownParams {
            ShellyBLEManager* bleManager;
            WebUIHandler* handler;
            int fd;
            String address;
            uint32_t passkey;
            String bindkey;
        };
        
        EncryptedKnownParams* params = new EncryptedKnownParams{
            self->bleManager,
            self,
            fd,
            address,
            passkey,
            bindkey
        };
        
        // Memory Stats VOR Task-Erstellung
        self->logMemoryStats("Before Encrypted Known Pairing Task");
        
        // Task erstellen (non-blocking)
        xTaskCreate([](void* pvParameters) {
            // RAII-Pattern
            std::unique_ptr<EncryptedKnownParams> p(
                static_cast<EncryptedKnownParams*>(pvParameters)
            );
            
            ESP_LOGI(TAG, "🔐 Already-Encrypted Pairing Task started");
            ESP_LOGI(TAG, "   Address: %s", p->address.c_str());
            
            // Watchdog entfernen
            esp_task_wdt_delete(NULL);
            
            // Memory Stats VOR Operation
            p->handler->logMemoryStats("Encrypted Known Pairing Start");
            
            // ════════════════════════════════════════════════════════════════
            // SCHRITT 1: Secure Bonding (OHNE Button-Press!)
            // ════════════════════════════════════════════════════════════════
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║   STEP 1: SECURE BONDING          ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "→ Establishing bonded connection...");
            ESP_LOGI(TAG, "  (No button press needed - already encrypted)");
            ESP_LOGI(TAG, "");
            
            // Get device info from discovered list
            uint8_t addressType = BLE_ADDR_RANDOM;  // Default für Shelly
            
            std::vector<ShellyBLEDevice> discovered = p->bleManager->getDiscoveredDevices();
            for (const auto& dev : discovered) {
                if (dev.address.equalsIgnoreCase(p->address)) {
                    addressType = dev.addressType;
                    break;
                }
            }
            
            // NimBLE Security Setup
            NimBLEDevice::setSecurityAuth(true, false, true);  // Bonding, No MITM, SC
            NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);  // Just Works
            
            // Create client
            NimBLEClient* pClient = NimBLEDevice::createClient();
            if (!pClient) {
                ESP_LOGE(TAG, "✗ Failed to create client");
                
                const char* error = "{\"type\":\"error\",\"message\":\"Failed to create BLE client\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                // unique_ptr wird automatisch freigegeben
                vTaskDelete(NULL);
                return;
            }
            
            pClient->setConnectTimeout(15000);
            
            // Connect
            NimBLEAddress bleAddr(p->address.c_str(), addressType);
            bool connected = pClient->connect(bleAddr, false);
            
            if (!connected) {
                // Try alternative address type
                uint8_t altType = (addressType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
                ESP_LOGI(TAG, "→ Trying alternative address type...");
                
                bleAddr = NimBLEAddress(p->address.c_str(), altType);
                connected = pClient->connect(bleAddr, false);
            }
            
            if (!connected) {
                ESP_LOGE(TAG, "✗ Connection failed");
                
                const char* error = "{\"type\":\"error\",\"message\":\"Connection failed. Device not reachable.\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                NimBLEDevice::deleteClient(pClient);
                // unique_ptr cleanup
                vTaskDelete(NULL);
                return;
            }
            
            ESP_LOGI(TAG, "✓ Connected");
            ESP_LOGI(TAG, "");
            
            // Request secure connection (bonding)
            ESP_LOGI(TAG, "→ Requesting secure connection...");
            bool secureResult = pClient->secureConnection();
            
            if (!secureResult) {
                ESP_LOGE(TAG, "✗ Secure connection failed");
                
                pClient->disconnect();
                NimBLEDevice::deleteClient(pClient);
                
                const char* error = "{\"type\":\"error\",\"message\":\"Bonding failed\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                                            };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                // unique_ptr cleanup
                vTaskDelete(NULL);
                return;
            }
            
            ESP_LOGI(TAG, "✓ Bonding complete");
            ESP_LOGI(TAG, "");
            
            // Disconnect (nicht mehr benötigt)
            pClient->disconnect();
            
            uint8_t retries = 0;
            while (pClient->isConnected() && retries < 20) {
                vTaskDelay(pdMS_TO_TICKS(100));
                retries++;
            }
            
            NimBLEDevice::deleteClient(pClient);
            
            // ════════════════════════════════════════════════════════════════
            // SCHRITT 2: Credentials speichern
            // ════════════════════════════════════════════════════════════════
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║   STEP 2: STORE CREDENTIALS       ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            
            // Find device name from discovered list
            String deviceName = "Unknown";
            for (const auto& dev : discovered) {
                if (dev.address.equalsIgnoreCase(p->address)) {
                    deviceName = dev.name;
                    break;
                }
            }
            
            // Store in BLE Manager
            Preferences prefs;
            prefs.begin("ShellyBLE", false);
            prefs.putString("address", p->address);
            prefs.putString("name", deviceName);
            prefs.putString("bindkey", p->bindkey);
            prefs.putUInt("passkey", p->passkey);
            prefs.end();
            
            ESP_LOGI(TAG, "✓ Stored in NVS:");
            ESP_LOGI(TAG, "  Address: %s", p->address.c_str());
            ESP_LOGI(TAG, "  Name: %s", deviceName.c_str());
            ESP_LOGI(TAG, "  Passkey: %06u", p->passkey);
            ESP_LOGI(TAG, "  Bindkey: %s", p->bindkey.c_str());
            ESP_LOGI(TAG, "");
            
            // Update internal state (reload from NVS)
            p->bleManager->loadPairedDevice();
            p->bleManager->updateDeviceState(ShellyBLEManager::STATE_CONNECTED_ENCRYPTED);
            
            // ════════════════════════════════════════════════════════════════
            // SCHRITT 3: Continuous Scan starten
            // ════════════════════════════════════════════════════════════════
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║   STEP 3: START CONTINUOUS SCAN   ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            p->bleManager->startContinuousScan();
            
            ESP_LOGI(TAG, "✓ Continuous scan started");
            ESP_LOGI(TAG, "");
            
            // Memory Stats NACH Operation
            p->handler->logMemoryStats("Encrypted Known Pairing End");
            
            // ════════════════════════════════════════════════════════════════
            // SUCCESS MESSAGE
            // ════════════════════════════════════════════════════════════════
            
            char success_msg[1024];
            snprintf(success_msg, sizeof(success_msg),
                    "{\"type\":\"success\",\"message\":\"<strong>Encrypted Device Paired!</strong><br><br>"
                    "Your device is now connected:<br>"
                    "✓ Secure bonded connection<br>"
                    "✓ Passkey: %06u<br>"
                    "✓ Bindkey: %s<br><br>"
                    "Broadcasts will be decrypted automatically.<br>"
                    "Continuous scan is now active.\"}",
                    p->passkey,
                    p->bindkey.c_str());
            
            if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)success_msg,
                    .len = strlen(success_msg)
                };
                httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                xSemaphoreGive(p->handler->client_mutex);
            }
            
            ESP_LOGI(TAG, "✓ Pairing successful");
            
            // Modal schließen
            vTaskDelay(pdMS_TO_TICKS(2000));
            p->handler->sendModalClose(p->fd, "ble-encrypted-known-modal");
            
            // Status-Update senden
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            char status_buf[768];
            snprintf(status_buf, sizeof(status_buf),
                    "{\"type\":\"ble_status\","
                    "\"paired\":true,"
                    "\"state\":\"connected_encrypted\","
                    "\"name\":\"%s\","
                    "\"address\":\"%s\","
                    "\"passkey\":\"%06u\","
                    "\"bindkey\":\"%s\","
                    "\"sensor_data\":{\"valid\":false}}",
                    deviceName.c_str(),
                    p->address.c_str(),
                    p->passkey,
                    p->bindkey.c_str());
            
            p->handler->broadcast_to_all_clients(status_buf);
            
            // High Water Mark Logging
            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack High Water Mark: %u bytes", 
                     highWater * sizeof(StackType_t));
            
            if (highWater < 512) {
                ESP_LOGW(TAG, "⚠️ Stack critically low! Consider increasing size.");
            }
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║  TASK COMPLETE                 ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            
            // unique_ptr wird automatisch freigegeben
            vTaskDelete(NULL);
            
        }, "ble_enc_known", 8192, params, 5, NULL);  // Stack: 8KB
        
        ESP_LOGI(TAG, "✓ Already-Encrypted pairing task created");
    }
}



else if (strncmp(cmd, "{\"cmd\":\"ble_unpair\"", 19) == 0) {
    if (self->bleManager) {
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "BLE UNPAIRING");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        
        if (self->bleManager->unpairDevice()) {
            // ✓ Callback für Contact Sensor Endpoint Removal
            if (self->remove_contact_sensor_callback) {
                ESP_LOGI(TAG, "→ Removing Contact Sensor endpoint...");
                self->remove_contact_sensor_callback();
            }
            
            const char* success = "{\"type\":\"info\",\"message\":\"Device unpaired\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGI(TAG, "✓ Device unpaired");
            ESP_LOGI(TAG, "✓ Continuous scan stopped");
        }
    }
}

else if (strncmp(cmd, "{\"cmd\":\"ble_pair\"", 17) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        int bindStart = json.indexOf("\"bindkey\":\"") + 11;
        int bindEnd = json.indexOf("\"", bindStart);
        String bindkey = json.substring(bindStart, bindEnd);
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "BLE PAIRING (Unencrypted)");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Bindkey: %s", bindkey.length() > 0 ? "[provided]" : "[empty]");
        
        if (self->bleManager->pairDevice(address, bindkey)) {
            const char* success = "{\"type\":\"info\",\"message\":\"Device paired successfully!\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGI(TAG, "✓ Pairing successful");
            
            // Continuous Scan starten
            ESP_LOGI(TAG, "→ Starting continuous scan for sensor data...");
            self->bleManager->startContinuousScan();
            
        } else {
            const char* error = "{\"type\":\"error\",\"message\":\"Failed to pair device\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGE(TAG, "✗ Pairing failed");
        }
    }
}

else if (strcmp(cmd, "ble_start_continuous_scan") == 0) {
    if (self->bleManager) {
        if (self->bleManager->isPaired()) {
            ESP_LOGI(TAG, "Starting continuous BLE scan (paired device exists)");
            self->bleManager->startContinuousScan();
            
            const char* success = "{\"type\":\"info\",\"message\":\"Continuous scanning started\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
        } else {
            ESP_LOGW(TAG, "Cannot start continuous scan - no device paired");
            
            const char* error = "{\"type\":\"error\",\"message\":\"No device paired\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
        }
    }
}

else if (strcmp(cmd, "ble_stop_scan") == 0) {
    if (self->bleManager) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  USER: STOP CONTINUOUS SCAN       ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        // KRITISCH: stopScan(true) = manueller Stop!
        // Dies verhindert Auto-Restart und setzt NVS auf false
        self->bleManager->stopScan(true);
        
        const char* success = 
            "{\"type\":\"info\",\"message\":\"Continuous scanning stopped by user\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)success,
            .len = strlen(success)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        ESP_LOGI(TAG, "✓ Continuous scan stopped (manual)");
        ESP_LOGI(TAG, "  NVS updated: continuous_scan = false");
        ESP_LOGI(TAG, "  Will NOT auto-restart");
        ESP_LOGI(TAG, "");
        
        // Status-Update nach kurzer Verzögerung
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // Sende aktuellen BLE Status
        if (self->bleManager->isPaired()) {
            PairedShellyDevice device = self->bleManager->getPairedDevice();
            
            char status_buf[512];
            snprintf(status_buf, sizeof(status_buf),
                    "{\"type\":\"ble_status\","
                    "\"paired\":true,"
                    "\"state\":\"connected_encrypted\","
                    "\"name\":\"%s\","
                    "\"address\":\"%s\","
                    "\"continuous_scan_active\":false,"
                    "\"sensor_data\":{\"valid\":false}}",
                    device.name.c_str(),
                    device.address.c_str());
            
            self->broadcast_to_all_clients(status_buf);
        }
    } else {
        const char* error = "{\"type\":\"error\",\"message\":\"BLE Manager not available\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)error,
            .len = strlen(error)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
}

    // ============================================================================
    // Contact Sensor Matter Toggle Commands
    // ============================================================================

    else if (strcmp(cmd, "contact_sensor_enable") == 0) {
        ESP_LOGI(TAG, "WebSocket: Enable Contact Sensor for Matter");
        
        extern void enableContactSensorMatter();
        enableContactSensorMatter();
        
        const char* success = "{\"type\":\"info\",\"message\":\"Contact Sensor enabled for Matter\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)success,
            .len = strlen(success)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
    else if (strcmp(cmd, "contact_sensor_disable") == 0) {
        ESP_LOGI(TAG, "WebSocket: Disable Contact Sensor for Matter");
        
        extern void disableContactSensorMatter();
        disableContactSensorMatter();
        
        const char* success = "{\"type\":\"info\",\"message\":\"Contact Sensor disabled for Matter\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)success,
            .len = strlen(success)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }

    // ════════════════════════════════════════════════════════════════════════
    // BLE READ SENSOR DATA (GATT)
    // ════════════════════════════════════════════════════════════════════════

    else if (strcmp(cmd, "read_sensor_data") == 0) {
        if (self->bleManager) {
            if (!self->bleManager->isPaired()) {
                const char* error = "{\"type\":\"error\",\"message\":\"No device paired\"}";
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)error,
                    .len = strlen(error)
                };
                httpd_ws_send_frame_async(req->handle, fd, &frame);
                free(buf);
                return ESP_OK;
            }
            
            ESP_LOGI(TAG, "═══════════════════════════════════");
            ESP_LOGI(TAG, "WebSocket: READ SENSOR DATA (GATT)");
            ESP_LOGI(TAG, "═══════════════════════════════════");
            
            PairedShellyDevice device = self->bleManager->getPairedDevice();
            
            // GATT Read in separatem Task (non-blocking)
            struct ReadTaskParams {
                ShellyBLEManager* bleManager;
                WebUIHandler* handler;
                int fd;
                String address;
            };
            
            ReadTaskParams* params = new ReadTaskParams{
                self->bleManager,
                self,
                fd,
                device.address
            };
            
            // Memory Stats VOR Task-Erstellung
            self->logMemoryStats("Before Read Sensor Data Task");
            
            xTaskCreate([](void* pvParameters) {
                // RAII-Pattern
                std::unique_ptr<ReadTaskParams> p(
                    static_cast<ReadTaskParams*>(pvParameters)
                );
                
                ESP_LOGI(TAG, "📖 Read Task started for %s", p->address.c_str());
                
                // Watchdog für diesen Task entfernen (kann lange dauern)
                esp_task_wdt_delete(NULL);
                
                // Memory Stats VOR Operation
                p->handler->logMemoryStats("Read Sensor Data Start");
                
                ShellyBLESensorData data;
                bool success = p->bleManager->readSampleBTHomeData(p->address, data);
                
                // Memory Stats NACH Operation
                p->handler->logMemoryStats("Read Sensor Data End");
                
                if (success) {
                    // Erfolg - Sende Daten an WebUI
                    char json_buf[512];
                    snprintf(json_buf, sizeof(json_buf),
                            "{\"type\":\"sensor_data_result\","
                            "\"success\":true,"
                            "\"packet_id\":%d,"
                            "\"battery\":%d,"
                            "\"window_open\":%s,"
                            "\"illuminance\":%u,"
                            "\"rotation\":%d,"
                            "\"rssi\":%d,"
                            "\"valid\":true}",
                            data.packetId,
                            data.battery,
                            data.windowOpen ? "true" : "false",
                            data.illuminance,
                            data.rotation,
                            data.rssi);
                    
                    if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        httpd_ws_frame_t frame = {
                            .type = HTTPD_WS_TYPE_TEXT,
                            .payload = (uint8_t*)json_buf,
                            .len = strlen(json_buf)
                        };
                        httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                        xSemaphoreGive(p->handler->client_mutex);
                    }
                    
                    ESP_LOGI(TAG, "✓ Sensor data sent to WebUI");
                    
                } else {
                    // Fehler
                    const char* error = "{\"type\":\"sensor_data_result\","
                                    "\"success\":false,"
                                    "\"error\":\"Failed to read sensor data\"}";
                    
                    if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        httpd_ws_frame_t frame = {
                            .type = HTTPD_WS_TYPE_TEXT,
                            .payload = (uint8_t*)error,
                            .len = strlen(error)
                        };
                        httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                        xSemaphoreGive(p->handler->client_mutex);
                    }
                    
                    ESP_LOGE(TAG, "✗ Failed to read sensor data");
                }
                
                // High Water Mark Logging
                UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
                ESP_LOGI(TAG, "Task Stack High Water Mark: %u bytes", 
                        highWater * sizeof(StackType_t));
                
                if (highWater < 512) {
                    ESP_LOGW(TAG, "⚠️ Stack critically low! Consider increasing size.");
                }
                
                // unique_ptr wird automatisch freigegeben
                vTaskDelete(NULL);
                
            }, "ble_read", 8192, params, 5, NULL);  // Stack: 8KB
            
            // Sofort Info an User senden
            const char* info = "{\"type\":\"info\",\"message\":\"Reading sensor data via GATT...\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)info,
                .len = strlen(info)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
        }
    }

    else if (strcmp(cmd, "contact_sensor_status") == 0) {
        extern bool contact_sensor_matter_enabled;
        extern bool contact_sensor_endpoint_active;
        
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf),
                "{\"type\":\"contact_sensor_status\","
                "\"enabled\":%s,"
                "\"active\":%s}",
                contact_sensor_matter_enabled ? "true" : "false",
                contact_sensor_endpoint_active ? "true" : "false");
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)status_buf,
            .len = strlen(status_buf)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
    else {
        ESP_LOGW(TAG, "Unknown command: '%s'", cmd);
    }
    
    free(buf);
    return ESP_OK;
}

// ============================================================================
// Client Management
// ============================================================================

void WebUIHandler::register_client(int fd) {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (active_clients.size() >= MAX_CLIENTS) {
            ESP_LOGW(TAG, "✗ WebSocket limit reached (%d/%d) - rejecting fd=%d", 
                     active_clients.size(), MAX_CLIENTS, fd);
            xSemaphoreGive(client_mutex);
            
            close(fd);
            return;
        }
        
        ClientInfo client;
        client.fd = fd;
        client.last_activity = millis();
        
        active_clients.push_back(client);
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Client connected: fd=%d (total: %d)", fd, active_clients.size());
        ESP_LOGI(TAG, "═══════════════════════════════════");
        xSemaphoreGive(client_mutex);
    }
}

void WebUIHandler::unregister_client(int fd) {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ESP_LOGI(TAG, "Closing socket fd=%d...", fd);
        
        active_clients.erase(
            std::remove_if(active_clients.begin(), active_clients.end(),
                          [fd](const ClientInfo& c) { return c.fd == fd; }),
            active_clients.end()
        );
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Client disconnected: fd=%d (remaining: %d)", fd, active_clients.size());
        ESP_LOGI(TAG, "═══════════════════════════════════");
        xSemaphoreGive(client_mutex);
    }
}

// ════════════════════════════════════════════════════════════════════════
// WebSocket Broadcast mit Heap-Allokation
// ════════════════════════════════════════════════════════════════════════

void WebUIHandler::broadcast_to_all_clients(const char* message) {
    if (!server || !message) return;
    
    // PRE-BROADCAST MEMORY CHECK
    logMemoryStats("Before WS Broadcast");
    
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t msg_len = strlen(message);
        
        // KRITISCHER FIX: Heap-Allokation für async Send
        char* msg_copy = (char*)malloc(msg_len + 1);
        if (!msg_copy) {
            ESP_LOGE(TAG, "✗ Failed to allocate %u bytes for broadcast", msg_len + 1);
            xSemaphoreGive(client_mutex);
            return;
        }
        strcpy(msg_copy, message);
        
        ESP_LOGI(TAG, "→ Broadcasting to %d clients (%u bytes, heap-allocated)", 
                 active_clients.size(), msg_len);
        
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.payload = (uint8_t*)msg_copy;
        ws_pkt.len = msg_len;
        
        // Kopie der Client-FDs (verhindert Deadlock bei unregister_client)
        std::vector<int> target_fds;
        target_fds.reserve(active_clients.size());
        for (const auto& client : active_clients) {
            target_fds.push_back(client.fd);
        }
        xSemaphoreGive(client_mutex);
        
        // Sende ohne Mutex (async Calls)
        int success_count = 0;
        for (int fd : target_fds) {
            esp_err_t ret = httpd_ws_send_frame_async(server, fd, &ws_pkt);
            
            if (ret == ESP_OK) {
                success_count++;
            } else {
                ESP_LOGW(TAG, "Failed to send to fd=%d: %s", fd, esp_err_to_name(ret));
            }
        }
        
        ESP_LOGI(TAG, "✓ Broadcast complete: %d/%d clients", 
                 success_count, target_fds.size());
        
        // ESP-IDF kopiert den Buffer intern SOFORT
        free(msg_copy);
        
        // POST-BROADCAST MEMORY CHECK
        logMemoryStats("After WS Broadcast");
        
    } else {
        ESP_LOGW(TAG, "Could not acquire mutex for broadcast");
    }
}

  // ============================================================================
  // WebSocket Broadcast: BLE State Change
  // ============================================================================

  void WebUIHandler::broadcastBLEStateChange(ShellyBLEManager::DeviceState oldState, 
                                            ShellyBLEManager::DeviceState newState) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   BROADCASTING STATE CHANGE       ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    
    const char* stateStr = "not_paired";
    const char* stateLabel = "Not Paired";
    
    if (newState == ShellyBLEManager::STATE_CONNECTED_UNENCRYPTED) {
        stateStr = "connected_unencrypted";
        stateLabel = "Connected (Unencrypted)";
    } else if (newState == ShellyBLEManager::STATE_CONNECTED_ENCRYPTED) {
        stateStr = "connected_encrypted";
        stateLabel = "Connected & Encrypted";
    }
    
    ESP_LOGI(TAG, "Old State: %d → New State: %d", (int)oldState, (int)newState);
    ESP_LOGI(TAG, "State String: %s", stateStr);
    ESP_LOGI(TAG, "Active clients: %d", active_clients.size());
    
    char msg[256];
    snprintf(msg, sizeof(msg),
            "{\"type\":\"ble_state_changed\","
            "\"state\":\"%s\","
            "\"label\":\"%s\"}",
            stateStr,
            stateLabel);
    
    ESP_LOGI(TAG, "Broadcasting message: %s", msg);
    broadcast_to_all_clients(msg);
    ESP_LOGI(TAG, "✓ Broadcast complete");
    ESP_LOGI(TAG, "");
  }

  // ============================================================================
  // WebSocket Broadcast: Sensor Data Update
  // ============================================================================

  void WebUIHandler::broadcastSensorDataUpdate(const String& address, const ShellyBLESensorData& data) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   BROADCASTING SENSOR DATA        ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Battery: %d%% | Window: %s", 
                data.battery, 
                data.windowOpen ? "OPEN" : "CLOSED");
        ESP_LOGI(TAG, "Packet ID: %d", data.packetId);
        ESP_LOGI(TAG, "Active clients: %d", active_clients.size());
        
        // Berechne "seconds_ago" mit Validierung
        uint32_t currentMillis = millis();
        uint32_t secondsAgo = 0;
        bool timeValid = false;
        
        // Prüfe auf valide lastUpdate
        if (data.lastUpdate > 0) {
            if (currentMillis >= data.lastUpdate) {
                // Normal case
                secondsAgo = (currentMillis - data.lastUpdate) / 1000;
                timeValid = true;
                
                ESP_LOGI(TAG, "Time calculation:");
                ESP_LOGI(TAG, "  Current millis: %u", currentMillis);
                ESP_LOGI(TAG, "  Last update:    %u", data.lastUpdate);
                ESP_LOGI(TAG, "  Difference:     %u ms", currentMillis - data.lastUpdate);
                ESP_LOGI(TAG, "  Seconds ago:    %u", secondsAgo);
                
            } else {
                // millis() overflow (nach ~49 Tagen)
                ESP_LOGW(TAG, "⚠ millis() overflow detected!");
                
                uint32_t millisToOverflow = (0xFFFFFFFF - data.lastUpdate);
                secondsAgo = (millisToOverflow + currentMillis) / 1000;
                timeValid = true;
                
                ESP_LOGI(TAG, "  Overflow calculation: %u seconds", secondsAgo);
            }
            
            // Wenn secondsAgo zu groß (> 1 Stunde ohne Update)
            if (secondsAgo > 3600) {
                ESP_LOGW(TAG, "");
                ESP_LOGW(TAG, "⚠️ WARNING: Last update very old!");
                ESP_LOGW(TAG, "   Seconds ago: %u (%.1f hours)", secondsAgo, secondsAgo / 3600.0f);
                ESP_LOGW(TAG, "   Possible issue: No new sensor data received!");
                ESP_LOGW(TAG, "");
                
                // Markiere als ungültig wenn > 24 Stunden
                if (secondsAgo > 86400) {
                    ESP_LOGE(TAG, "✗ Data too old (> 24 hours), marking as invalid");
                    timeValid = false;
                }
            }
            
        } else {
            ESP_LOGW(TAG, "");
            ESP_LOGW(TAG, "⚠ lastUpdate is 0 - no valid timestamp!");
            ESP_LOGW(TAG, "  This indicates no sensor data has been received yet.");
            ESP_LOGW(TAG, "");
            timeValid = false;
        }
        
        char json_buf[512];
        
        if (timeValid) {
            snprintf(json_buf, sizeof(json_buf),
                    "{\"type\":\"ble_sensor_update\","
                    "\"address\":\"%s\","
                    "\"window_open\":%s,"
                    "\"battery\":%d,"
                    "\"illuminance\":%u,"
                    "\"rotation\":%d,"
                    "\"rssi\":%d,"
                    "\"packet_id\":%d,"
                    "\"has_button_event\":%s,"
                    "\"button_event\":%d,"
                    "\"seconds_ago\":%u}",
                    address.c_str(),
                    data.windowOpen ? "true" : "false",
                    data.battery,
                    data.illuminance,
                    data.rotation,
                    data.rssi,
                    data.packetId,
                    data.hasButtonEvent ? "true" : "false",
                    (int)data.buttonEvent,
                    secondsAgo);
        } else {
            snprintf(json_buf, sizeof(json_buf),
                    "{\"type\":\"ble_sensor_update\","
                    "\"address\":\"%s\","
                    "\"window_open\":%s,"
                    "\"battery\":%d,"
                    "\"illuminance\":%u,"
                    "\"rotation\":%d,"
                    "\"rssi\":%d,"
                    "\"packet_id\":%d,"
                    "\"has_button_event\":%s,"
                    "\"button_event\":%d,"
                    "\"seconds_ago\":-1}",
                    address.c_str(),
                    data.windowOpen ? "true" : "false",
                    data.battery,
                    data.illuminance,
                    data.rotation,
                    data.rssi,
                    data.packetId,
                    data.hasButtonEvent ? "true" : "false",
                    (int)data.buttonEvent);
        }
        
        ESP_LOGI(TAG, "Broadcasting message: %s", json_buf);
        broadcast_to_all_clients(json_buf);
        ESP_LOGI(TAG, "✓ Broadcast complete");
        ESP_LOGI(TAG, "");
    }

    // ============================================================================
    // Helper: Send Modal Close Command
    // ============================================================================

    void WebUIHandler::sendModalClose(int fd, const char* modal_id) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                "{\"type\":\"modal_close\",\"modal_id\":\"%s\"}",
                modal_id);
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)msg,
            .len = strlen(msg)
        };
        
        httpd_ws_send_frame_async(server, fd, &frame);
        ESP_LOGI(TAG, "→ Sent modal close command: %s", modal_id);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Memory Monitoring Helper
    // ════════════════════════════════════════════════════════════════════════

    void WebUIHandler::logMemoryStats(const char* location) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  MEMORY STATS @ %-17s║", location);
        ESP_LOGI(TAG, "╠═══════════════════════════════════╣");
        
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_free_heap = esp_get_minimum_free_heap_size();
        uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_8BIT);
        
        ESP_LOGI(TAG, "║ Free Heap:         %6u bytes    ║", free_heap);
        ESP_LOGI(TAG, "║ Min Free (ever):   %6u bytes    ║", min_free_heap);
        ESP_LOGI(TAG, "║ Largest Block:     %6u bytes    ║", largest_block);
        ESP_LOGI(TAG, "║ Total Allocated:   %6u bytes    ║", info.total_allocated_bytes);
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        
        // WARNUNG bei kritischem Heap
        if (free_heap < 20000) {
            ESP_LOGW(TAG, "⚠️ WARNING: Free heap below 20KB!");
        }
        
        if (largest_block < 10000) {
            ESP_LOGW(TAG, "⚠️ WARNING: Largest free block below 10KB - fragmentation!");
        }
        
        ESP_LOGI(TAG, "");
    }

    void WebUIHandler::remove_client(int fd) {
    unregister_client(fd);
}

void WebUIHandler::cleanup_idle_clients() {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t now = millis();
        std::vector<int> timeout_fds;
        
        for (const auto& client : active_clients) {
            if (now - client.last_activity > WS_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Client fd=%d timed out (idle for %u ms)", 
                         client.fd, now - client.last_activity);
                timeout_fds.push_back(client.fd);
            }
        }
        
        xSemaphoreGive(client_mutex);
        
        // Cleanup außerhalb des Mutex
        for (int fd : timeout_fds) {
            unregister_client(fd);
        }
        
        if (timeout_fds.size() > 0) {
            ESP_LOGI(TAG, "Cleaned up %d idle clients", timeout_fds.size());
        }
    }
  }