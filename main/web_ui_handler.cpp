#include <Arduino.h>

#include "web_ui_handler.h"
#include "device_naming.h"
#include "credentials.h"

#include <WiFi.h>
#include <ESPmDNS.h>
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
#include <app/server/CommissioningWindowManager.h>
#include <platform/PlatformManager.h>
#include <lib/dnssd/Advertiser.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <Matter.h>
#include <Preferences.h>

#include <esp_matter.h>
#include <esp_matter_cluster.h>
#include <esp_matter_attribute.h>
#include <app-common/zap-generated/cluster-objects.h>

using namespace esp_matter;

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <Update.h>

extern uint16_t window_covering_endpoint_id;
extern WebUIHandler* webUI;

static const char* TAG = "WebUI";

extern class DeviceNaming* deviceNaming;

#define CMD_MATCH(cmd_str, pattern) \
    (strncmp(cmd_str, pattern, sizeof(pattern) - 1) == 0)

struct BLETaskParams {
    WebUIHandler* handler;
    int fd;
    String address;
    uint32_t passkey;
};

static uint8_t failed_login_count = 0;
static uint32_t last_failed_login = 0;
static uint32_t lockout_until = 0;

#ifndef HTTPD_429_TOO_MANY_REQUESTS
    #define HTTPD_429_TOO_MANY_REQUESTS ((httpd_err_code_t)429)
#endif

// ════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════════

static bool checkBasicAuth(httpd_req_t *req);
static bool loadAuthFromNVS(char* username, size_t username_len, 
                           char* password, size_t password_len);
static bool saveAuthToNVS(const char* username, const char* password);
static bool base64_decode(const char* input, char* output, size_t output_len);

static esp_err_t favicon_handler(httpd_req_t *req);
static esp_err_t apple_touch_icon_handler(httpd_req_t *req);

// ════════════════════════════════════════════════════════════════════════
// BASE64 HELPER (Implementation BEFORE first use)
// ════════════════════════════════════════════════════════════════════════

static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static bool base64_decode(const char* input, char* output, size_t output_len) {
    if (!input || !output) return false;
    
    size_t input_len = strlen(input);
    if (input_len == 0 || input_len % 4 != 0) return false;
    
    size_t output_idx = 0;
    
    for (size_t i = 0; i < input_len; i += 4) {
        if (output_idx + 3 >= output_len) return false;
        
        const char* p1 = strchr(base64_chars, input[i]);
        const char* p2 = strchr(base64_chars, input[i+1]);
        
        if (!p1 || !p2) return false;
        
        uint8_t a = p1 - base64_chars;
        uint8_t b = p2 - base64_chars;
        
        uint8_t c = 0, d = 0;
        
        if (input[i+2] != '=') {
            const char* p3 = strchr(base64_chars, input[i+2]);
            if (!p3) return false;
            c = p3 - base64_chars;
        }
        
        if (input[i+3] != '=') {
            const char* p4 = strchr(base64_chars, input[i+3]);
            if (!p4) return false;
            d = p4 - base64_chars;
        }
        
        output[output_idx++] = (a << 2) | (b >> 4);
        if (input[i+2] != '=') output[output_idx++] = (b << 4) | (c >> 2);
        if (input[i+3] != '=') output[output_idx++] = (c << 6) | d;
    }
    
    output[output_idx] = '\0';
    return true;
}

// ════════════════════════════════════════════════════════════════════════
// NVS AUTH MANAGEMENT
// ════════════════════════════════════════════════════════════════════════

static bool loadAuthFromNVS(char* username, size_t username_len, 
                           char* password, size_t password_len) {
    Preferences prefs;
    if (!prefs.begin("webui_auth", true)) {
        // NVS nicht verfügbar → Fallback zu credentials.h
        #ifdef WEBUI_USERNAME
            strncpy(username, WEBUI_USERNAME, username_len - 1);
            username[username_len - 1] = '\0';
            strncpy(password, WEBUI_PASSWORD, password_len - 1);
            password[password_len - 1] = '\0';
            ESP_LOGI(TAG, "Using credentials from credentials.h (NVS unavailable)");
        #else
            // Default Fallback
            strncpy(username, "admin", username_len - 1);
            username[username_len - 1] = '\0';
            strncpy(password, "admin", password_len - 1);
            password[password_len - 1] = '\0';
            ESP_LOGW(TAG, "⚠️ Using DEFAULT credentials (INSECURE!)");
        #endif
        return false;
    }
    
    String stored_user = prefs.getString("username", "");
    String stored_pass = prefs.getString("password", "");
    
    prefs.end();
    
    if (stored_user.length() == 0 || stored_pass.length() == 0) {
        // NVS leer → Erste Verwendung
        #ifdef WEBUI_USERNAME
            strncpy(username, WEBUI_USERNAME, username_len - 1);
            username[username_len - 1] = '\0';
            strncpy(password, WEBUI_PASSWORD, password_len - 1);
            password[password_len - 1] = '\0';
            
            // In NVS speichern für zukünftige Änderungen
            saveAuthToNVS(username, password);
            
            ESP_LOGI(TAG, "✓ Initialized NVS with credentials from credentials.h");
        #else
            strncpy(username, "admin", username_len - 1);
            username[username_len - 1] = '\0';
            strncpy(password, "admin", password_len - 1);
            password[password_len - 1] = '\0';
            ESP_LOGW(TAG, "⚠️ No credentials defined - using DEFAULT");
        #endif
        return false;
    }
    
    // NVS hat Credentials → Diese verwenden
    strncpy(username, stored_user.c_str(), username_len - 1);
    username[username_len - 1] = '\0';
    strncpy(password, stored_pass.c_str(), password_len - 1);
    password[password_len - 1] = '\0';
    
    ESP_LOGI(TAG, "✓ Loaded credentials from NVS (user: %s)", username);
    return true;
}

static bool saveAuthToNVS(const char* username, const char* password) {
    if (!username || !password) return false;
    
    Preferences prefs;
    if (!prefs.begin("webui_auth", false)) {
        ESP_LOGE(TAG, "Failed to open NVS for auth storage");
        return false;
    }
    
    prefs.putString("username", username);
    prefs.putString("password", password);
    
    prefs.end();
    
    ESP_LOGI(TAG, "✓ Auth credentials saved to NVS (user: %s)", username);
    return true;
}

// ════════════════════════════════════════════════════════════════════════
// HTTP BASIC AUTH CHECK
// ════════════════════════════════════════════════════════════════════════

static bool checkBasicAuth(httpd_req_t *req) {
    // ════════════════════════════════════════════════════════════════════
    // STEP 1: Rate Limit Check
    // ════════════════════════════════════════════════════════════════════
    
    if (failed_login_count >= 5) {
        uint32_t now = millis();
        
        if (now < lockout_until) {
            uint32_t remaining_sec = (lockout_until - now) / 1000;
            
            char msg[128];
            snprintf(msg, sizeof(msg), 
                    "Too many failed attempts. Try again in %u seconds.", 
                    remaining_sec);
            
            ESP_LOGW(TAG, "Rate limit active: %u seconds remaining", remaining_sec);
            
            char retry_after[16];
            snprintf(retry_after, sizeof(retry_after), "%u", remaining_sec);
            httpd_resp_set_hdr(req, "Retry-After", retry_after);
            httpd_resp_send_err(req, HTTPD_429_TOO_MANY_REQUESTS, msg);
            return false;
        } else {
            // Lockout abgelaufen → Reset
            ESP_LOGI(TAG, "Rate limit expired, resetting counter");
            failed_login_count = 0;
            lockout_until = 0;
        }
    }
    
    // ════════════════════════════════════════════════════════════════════
    // STEP 2: Get Authorization Header
    // ════════════════════════════════════════════════════════════════════
    
    char auth_header[256];
    
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        ESP_LOGD(TAG, "No Authorization header found");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BeltWinder Matter\"");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Authentication required");
        
        // Increment failed counter
        failed_login_count++;
        last_failed_login = millis();
        
        if (failed_login_count >= 5) {
            lockout_until = millis() + 60000;
            ESP_LOGW(TAG, "⚠️ Rate limit triggered! Locked out for 60 seconds");
        }
        
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // STEP 3: Check "Basic " prefix
    // ════════════════════════════════════════════════════════════════════
    
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        ESP_LOGW(TAG, "Invalid auth format (expected 'Basic ...')");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BeltWinder Matter\"");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid authentication format");
        
        // Increment failed counter
        failed_login_count++;
        last_failed_login = millis();
        
        if (failed_login_count >= 5) {
            lockout_until = millis() + 60000;
            ESP_LOGW(TAG, "⚠️ Rate limit triggered! Locked out for 60 seconds");
        }
        
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // STEP 4: Load expected credentials
    // ════════════════════════════════════════════════════════════════════
    
    char username[64];
    char password[64];
    loadAuthFromNVS(username, sizeof(username), password, sizeof(password));
    
    // ════════════════════════════════════════════════════════════════════
    // STEP 5: Decode provided credentials
    // ════════════════════════════════════════════════════════════════════
    
    char decoded[128];
    if (!base64_decode(auth_header + 6, decoded, sizeof(decoded))) {
        ESP_LOGW(TAG, "Failed to decode Base64");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BeltWinder Matter\"");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid Base64");
        
        // Increment failed counter
        failed_login_count++;
        last_failed_login = millis();
        
        if (failed_login_count >= 5) {
            lockout_until = millis() + 60000;
            ESP_LOGW(TAG, "⚠️ Rate limit triggered! Locked out for 60 seconds");
        }
        
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // STEP 6: Compare credentials
    // ════════════════════════════════════════════════════════════════════
    
    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s", username, password);
    
    if (strcmp(decoded, expected) == 0) {
        // SUCCESS
        ESP_LOGI(TAG, "✓ Authentication successful (user: %s)", username);
        
        // Reset rate limit counters
        failed_login_count = 0;
        lockout_until = 0;
        
        return true;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // STEP 7: Authentication FAILED
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGW(TAG, "✗ Authentication failed");
    ESP_LOGD(TAG, "  Provided: %s", decoded);
    ESP_LOGD(TAG, "  Expected: %s", expected);
    
    // Increment failed counter
    failed_login_count++;
    last_failed_login = millis();
    
    if (failed_login_count >= 5) {
        lockout_until = millis() + 60000;  // 60 Sekunden Lockout
        ESP_LOGW(TAG, "⚠️ Rate limit triggered! Locked out for 60 seconds");
    } else {
        ESP_LOGW(TAG, "Failed login attempt %d/5", failed_login_count);
    }
    
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BeltWinder Matter\"");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Wrong username or password");
    
    return false;
}


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


// ════════════════════════════════════════════════════════════════════════
// Icon Handler Implementierung
// ════════════════════════════════════════════════════════════════════════

static esp_err_t favicon_handler(httpd_req_t *req) {
    // KEIN Auth-Check für Favicon!
    // Dies verhindert, dass Browser-Requests das Rate Limit triggern
    
    ESP_LOGD(TAG, "Favicon requested (no auth required)");
    
    // Option 1: 204 No Content (schnellste Lösung)
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
    
    /* Option 2: Echtes Icon senden (falls gewünscht)
    // 16x16 transparent PNG als Platzhalter
    const uint8_t favicon_ico[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 
        // ... rest of icon data ...
    };
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char*)favicon_ico, sizeof(favicon_ico));
    return ESP_OK;
    */
}

static esp_err_t apple_touch_icon_handler(httpd_req_t *req) {
    // KEIN Auth-Check für Apple Touch Icons!
    
    ESP_LOGD(TAG, "Apple Touch Icon requested (no auth required)");
    
    // 204 No Content - Browser gibt auf und zeigt Standardicon
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ════════════════════════════════════════════════════════════════════════
// HTTP GET Handler für /update (zeigt aktuellen Status)
// ════════════════════════════════════════════════════════════════════════

static esp_err_t update_get_handler(httpd_req_t *req) {

    // AUTH CHECK
    if (!checkBasicAuth(req)) {
        return ESP_FAIL;
    }

    // Zeige aktuelle Firmware Version + Partition Info
    
    const esp_app_desc_t* app_desc = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    
    char response[512];
    snprintf(response, sizeof(response),
            "Current Firmware:\n"
            "  Version: %s\n"
            "  Date: %s %s\n"
            "  IDF: %s\n\n"
            "Partition Info:\n"
            "  Running: %s (0x%x)\n"
            "  Boot:    %s (0x%x)\n"
            "  Type:    %s\n\n"
            "Upload firmware.bin via POST to /update",
            app_desc->version,
            app_desc->date,
            app_desc->time,
            app_desc->idf_ver,
            running->label,
            running->address,
            boot->label,
            boot->address,
            (running == boot) ? "Same (no pending update)" : "Different (rollback possible)");
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// ════════════════════════════════════════════════════════════════════════
// HTTP POST Handler für /update
// ════════════════════════════════════════════════════════════════════════

static esp_err_t update_post_handler(httpd_req_t *req) {

    // AUTH CHECK
    if (!checkBasicAuth(req)) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   OTA UPDATE VIA HTTP POST        ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // Extrahiere Content-Length aus Header
    size_t content_len = req->content_len;
    
    ESP_LOGI(TAG, "Content-Length: %u bytes (%.2f KB)", 
             content_len, content_len / 1024.0f);
    
    if (content_len == 0 || content_len > (2 * 1024 * 1024)) {
        ESP_LOGE(TAG, "✗ Invalid content length: %u", content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 
                           "Invalid file size (max 2MB)");
        return ESP_FAIL;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Parse Multipart Form Data Header
    // ════════════════════════════════════════════════════════════════════
    
    char boundary[128] = {0};
    size_t boundary_len = 0;
    
    // Lese Content-Type Header
    char content_type[256];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", 
                                    content_type, sizeof(content_type)) == ESP_OK) {
        
        ESP_LOGI(TAG, "Content-Type: %s", content_type);
        
        // Extrahiere Boundary (z.B. "----WebKitFormBoundary...")
        char* boundary_start = strstr(content_type, "boundary=");
        if (boundary_start) {
            boundary_start += 9; // Skip "boundary="
            
            // Kopiere Boundary String
            strncpy(boundary, boundary_start, sizeof(boundary) - 1);
            boundary_len = strlen(boundary);
            
            ESP_LOGI(TAG, "Boundary: %s (len=%u)", boundary, boundary_len);
        } else {
            ESP_LOGE(TAG, "✗ No boundary found in Content-Type");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 
                               "Missing boundary in multipart form");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "✗ No Content-Type header");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 
                           "Missing Content-Type header");
        return ESP_FAIL;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Initialize OTA Update
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Initializing OTA update...");
    
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        ESP_LOGE(TAG, "✗ Update.begin() failed");
        ESP_LOGE(TAG, "  Error: %s", Update.errorString());
        
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                           "OTA init failed");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✓ OTA Update initialized");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Read and Flash Data in Chunks
    // ════════════════════════════════════════════════════════════════════
    
    const size_t BUFFER_SIZE = 1024;
    uint8_t* buffer = (uint8_t*)malloc(BUFFER_SIZE);
    
    if (!buffer) {
        ESP_LOGE(TAG, "✗ Failed to allocate buffer");
        Update.abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                           "Memory allocation failed");
        return ESP_FAIL;
    }
    
    size_t total_received = 0;
    size_t firmware_start = 0;
    size_t firmware_end = 0;
    bool firmware_started = false;
    bool header_parsed = false;
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   RECEIVING & FLASHING DATA       ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    int last_percent = -1;
    
    while (total_received < content_len) {
        // Berechne wie viel wir noch empfangen können
        size_t remaining = content_len - total_received;
        size_t to_recv = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        
        // Empfange Chunk
        int ret = httpd_req_recv(req, (char*)buffer, to_recv);
        
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "⚠ Socket timeout - retrying...");
                continue;
            }
            
            ESP_LOGE(TAG, "✗ Failed to receive data: %d", ret);
            break;
        }
        
        total_received += ret;
        
        // ════════════════════════════════════════════════════════════════
        // Parse Multipart Header (einmalig am Anfang)
        // ════════════════════════════════════════════════════════════════
        
        if (!header_parsed && total_received >= 200) {
            // Suche nach doppeltem CRLF (Ende des Headers)
            for (size_t i = 0; i < ret - 3; i++) {
                if (buffer[i] == '\r' && buffer[i+1] == '\n' &&
                    buffer[i+2] == '\r' && buffer[i+3] == '\n') {
                    
                    firmware_start = i + 4;
                    firmware_started = true;
                    header_parsed = true;
                    
                    ESP_LOGI(TAG, "✓ Multipart header parsed");
                    ESP_LOGI(TAG, "  Firmware data starts at byte: %u", firmware_start);
                    
                    // Schreibe erste Firmware-Bytes
                    size_t first_chunk_size = ret - firmware_start;
                    
                    if (first_chunk_size > 0) {
                        size_t written = Update.write(buffer + firmware_start, first_chunk_size);
                        
                        if (written != first_chunk_size) {
                            ESP_LOGE(TAG, "✗ Write failed: wrote %u of %u bytes", 
                                     written, first_chunk_size);
                            Update.abort();
                            free(buffer);
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                               "Flash write error");
                            return ESP_FAIL;
                        }
                        
                        firmware_end += first_chunk_size;
                    }
                    
                    break;
                }
            }
            
            if (!header_parsed) {
                ESP_LOGW(TAG, "⚠ Header not found yet (received: %u bytes)", total_received);
            }
            
        } else if (firmware_started) {
            // ════════════════════════════════════════════════════════════
            // Schreibe Firmware-Daten
            // ════════════════════════════════════════════════════════════
            
            // Prüfe ob wir am Ende sind (Boundary Footer)
            if (total_received >= content_len - 200) {
                // Suche nach Boundary Ende Marker
                // Format: "\r\n------WebKitFormBoundary...\r\n"
                
                bool found_end = false;
                for (int i = ret - 1; i >= 0; i--) {
                    if (buffer[i] == '-' && i >= 2) {
                        // Prüfe ob das der Footer ist
                        char test[64];
                        snprintf(test, sizeof(test), "--%s", boundary);
                        
                        if (i + strlen(test) <= ret) {
                            if (memcmp(buffer + i, test, strlen(test)) == 0) {
                                // Footer gefunden - schneide ab
                                ret = i - 2; // -2 für \r\n davor
                                found_end = true;
                                ESP_LOGI(TAG, "✓ Found boundary footer at byte %d", i);
                                break;
                            }
                        }
                    }
                }
            }
            
            if (ret > 0) {
                size_t written = Update.write(buffer, ret);
                
                if (written != ret) {
                    ESP_LOGE(TAG, "✗ Write failed: wrote %u of %u bytes", written, ret);
                    Update.abort();
                    free(buffer);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                       "Flash write error");
                    return ESP_FAIL;
                }
                
                firmware_end += ret;
            }
        }
        
        // ════════════════════════════════════════════════════════════════
        // Progress Logging (alle 5%)
        // ════════════════════════════════════════════════════════════════
        
        int percent = (total_received * 100) / content_len;
        
        if (percent != last_percent && percent % 5 == 0) {
            ESP_LOGI(TAG, "📊 Progress: %d%% (%u / %u bytes)", 
                     percent, total_received, content_len);
            last_percent = percent;
        }
        
        // Watchdog Reset (wichtig bei großen Files!)
        esp_task_wdt_reset();
    }
    
    free(buffer);
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   FINALIZING OTA UPDATE           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Total received: %u bytes", total_received);
    ESP_LOGI(TAG, "Firmware size:  %u bytes", firmware_end);
    
    // ════════════════════════════════════════════════════════════════════
    // Finalize and Verify OTA Update
    // ════════════════════════════════════════════════════════════════════
    
    if (Update.end(true)) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "✓✓✓ OTA UPDATE SUCCESSFUL ✓✓✓");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Written:  %u bytes", Update.progress());
        ESP_LOGI(TAG, "Expected: %u bytes", firmware_end);
        ESP_LOGI(TAG, "MD5 Hash: %s", Update.md5String().c_str());
        ESP_LOGI(TAG, "");
        
        // Send Success Response
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Update successful! Rebooting...");
        
        // Short delay to send response
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        ESP_LOGI(TAG, "🔄 Rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        ESP.restart();
        
    } else {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗✗✗ OTA UPDATE FAILED ✗✗✗");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Error Code: %d", Update.getError());
        ESP_LOGE(TAG, "Error String: %s", Update.errorString());
        ESP_LOGI(TAG, "");
        
        Update.abort();
        
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), 
                "Update failed: %s", Update.errorString());
        
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, error_msg);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// ============================================================================
// Static Close Callback
// ============================================================================

static void ws_close_callback(httpd_handle_t hd, int sockfd) {
    ESP_LOGI("WebUI", "Socket %d closed by HTTP server", sockfd);
}

// ============================================================================
// HTTP Root Handler - Serves Web-UI
// ============================================================================

esp_err_t WebUIHandler::root_handler(httpd_req_t *req) {
    // Auth Check
    if (!checkBasicAuth(req)) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Serving Web-UI to client: %s", 
             req->user_ctx ? "authenticated" : "anonymous");
    
    #ifdef SERVE_COMPRESSED_HTML
        // ====================================================================
        // Serve GZIP Compressed HTML
        // ====================================================================
        
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
        
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
        // Auth Check beim Handshake
        if (!checkBasicAuth(req)) {
            return ESP_FAIL;
        }
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
        ESP_LOGI(TAG, "→ Command: START CALIBRATION (from top)");
        bool was_calibrated = shutter_driver_is_calibrated(self->handle);
        esp_err_t result = shutter_driver_start_calibration(self->handle);
        ESP_LOGI(TAG, "← Calibration started | Previously calibrated: %s | Result: %s",
                was_calibrated ? "YES" : "NO",
                result == ESP_OK ? "SUCCESS" : "FAILED");
    }
    else if (strcmp(cmd, "calibrate_from_bottom") == 0) {
        ESP_LOGI(TAG, "→ Command: START CALIBRATION (from bottom / DOWN first)");
        bool was_calibrated = shutter_driver_is_calibrated(self->handle);
        esp_err_t result = shutter_driver_start_calibration_from_bottom(self->handle);
        ESP_LOGI(TAG, "← Calibration (from bottom) started | Previously calibrated: %s | Result: %s",
                was_calibrated ? "YES" : "NO",
                result == ESP_OK ? "SUCCESS" : "FAILED");
    }
    else if (strcmp(cmd, "invert_on") == 0) {
            ESP_LOGI(TAG, "WebUI: Setting direction to INVERTED");
        
        // ════════════════════════════════════════════════════════════════
        // 1. Update Mode Attribut in Matter
        // ════════════════════════════════════════════════════════════════
        
        cluster_t* wc_cluster = cluster::get(window_covering_endpoint_id, 
                                            chip::app::Clusters::WindowCovering::Id);
        if (wc_cluster) {
            attribute_t* mode_attr = attribute::get(wc_cluster, 
                                                chip::app::Clusters::WindowCovering::Attributes::Mode::Id);
            if (mode_attr) {
                // Aktuellen Mode lesen
                esp_matter_attr_val_t current_mode;
                attribute::get_val(mode_attr, &current_mode);
                
                // Bit 0 setzen (MotorDirectionReversed)
                uint8_t new_mode = current_mode.val.u8 | 0x01;
                
                esp_matter_attr_val_t new_val = esp_matter_bitmap8(new_mode);
                
                // Attribut updaten (triggert app_attribute_update_cb!)
                attribute::update(window_covering_endpoint_id,
                                chip::app::Clusters::WindowCovering::Id,
                                chip::app::Clusters::WindowCovering::Attributes::Mode::Id,
                                &new_val);
                
                ESP_LOGI(TAG, "✓ Mode attribute updated: 0x%02X → 0x%02X", 
                        current_mode.val.u8, new_mode);
            }
        }
        
        // ════════════════════════════════════════════════════════════════
        // 2. WebUI Response
        // ════════════════════════════════════════════════════════════════
        
        vTaskDelay(pdMS_TO_TICKS(100));  // Kurz warten bis Update propagiert ist
        
        bool inverted = shutter_driver_get_direction_inverted(self->handle);
        
        // ✅ Response Buffer definieren
        char response[128];
        snprintf(response, sizeof(response), 
                "{\"type\":\"direction\",\"inverted\":%s}",
                inverted ? "true" : "false");
        
        // WebSocket Frame vorbereiten
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t*)response;
        ws_pkt.len = strlen(response);
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        
        httpd_ws_send_frame(req, &ws_pkt);
    }
    else if (strcmp(cmd, "invert_off") == 0) {
            ESP_LOGI(TAG, "WebUI: Setting direction to NORMAL");
        
        // ════════════════════════════════════════════════════════════════
        // 1. Update Mode Attribut in Matter
        // ════════════════════════════════════════════════════════════════
        
        cluster_t* wc_cluster = cluster::get(window_covering_endpoint_id, 
                                            chip::app::Clusters::WindowCovering::Id);
        if (wc_cluster) {
            attribute_t* mode_attr = attribute::get(wc_cluster, 
                                                chip::app::Clusters::WindowCovering::Attributes::Mode::Id);
            if (mode_attr) {
                // Aktuellen Mode lesen
                esp_matter_attr_val_t current_mode;
                attribute::get_val(mode_attr, &current_mode);
                
                // Bit 0 löschen (MotorDirectionReversed)
                uint8_t new_mode = current_mode.val.u8 & ~0x01;
                
                esp_matter_attr_val_t new_val = esp_matter_bitmap8(new_mode);
                
                // Attribut updaten (triggert app_attribute_update_cb!)
                attribute::update(window_covering_endpoint_id,
                                chip::app::Clusters::WindowCovering::Id,
                                chip::app::Clusters::WindowCovering::Attributes::Mode::Id,
                                &new_val);
                
                ESP_LOGI(TAG, "✓ Mode attribute updated: 0x%02X → 0x%02X", 
                        current_mode.val.u8, new_mode);
            }
        }
        
        // ════════════════════════════════════════════════════════════════
        // 2. WebUI Response
        // ════════════════════════════════════════════════════════════════
        
        vTaskDelay(pdMS_TO_TICKS(100));
        
        bool inverted = shutter_driver_get_direction_inverted(self->handle);
        
        // ✅ Response Buffer definieren
        char response[128];
        snprintf(response, sizeof(response), 
                "{\"type\":\"direction\",\"inverted\":%s}",
                inverted ? "true" : "false");
        
        // WebSocket Frame vorbereiten
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t*)response;
        ws_pkt.len = strlen(response);
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        
        httpd_ws_send_frame(req, &ws_pkt);
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
        
        // ✅ CALL COMPLETE FACTORY RESET
        extern void performCompleteFactoryReset();
        performCompleteFactoryReset();
        
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
            "\"deviceType\":\"%s\","
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
    httpd_ws_send_frame(req, &frame);
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
        httpd_ws_send_frame(req, &status_frame);
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

        esp_err_t ret = httpd_ws_send_frame(req, &matter_frame);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send matter_status: %s", esp_err_to_name(ret));
        }
    }
    else if (strcmp(cmd, "matter_open_commissioning") == 0) {
        extern volatile bool matter_stack_started;
        if (!matter_stack_started) {
            const char* err = "{\"type\":\"error\",\"message\":\"Matter stack is not running. Enable Matter first.\"}";
            httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)err, .len = strlen(err) };
            httpd_ws_send_frame_async(req->handle, fd, &f);
        } else if (Matter.isDeviceCommissioned()) {
            const char* err = "{\"type\":\"error\",\"message\":\"Device is already commissioned.\"}";
            httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)err, .len = strlen(err) };
            httpd_ws_send_frame_async(req->handle, fd, &f);
        } else {
            // Release port 5353 before opening the commissioning window.
            //
            // Root cause of ERR_USE (0x3000008):
            //   The Arduino ESP-IDF mDNS library (mdns_networking_lwip.c) creates its
            //   UDP PCB via udp_new() WITHOUT SOF_REUSEADDR.  LwIP udp_bind() returns
            //   ERR_USE whenever ANY existing PCB on the same port lacks SOF_REUSEADDR,
            //   even if the new PCB has it.  Arduino MDNS starts in the 1-second window
            //   after esp_matter::start(), binds port 5353 first, and permanently blocks
            //   CHIP's minimal mDNS from ever binding.
            //
            // Fix: stop Arduino MDNS (releases the PCB), then stop CHIP's advertiser if
            //   it somehow managed to initialize, so Init() gets a fully clean slate.
            MDNS.end();

            chip::DeviceLayer::PlatformMgr().LockChipStack();
            if (chip::Dnssd::ServiceAdvertiser::Instance().IsInitialized()) {
                chip::Dnssd::ServiceAdvertiser::Instance().Shutdown();
            }

            ESP_LOGI(TAG, "→ Opening commissioning window (DNS-SD, 900 s)...");
            CHIP_ERROR cwErr = chip::Server::GetInstance()
                .GetCommissioningWindowManager()
                .OpenBasicCommissioningWindow(
                    chip::System::Clock::Seconds32(900),
                    chip::CommissioningWindowAdvertisement::kDnssdOnly);

            // Generate QR code with kOnNetwork-only rendezvous so Apple Home (iOS)
            // skips BLE discovery and commissions directly over WiFi/DNS-SD.
            // Matter.getOnboardingQRCodeUrl() is hardcoded with kBLE set, which causes
            // iOS to try BLE first, time out, and abort the commissioning session.
            char qrCodeBuf[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {};
            chip::MutableCharSpan qrCodeSpan(qrCodeBuf, sizeof(qrCodeBuf) - 1);
            String qrUrl, qrMt;
            if (GetQRCode(qrCodeSpan,
                          chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork)) == CHIP_NO_ERROR) {
                char urlBuf[192];
                snprintf(urlBuf, sizeof(urlBuf),
                         "https://project-chip.github.io/connectedhomeip/qrcode.html?data=%s",
                         qrCodeBuf);
                qrUrl = String(urlBuf);
                qrMt  = String(qrCodeBuf);
            } else {
                // Fallback to hardcoded value (may include kBLE, but better than nothing)
                qrUrl = Matter.getOnboardingQRCodeUrl();
                int mtIdx = qrUrl.indexOf("MT:");
                qrMt = (mtIdx >= 0) ? qrUrl.substring(mtIdx) : qrUrl;
            }
            String pairingCode = Matter.getManualPairingCode();
            chip::DeviceLayer::PlatformMgr().UnlockChipStack();

            if (cwErr == CHIP_NO_ERROR) {
                ESP_LOGI(TAG, "✓ Commissioning window opened");
                char okBuf[1024];
                snprintf(okBuf, sizeof(okBuf),
                         "{\"type\":\"matter_commissioning_ready\","
                         "\"qr_url\":\"%s\","
                         "\"qr_mt\":\"%s\","
                         "\"pairing_code\":\"%s\"}",
                         qrUrl.c_str(),
                         qrMt.c_str(),
                         pairingCode.c_str());
                // Use synchronous send: okBuf is on the stack and must stay valid
                // during send. httpd_ws_send_frame() sends immediately without queuing.
                httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)okBuf, .len = strlen(okBuf) };
                httpd_ws_send_frame(req, &f);
            } else {
                char errBuf[256];
                snprintf(errBuf, sizeof(errBuf),
                         "{\"type\":\"error\",\"message\":\"Could not open commissioning window: %s\"}",
                         chip::ErrorStr(cwErr));
                httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)errBuf, .len = strlen(errBuf) };
                httpd_ws_send_frame(req, &f);
            }
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

        char version_str[32];
        #ifdef APP_VERSION
            snprintf(version_str, sizeof(version_str), "%s", APP_VERSION);
        #else
            snprintf(version_str, sizeof(version_str), "%s", app->version);
        #endif
        
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
                 version_str,
                 reset_reason_str);
        
        httpd_ws_frame_t info_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)info_buf,
            .len = strlen(info_buf)
        };
        httpd_ws_send_frame_async(req->handle, fd, &info_frame);
    }
    else if (CMD_MATCH(cmd, "{\"cmd\":\"save_device_name\"")) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  SAVE DEVICE NAME COMMAND         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    ESP_LOGI(TAG, "Raw Command: %s", cmd);
    ESP_LOGI(TAG, "Command Length: %d", strlen(cmd));
    ESP_LOGI(TAG, "");
    
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

    auto extractField = [&](const char* key, int keyLen) -> String {
        int start = json.indexOf(key);
        if (start < 0) return "";
        start += keyLen;
        int end = json.indexOf("\"", start);
        if (end < start) return "";
        return json.substring(start, end);
    };

    String room     = extractField("\"room\":\"",     8);
    String type     = extractField("\"type\":\"",     8);
    String position = extractField("\"position\":\"", 12);
    
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
    else if (CMD_MATCH(cmd, "{\"cmd\":\"change_webui_password\"")) {
    String json = String(cmd);
    
    // Parse new username/password
    int userStart = json.indexOf("\"username\":\"") + 12;
    int userEnd = json.indexOf("\"", userStart);
    String new_username = json.substring(userStart, userEnd);
    
    int passStart = json.indexOf("\"password\":\"") + 12;
    int passEnd = json.indexOf("\"", passStart);
    String new_password = json.substring(passStart, passEnd);
    
    // Validate
    if (new_username.length() < 3 || new_password.length() < 6) {
        const char* error = "{\"type\":\"error\",\"message\":\"Username min 3 chars, password min 6 chars\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)error,
            .len = strlen(error)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        free(buf);
        return ESP_OK;
    }
    
    // Save to NVS
    if (saveAuthToNVS(new_username.c_str(), new_password.c_str())) {
        ESP_LOGI(TAG, "✓ WebUI credentials changed");
        ESP_LOGI(TAG, "  New username: %s", new_username.c_str());
        
        const char* success = "{\"type\":\"success\",\"message\":\"Credentials updated! Please log in again.\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)success,
            .len = strlen(success)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);

        // Disconnect all clients after a short delay so the success message
        // reaches the browser before the connection closes.
        // Done in a separate task to avoid blocking the httpd task.
        struct DisconnectParams { WebUIHandler* handler; };
        auto* dp = new DisconnectParams{self};
        xTaskCreate([](void* arg) {
            auto* p = static_cast<DisconnectParams*>(arg);
            vTaskDelay(pdMS_TO_TICKS(2000));
            p->handler->disconnect_all_clients();
            delete p;
            vTaskDelete(NULL);
        }, "ws_dc", 2048, dp, 1, NULL);
        
    } else {
        const char* error = "{\"type\":\"error\",\"message\":\"Failed to save credentials\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)error,
            .len = strlen(error)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
}

    else if (strcmp(cmd, "restart") == 0) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   DEVICE RESTART REQUESTED        ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "User triggered restart via WebUI");
        ESP_LOGI(TAG, "");
        
        // Send confirmation to client
        const char* confirm_msg = 
            "{\"type\":\"info\",\"message\":\"Device restarting...\"}";
        
        httpd_ws_frame_t confirm_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)confirm_msg,
            .len = strlen(confirm_msg)
        };
        httpd_ws_send_frame_async(req->handle, fd, &confirm_frame);
        
        // Wait a moment to ensure message is sent
        vTaskDelay(pdMS_TO_TICKS(500));
        
        ESP_LOGI(TAG, "🔄 Restarting ESP32 in 2 seconds...");
        ESP_LOGI(TAG, "");
        
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Restart
        esp_restart();
        
        // Never reached
    }

    else if (strcmp(cmd, "discover_devices") == 0) {
    ESP_LOGI(TAG, "WebSocket: Device Discovery requested");
    
    WebUIHandler* handler = self;
    
    xTaskCreate([](void* param) {
        WebUIHandler* h = (WebUIHandler*)param;
        
        ESP_LOGI(TAG, "Discovery task started");
        
        // Remove from Watchdog (scan kann lange dauern)
        esp_task_wdt_delete(NULL);
        
        // Run discovery
        h->broadcastDiscoveredDevices();
        
        ESP_LOGI(TAG, "Discovery task complete");

        UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "Task -device_disc- Stack High Water Mark: %u bytes", highWater * sizeof(StackType_t));

        if (highWater < 256) {  // < 1KB frei
            ESP_LOGW(TAG, "⚠️ Stack critically low!");
        }
        
        vTaskDelete(NULL);
        
    }, "device_disc", 8192, handler, 1, NULL); // Stack 8KB (broadcastDiscoveredDevices allocates 2KB json_buf + WiFi scan + mDNS query overhead)
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
            
            BaseType_t taskRet = xTaskCreate([](void *param) {
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

                // Send devices FIRST so the list is populated before the overlay
                // is hidden (ble_scan_complete triggers hideLoading() in the JS).
                if (p->bleManager) {
                    const auto& discovered = p->bleManager->getDiscoveredDevices();

                    if (discovered.size() > 0) {
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

                // Send completion AFTER devices — JS hides loading overlay on this message,
                // revealing the already-populated device list.
                const char *complete_msg = "{\"type\":\"ble_scan_complete\"}";
                p->handler->broadcast_to_all_clients(complete_msg);
                ESP_LOGI(TAG, "✓ Scan complete sent");
                
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
                
            }, "ble_scan_mon", 5120, params, 1, NULL);  // Stack: 5KB (has char json_buf[2048] inside)
            if (taskRet != pdPASS) {
                ESP_LOGE(TAG, "✗ Failed to create ble_scan_mon task");
                delete params;
            }
        }
    }

    else if (strcmp(cmd, "ble_status") == 0) {
    if (self->bleManager) {
        ESP_LOGI(TAG, "BLE status requested");
        
        // ════════════════════════════════════════════════════════════════
        // 1. Discovery List (bleibt unverändert)
        // ════════════════════════════════════════════════════════════════
        
        const auto& discovered = self->bleManager->getDiscoveredDevices();
        
        static const int BLE_BUF_SIZE = 2048;
        char* json_buf = (char*)malloc(BLE_BUF_SIZE);
        if (!json_buf) {
            ESP_LOGE(TAG, "ble_status: out of memory");
            free(buf);
            return ESP_ERR_NO_MEM;
        }
        int offset = snprintf(json_buf, BLE_BUF_SIZE,
                              "{\"type\":\"ble_discovered\",\"devices\":[");

        for (size_t i = 0; i < discovered.size(); i++) {
            offset += snprintf(json_buf + offset, BLE_BUF_SIZE - offset,
                               "%s{\"name\":\"%s\",\"address\":\"%s\",\"rssi\":%d,\"encrypted\":%s}",
                               i > 0 ? "," : "",
                               discovered[i].name.c_str(),
                               discovered[i].address.c_str(),
                               discovered[i].rssi,
                               discovered[i].isEncrypted ? "true" : "false");
        }
        snprintf(json_buf + offset, BLE_BUF_SIZE - offset, "]}");

        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)json_buf,
            .len = strlen(json_buf)
        };
        httpd_ws_send_frame(req, &frame);
        
        // ════════════════════════════════════════════════════════════════
        // 2. Paired Device Status
        // ════════════════════════════════════════════════════════════════
        
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

            bool continuousScanActive = self->bleManager->isContinuousScanEnabled();
            
            offset = snprintf(json_buf, BLE_BUF_SIZE,
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
                // ════════════════════════════════════════════════════════
                // ✅ FIX: Verbesserte seconds_ago Berechnung mit Debug
                // ════════════════════════════════════════════════════════
                
                uint32_t currentMillis = millis();
                int32_t secondsAgoToSend = -1;  // Default: ungültig
                
                ESP_LOGD(TAG, "Time Calculation:");
                ESP_LOGD(TAG, "  Current millis: %u", currentMillis);
                ESP_LOGD(TAG, "  Last update:    %u", sensorData.lastUpdate);
                ESP_LOGD(TAG, "  Has data:       %s", hasData ? "true" : "false");
                ESP_LOGD(TAG, "  Data valid:     %s", sensorData.dataValid ? "true" : "false");
                
                // ✅ Prüfe ob lastUpdate gesetzt wurde
                if (sensorData.lastUpdate > 0) {
                    uint32_t secondsAgo = 0;
                    
                    // Prüfe auf millis() Overflow
                    if (currentMillis >= sensorData.lastUpdate) {
                        // Normal case
                        uint32_t diff = currentMillis - sensorData.lastUpdate;
                        secondsAgo = diff / 1000;
                        
                        ESP_LOGD(TAG, "  Difference:     %u ms", diff);
                        ESP_LOGD(TAG, "  Seconds ago:    %u", secondsAgo);
                        
                    } else {
                        // Overflow (nach ~49 Tagen Uptime)
                        uint32_t millisToOverflow = (0xFFFFFFFF - sensorData.lastUpdate);
                        uint32_t diff = millisToOverflow + currentMillis;
                        secondsAgo = diff / 1000;
                        
                        ESP_LOGW(TAG, "  millis() overflow detected!");
                        ESP_LOGD(TAG, "  Calculated seconds: %u", secondsAgo);
                    }
                    
                    // Sanity check: Nicht älter als 24 Stunden
                    if (secondsAgo <= 86400) {
                        secondsAgoToSend = (int32_t)secondsAgo;
                        ESP_LOGD(TAG, "  ✓ Valid time: %d seconds", secondsAgoToSend);
                    } else {
                        ESP_LOGW(TAG, "  ⚠ Data too old: %u seconds (%.1f hours)", 
                                 secondsAgo, secondsAgo / 3600.0f);
                        secondsAgoToSend = -1;
                    }
                    
                } else {
                    ESP_LOGW(TAG, "  ⚠ lastUpdate is 0 - no valid timestamp");
                    secondsAgoToSend = -1;
                }
                
                ESP_LOGD(TAG, "  → Sending seconds_ago: %d", secondsAgoToSend);
                
                // ════════════════════════════════════════════════════════
                // JSON mit allen Sensor-Daten
                // ════════════════════════════════════════════════════════
                
                offset += snprintf(json_buf + offset, BLE_BUF_SIZE - offset,
                                  "\"valid\":true,"
                                  "\"packet_id\":%d,"
                                  "\"window_open\":%s,"
                                  "\"battery\":%d,"
                                  "\"illuminance\":%u,"
                                  "\"rotation\":%d,"
                                  "\"rssi\":%d,"
                                  "\"has_button_event\":%s,"
                                  "\"button_event\":%d,"
                                  "\"seconds_ago\":%d",  // ← int32_t, kann -1 sein
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
                // Keine Daten verfügbar
                ESP_LOGD(TAG, "No sensor data available");

                offset += snprintf(json_buf + offset, BLE_BUF_SIZE - offset,
                                  "\"valid\":false,"
                                  "\"seconds_ago\":-1");  // ← Explizit -1
            }

            snprintf(json_buf + offset, BLE_BUF_SIZE - offset, "}}");

        } else {
            // Nicht gepairt
            bool isScanActive = self->bleManager ? self->bleManager->isScanActive() : false;
            snprintf(json_buf, BLE_BUF_SIZE,
                    "{\"type\":\"ble_status\","
                    "\"paired\":false,"
                    "\"state\":\"%s\","
                    "\"continuous_scan_active\":%s}",
                    stateStr,
                    isScanActive ? "true" : "false");
        }
        
        // Sende zweite Message (ble_status) — synchronous send, then free heap buffer
        frame.payload = (uint8_t*)json_buf;
        frame.len = strlen(json_buf);
        httpd_ws_send_frame(req, &frame);
        free(json_buf);
    }
}

// ════════════════════════════════════════════════════════════════════════
// BLE SMART CONNECT COMMAND (3-in-1 Workflow)
// ════════════════════════════════════════════════════════════════════════
else if (CMD_MATCH(cmd, "{\"cmd\":\"ble_smart_connect\"")) {
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

                // Broadcast full status to ALL connected clients
                vTaskDelay(pdMS_TO_TICKS(1000));
                p->handler->broadcastBLEStatus();
                
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
            
        }, "ble_smart", 6144, params, 5, NULL);  // Stack: 6KB (NimBLE client ops)
        
        ESP_LOGI(TAG, "✓ Smart Connect task created");
    }
}

// ============================================================================
// BLE Connect Command (Phase 1) - Mit Smart Detection
// ============================================================================
else if (CMD_MATCH(cmd, "{\"cmd\":\"ble_connect\"")) {
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
            
            self->broadcastBLEStatus();

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

else if (CMD_MATCH(cmd, "{\"cmd\":\"ble_encrypt\"")) {
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

                // Broadcast full status to ALL connected clients
                p->handler->broadcastBLEStatus();
            } else {
                const char* error = "{\"type\":\"error\",\"message\":\"Encryption failed. Check passkey!\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000))) {
                    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)error, .len = strlen(error) };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
            }

            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task -ble_enc_task- Stack High Water Mark: %u bytes", highWater * sizeof(StackType_t));

            if (highWater < 256) {  // < 1KB frei
                ESP_LOGW(TAG, "⚠️ Stack critically low!");
            }

            delete p;
            vTaskDelete(NULL);

        }, "ble_enc_task", 6144, params, 1, NULL);  // Stack: 6KB (AES + NimBLE client ops)
    }
}

// ════════════════════════════════════════════════════════════════════════
// BLE ENABLE ENCRYPTION (Phase 2)
// ════════════════════════════════════════════════════════════════════════

else if (CMD_MATCH(cmd, "{\"cmd\":\"ble_enable_encryption\"")) {
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
                
                // Continuous Scan starten
                ESP_LOGI(TAG, "→ Starting continuous scan for sensor data...");
                p->bleManager->startContinuousScan();

                // Broadcast full status to ALL connected clients
                vTaskDelay(pdMS_TO_TICKS(1000));
                p->handler->broadcastBLEStatus();
                
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
            ESP_LOGI(TAG, "Task -ble_enc- Stack High Water Mark: %u bytes", 
                     highWater * sizeof(StackType_t));
            
            if (highWater < 512) {
                ESP_LOGW(TAG, "⚠️ Stack critically low! Consider increasing size.");
            }
            
            ESP_LOGI(TAG, "✓ Encryption task completed");
            
            // unique_ptr wird automatisch freigegeben
            vTaskDelete(NULL);
            
        }, "ble_enc", 6144, params, 5, NULL);  // Stack: 6KB (AES + NimBLE client ops)
        
        ESP_LOGI(TAG, "✓ Encryption task created");
    }
}

// ════════════════════════════════════════════════════════════════════════
// BLE PAIR ALREADY-ENCRYPTED DEVICE (Passkey + Bindkey Known)
// ════════════════════════════════════════════════════════════════════════

else if (CMD_MATCH(cmd, "{\"cmd\":\"ble_pair_encrypted_known\"")) {
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
            
            const auto& discovered = p->bleManager->getDiscoveredDevices();
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
            
            // Broadcast full status to ALL connected clients
            vTaskDelay(pdMS_TO_TICKS(1000));
            p->handler->broadcastBLEStatus();
            
            // High Water Mark Logging
            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task -ble_enc_known- Stack High Water Mark: %u bytes", 
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
            
        }, "ble_enc_known", 4086, params, 5, NULL);  // Stack: 4KB
        
        ESP_LOGI(TAG, "✓ Already-Encrypted pairing task created");
    }
}



else if (CMD_MATCH(cmd, "{\"cmd\":\"ble_unpair\"")) {
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
            // Notify ALL open browser tabs that the device is now unpaired
            self->broadcastBLEStatus();
        }
    }
}

else if (CMD_MATCH(cmd, "{\"cmd\":\"ble_pair\"")) {
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

            // Notify ALL open browser tabs (e.g. multiple phones)
            self->broadcastBLEStatus();

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
            self->broadcastBLEStatus();
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
        
        // Broadcast full status to ALL connected clients
        vTaskDelay(pdMS_TO_TICKS(500));
        self->broadcastBLEStatus();
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
                ESP_LOGI(TAG, "Task -ble_read- Stack High Water Mark: %u bytes", 
                        highWater * sizeof(StackType_t));
                
                if (highWater < 512) {
                    ESP_LOGW(TAG, "⚠️ Stack critically low! Consider increasing size.");
                }
                
                // unique_ptr wird automatisch freigegeben
                vTaskDelete(NULL);
                
            }, "ble_read", 5120, params, 5, NULL);  // Stack: 5KB (GATT read + char json_buf[512])
            
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
        extern volatile bool contact_sensor_matter_enabled;
        extern volatile bool contact_sensor_endpoint_active;
        
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
    else if (strcmp(cmd, "window_logic_status") == 0) {
        WindowLogicConfig cfg = shutter_driver_get_window_logic_config(self->handle);
        WindowState       ws  = shutter_driver_get_window_state(self->handle);

        const char* wsStr = "closed";
        if      (ws == WindowState::PENDING) wsStr = "pending";
        else if (ws == WindowState::TILTED)  wsStr = "tilted";
        else if (ws == WindowState::OPEN)    wsStr = "open";

        char buf2[200];
        snprintf(buf2, sizeof(buf2),
                 "{\"type\":\"window_logic_status\","
                 "\"enabled\":%s,"
                 "\"reed_delay\":%u,"
                 "\"tilt_thresh\":%d,"
                 "\"vent_pos\":%u,"
                 "\"window_state\":\"%s\"}",
                 cfg.enabled      ? "true" : "false",
                 (unsigned)cfg.reedDelayMs,
                 (int)cfg.tiltThreshold,
                 (unsigned)cfg.ventPosition,
                 wsStr);

        httpd_ws_frame_t wl_frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)buf2,
            .len     = strlen(buf2)
        };
        httpd_ws_send_frame_async(req->handle, fd, &wl_frame);
    }
    else if (CMD_MATCH(cmd, "{\"cmd\":\"window_logic_save\"")) {
        String json = String(cmd);

        auto readBool = [&](const char* key, bool def) -> bool {
            String pattern = String("\"") + key + "\":";
            int idx = json.indexOf(pattern);
            if (idx < 0) return def;
            idx += pattern.length();
            while (idx < (int)json.length() && json[idx] == ' ') idx++;
            return json.substring(idx, idx + 4) == "true";
        };
        auto readInt = [&](const char* key, int def) -> int {
            String pattern = String("\"") + key + "\":";
            int idx = json.indexOf(pattern);
            if (idx < 0) return def;
            idx += pattern.length();
            while (idx < (int)json.length() && json[idx] == ' ') idx++;
            return json.substring(idx).toInt();
        };

        WindowLogicConfig cfg = shutter_driver_get_window_logic_config(self->handle);
        cfg.enabled       = readBool("enabled",    cfg.enabled);
        cfg.reedDelayMs   = (uint16_t)readInt("reed_delay",  cfg.reedDelayMs);
        cfg.tiltThreshold = (int16_t) readInt("tilt_thresh", cfg.tiltThreshold);
        cfg.ventPosition  = (uint8_t) readInt("vent_pos",    cfg.ventPosition);

        shutter_driver_set_window_logic_config(self->handle, cfg);

        const char* ok = "{\"type\":\"info\",\"message\":\"Window logic settings saved\"}";
        httpd_ws_frame_t ok_frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)ok,
            .len     = strlen(ok)
        };
        httpd_ws_send_frame_async(req->handle, fd, &ok_frame);
    }
    else {
        ESP_LOGW(TAG, "Unknown command: '%s'", cmd);
    }

    free(buf);
    return ESP_OK;
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

// ════════════════════════════════════════════════════════════════════════
// Authentication Helper (Basic Auth)
// ════════════════════════════════════════════════════════════════════════

bool WebUIHandler::check_basic_auth(httpd_req_t *req) {
    // Get Authorization header length
    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    
    if (auth_len == 0) {
        // No auth header provided
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BeltWinder\"");
        httpd_resp_send(req, "Unauthorized", -1);
        return false;
    }
    
    // Allocate buffer for auth header
    char* auth_header = (char*)malloc(auth_len + 1);
    if (!auth_header) {
        httpd_resp_send_500(req);
        return false;
    }
    
    // Get Authorization header
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, auth_len + 1) != ESP_OK) {
        free(auth_header);
        httpd_resp_send_500(req);
        return false;
    }
    
    // Check if it starts with "Basic "
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        free(auth_header);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Invalid auth", -1);
        return false;
    }
    
    // For Matter API: Accept any credentials (or implement actual check)
    // TODO: Implement proper credential checking if needed
    
    free(auth_header);
    return true;  // ✓ Auth OK
}

// ════════════════════════════════════════════════════════════════════════
// Matter Start Handler (ESP-IDF HTTP Server)
// ════════════════════════════════════════════════════════════════════════

esp_err_t WebUIHandler::handle_start_matter(httpd_req_t *req) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   MATTER START REQUEST (WebUI)    ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Authentication Check
    // ════════════════════════════════════════════════════════════════════
    
    WebUIHandler* handler = (WebUIHandler*)req->user_ctx;
    
    if (!handler->check_basic_auth(req)) {
        ESP_LOGW(TAG, "✗ Unauthorized access attempt");
        return ESP_FAIL;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Initialize and Start Matter
    // ════════════════════════════════════════════════════════════════════
    
    MatterStartResult result = initializeAndStartMatter();
    
    // ════════════════════════════════════════════════════════════════════
    // Build JSON Response
    // ════════════════════════════════════════════════════════════════════
    
    // StaticJsonDocument on stack avoids 2KB heap allocation + fragmentation
    StaticJsonDocument<512> doc;
    doc["success"] = result.success;
    doc["reboot_triggered"] = result.reboot_triggered;
    
    if (result.success) {
        if (result.reboot_triggered) {
            // ════════════════════════════════════════════════════════════
            // CASE 1: Reboot wird ausgelöst
            // ════════════════════════════════════════════════════════════
            
            doc["message"] = "Device will reboot to enable Matter";
            doc["reboot_countdown"] = 3;  // Sekunden bis Reboot
            doc["qr_url"] = "";
            doc["pairing_code"] = "";
            
            ESP_LOGI(TAG, "→ Response: Reboot triggered");
            
        } else if (result.already_commissioned) {
            // ════════════════════════════════════════════════════════════
            // CASE 2: Bereits commissioned (kein Reboot nötig)
            // ════════════════════════════════════════════════════════════
            
            doc["already_commissioned"] = true;
            doc["message"] = result.error_message.c_str();
            doc["fabric_count"] = chip::Server::GetInstance().GetFabricTable().FabricCount();
            doc["qr_url"] = "";
            doc["pairing_code"] = "";
            
            ESP_LOGI(TAG, "→ Response: Already commissioned");
            
        } else {
            // ════════════════════════════════════════════════════════════
            // CASE 3: Matter erfolgreich gestartet (Live-Start)
            // ════════════════════════════════════════════════════════════
            
            doc["already_commissioned"] = false;
            doc["message"] = "Matter commissioning started successfully";
            doc["qr_url"] = result.qr_url.c_str();
            doc["pairing_code"] = result.pairing_code.c_str();
            
            ESP_LOGI(TAG, "→ Response: Matter started (live)");
        }
    } else {
        // ════════════════════════════════════════════════════════════════
        // CASE 4: Fehler
        // ════════════════════════════════════════════════════════════════
        
        doc["error"] = result.error_message.c_str();
        
        ESP_LOGE(TAG, "→ Response: Error - %s", result.error_message.c_str());
    }
    
    String response;
    serializeJson(doc, response);
    
    // ════════════════════════════════════════════════════════════════════
    // Send HTTP Response
    // ════════════════════════════════════════════════════════════════════
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    
    esp_err_t err = httpd_resp_send(req, response.c_str(), response.length());

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ Response sent successfully");
        
        // ════════════════════════════════════════════════════════════════
        // Reboot NACH Response (falls nötig)
        // ════════════════════════════════════════════════════════════════
        
        if (result.reboot_triggered) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "→ Reboot triggered by user");
            ESP_LOGI(TAG, "  Waiting 3 seconds...");
            ESP_LOGI(TAG, "");
            
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            ESP_LOGI(TAG, "→ Rebooting NOW...");
            ESP.restart();
            
            // Code nach restart() wird nicht ausgeführt
        }
    }

    return err;

}

// ════════════════════════════════════════════════════════════════════════
// Matter Status Handler (ESP-IDF HTTP Server)
// ════════════════════════════════════════════════════════════════════════

esp_err_t WebUIHandler::handle_matter_status(httpd_req_t *req) {
    // ════════════════════════════════════════════════════════════════════
    // Authentication Check
    // ════════════════════════════════════════════════════════════════════
    
    WebUIHandler* handler = (WebUIHandler*)req->user_ctx;
    
    if (!handler->check_basic_auth(req)) {
        return ESP_FAIL;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // External declarations (from main.cpp)
    // ════════════════════════════════════════════════════════════════════
    
    extern volatile bool matter_node_created;
    extern volatile bool matter_stack_started;

    // ════════════════════════════════════════════════════════════════════
    // Build JSON Response
    // ════════════════════════════════════════════════════════════════════
    
    // StaticJsonDocument on stack avoids 1KB heap allocation + fragmentation
    StaticJsonDocument<384> doc;

    doc["node_created"] = matter_node_created;
    doc["stack_started"] = matter_stack_started;
    doc["commissioned"] = Matter.isDeviceCommissioned();
    doc["fabric_count"] = chip::Server::GetInstance().GetFabricTable().FabricCount();
    
    if (matter_stack_started && !Matter.isDeviceCommissioned()) {
        String qrUrl = Matter.getOnboardingQRCodeUrl();
        String pairingCode = Matter.getManualPairingCode();
        
        doc["qr_url"] = qrUrl.c_str();
        doc["pairing_code"] = pairingCode.c_str();
    }
    
    String response;
    serializeJson(doc, response);
    
    // ════════════════════════════════════════════════════════════════════
    // Send HTTP Response
    // ════════════════════════════════════════════════════════════════════
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    
    return httpd_resp_send(req, response.c_str(), response.length());
}

void WebUIHandler::begin() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    
    cfg.max_open_sockets = 4;  // 3 WebSocket clients + 1 for pending HTTP requests
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;  // 11 handlers registered: root, 3x icons, ws, 2x update, 2x matter, 2x drift
    cfg.stack_size = 8192;
    cfg.ctrl_port = 32768;
    cfg.close_fn = ws_close_callback;
    cfg.close_fn = nullptr;
    cfg.uri_match_fn = nullptr;
    cfg.keep_alive_enable = false;
    cfg.keep_alive_idle = 0;
    cfg.keep_alive_interval = 0;
    cfg.keep_alive_count = 0;

    if (httpd_start(&server, &cfg) == ESP_OK) {
        
        // ════════════════════════════════════════════════════════════════
        // 1. Root Handler (Web-UI)
        // ════════════════════════════════════════════════════════════════
        
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &root);

        // ════════════════════════════════════════════════════════════════
        // 2. Icons - OHNE Auth (verhindert Rate Limiting!)
        // ════════════════════════════════════════════════════════════════
        
        httpd_uri_t favicon = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_handler,
            .user_ctx  = nullptr
        };
        httpd_register_uri_handler(server, &favicon);
        
        httpd_uri_t apple_icon = {
            .uri       = "/apple-touch-icon.png",
            .method    = HTTP_GET,
            .handler   = apple_touch_icon_handler,
            .user_ctx  = nullptr
        };
        httpd_register_uri_handler(server, &apple_icon);
        
        httpd_uri_t apple_icon_precomp = {
            .uri       = "/apple-touch-icon-precomposed.png",
            .method    = HTTP_GET,
            .handler   = apple_touch_icon_handler,
            .user_ctx  = nullptr
        };
        httpd_register_uri_handler(server, &apple_icon_precomp);
        
        // ════════════════════════════════════════════════════════════════
        // 3. WebSocket Handler - MIT Auth
        // ════════════════════════════════════════════════════════════════
        
        httpd_uri_t ws = {
            .uri          = "/ws",
            .method       = HTTP_GET,
            .handler      = ws_handler,
            .user_ctx     = this,
            .is_websocket = true,
            .handle_ws_control_frames = true
        };
        httpd_register_uri_handler(server, &ws);
        
        // ════════════════════════════════════════════════════════════════
        // 3. OTA Update Handler (GET - Status)
        // ════════════════════════════════════════════════════════════════
        
        httpd_uri_t update_get = {
            .uri       = "/update",
            .method    = HTTP_GET,
            .handler   = update_get_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &update_get);
        
        // ════════════════════════════════════════════════════════════════
        // 4. OTA Update Handler (POST - Upload)
        // ════════════════════════════════════════════════════════════════
        
        httpd_uri_t update_post = {
            .uri       = "/update",
            .method    = HTTP_POST,
            .handler   = update_post_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &update_post);

        // ════════════════════════════════════════════════════════════════════
        // 5. MATTER ROUTES
        // ════════════════════════════════════════════════════════════════════
        
        httpd_uri_t uri_matter_start = {
            .uri       = "/api/matter/start",
            .method    = HTTP_POST,
            .handler   = handle_start_matter,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &uri_matter_start);
        ESP_LOGI(TAG, "✓ Route registered: POST /api/matter/start");

        // GET /api/matter/status
        httpd_uri_t uri_matter_status = {
            .uri       = "/api/matter/status",
            .method    = HTTP_GET,
            .handler   = handle_matter_status,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &uri_matter_status);
        ESP_LOGI(TAG, "✓ Route registered: GET /api/matter/status");   
        
        ESP_LOGI(TAG, "✓ Matter routes registered:");
        ESP_LOGI(TAG, "  POST /api/matter/start");
        ESP_LOGI(TAG, "  GET  /api/matter/status");
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   HTTP SERVER STARTED             ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Registered Endpoints:");
        ESP_LOGI(TAG, "  GET  /         → Web-UI (AUTH)");
        ESP_LOGI(TAG, "  GET  /ws       → WebSocket (AUTH)");
        ESP_LOGI(TAG, "  GET  /favicon.ico → Icon (NO AUTH)");
        ESP_LOGI(TAG, "  GET  /apple-touch-icon*.png → Icons (NO AUTH)");
        ESP_LOGI(TAG, "  GET  /update   → OTA Status (AUTH)");
        ESP_LOGI(TAG, "  POST /update   → OTA Upload (AUTH)");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Max open sockets: %d", cfg.max_open_sockets);
        ESP_LOGI(TAG, "LRU purge: %s", cfg.lru_purge_enable ? "enabled" : "disabled");
        
        #ifdef SERVE_COMPRESSED_HTML
            ESP_LOGI(TAG, "  Web-UI: GZIP compressed (%d bytes)", index_html_gz_len);
        #else
            ESP_LOGI(TAG, "  Web-UI: Uncompressed (fallback mode)");
        #endif
        
        ESP_LOGI(TAG, "");
        
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

        // ════════════════════════════════════════════════════════════════
        // 5. Drift Statistics Handler (GET)
        // ════════════════════════════════════════════════════════════════

        httpd_uri_t drift_get = {
            .uri       = "/api/drift",
            .method    = HTTP_GET,
            .handler   = drift_stats_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &drift_get);

        // ════════════════════════════════════════════════════════════════
        // 6. Drift Reset Handler (POST)
        // ════════════════════════════════════════════════════════════════

        httpd_uri_t drift_reset = {
            .uri       = "/api/drift/reset",
            .method    = HTTP_POST,
            .handler   = drift_reset_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &drift_reset);
    } else {
        ESP_LOGE(TAG, "✗ Failed to start HTTP server");
    }
}

// ============================================================================
// Drift Statistics API Endpoints
// ============================================================================

esp_err_t WebUIHandler::drift_stats_handler(httpd_req_t *req) {
    WebUIHandler* self = (WebUIHandler*)req->user_ctx;
    
    if (!self->handle) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                           "Shutter handle not initialized");
        return ESP_FAIL;
    }
    
    // Hole Drift-Statistiken vom Shutter
    String json = ((RollerShutter*)self->handle)->getDriftStatisticsJson();
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    
    return ESP_OK;
}

esp_err_t WebUIHandler::drift_reset_handler(httpd_req_t *req) {
    WebUIHandler* self = (WebUIHandler*)req->user_ctx;
    
    if (!self->handle) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                           "Shutter handle not initialized");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Resetting drift history via API");
    
    ((RollerShutter*)self->handle)->resetDriftHistory();
    
    const char* success = "{\"success\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, success);
    
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

    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t msg_len = strlen(message);
        
        // ✅ Heap-Allokation für async Send
        char* msg_copy = (char*)malloc(msg_len + 1);
        if (!msg_copy) {
            ESP_LOGE(TAG, "✗ Failed to allocate %u bytes for broadcast", msg_len + 1);
            xSemaphoreGive(client_mutex);
            return;
        }
        strcpy(msg_copy, message);
        
        ESP_LOGI(TAG, "→ Broadcasting to %d clients (%u bytes)", 
                 active_clients.size(), msg_len);
        
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.payload = (uint8_t*)msg_copy;
        ws_pkt.len = msg_len;
        
        // Kopie der Client-FDs
        std::vector<int> target_fds;
        target_fds.reserve(active_clients.size());
        for (const auto& client : active_clients) {
            target_fds.push_back(client.fd);
        }
        xSemaphoreGive(client_mutex);
        
        // ✅ Sende MIT TIMEOUT-KONTROLLE
        int success_count = 0;
        std::vector<int> dead_clients;  // Sammle tote Clients
        
        for (int fd : target_fds) {
            // ✅ Watchdog Reset VOR jedem Send
            esp_task_wdt_reset();
            
            // ✅ Send mit Timeout (max 500ms pro Client)
            uint32_t send_start = millis();
            esp_err_t ret = httpd_ws_send_frame_async(server, fd, &ws_pkt);
            uint32_t send_duration = millis() - send_start;
            
            if (ret == ESP_OK) {
                success_count++;
                
                if (send_duration > 100) {
                    ESP_LOGW(TAG, "⚠ Slow client fd=%d (took %ums)", fd, send_duration);
                }
            } else {
                ESP_LOGW(TAG, "Failed to send to fd=%d: %s", fd, esp_err_to_name(ret));
                
                // ✅ Markiere als tot wenn Fehler
                if (ret == ESP_ERR_INVALID_ARG || ret == ESP_FAIL) {
                    dead_clients.push_back(fd);
                }
            }
            
            // ✅ Abbruch bei zu langer Verzögerung (verhindert Watchdog Timeout)
            if (send_duration > 500) {
                ESP_LOGE(TAG, "✗ Client fd=%d blocked for %ums - aborting broadcast!", 
                         fd, send_duration);
                dead_clients.push_back(fd);
                break;  // Stop sending to other clients
            }
        }
        
        ESP_LOGI(TAG, "✓ Broadcast complete: %d/%d clients", 
                 success_count, target_fds.size());
        
        // ✅ Cleanup tote Clients
        for (int fd : dead_clients) {
            ESP_LOGW(TAG, "→ Removing dead client fd=%d", fd);
            unregister_client(fd);
        }
        
        free(msg_copy);

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
  // WebSocket Broadcast: Full BLE Status to ALL clients
  // Called after any significant BLE state change so every open browser tab
  // reflects the new state without requiring a manual tab switch / reload.
  // ============================================================================

  void WebUIHandler::broadcastBLEStatus() {
    if (!bleManager) return;

    bool paired = bleManager->isPaired();
    ShellyBLEManager::DeviceState devState = bleManager->getDeviceState();

    const char* stateStr = "not_paired";
    if      (devState == ShellyBLEManager::STATE_CONNECTED_UNENCRYPTED) stateStr = "connected_unencrypted";
    else if (devState == ShellyBLEManager::STATE_CONNECTED_ENCRYPTED)   stateStr = "connected_encrypted";

    char msg[512];

    if (!paired) {
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"ble_status\","
                 "\"paired\":false,"
                 "\"state\":\"not_paired\","
                 "\"continuous_scan_active\":%s}",
                 bleManager->isContinuousScanEnabled() ? "true" : "false");
    } else {
        PairedShellyDevice dev = bleManager->getPairedDevice();
        ShellyBLESensorData sd;
        bool hasData = bleManager->getSensorData(sd);

        // Window state from shutter driver
        const char* wsStr = "closed";
        if (handle) {
            WindowState ws = shutter_driver_get_window_state(handle);
            if      (ws == WindowState::PENDING) wsStr = "pending";
            else if (ws == WindowState::TILTED)  wsStr = "tilted";
            else if (ws == WindowState::OPEN)    wsStr = "open";
        }

        // seconds_ago
        int secondsAgo = -1;
        if (hasData && sd.lastUpdate > 0) {
            uint32_t now = millis();
            uint32_t elapsed = (now >= sd.lastUpdate) ? (now - sd.lastUpdate)
                                                      : (0xFFFFFFFF - sd.lastUpdate + now);
            if (elapsed < 86400000u) secondsAgo = (int)(elapsed / 1000);
        }

        // bindkey mask
        bool hasBindkey = dev.bindkey.length() == 32;
        char bindkeyMasked[8] = "None";
        if (hasBindkey) snprintf(bindkeyMasked, sizeof(bindkeyMasked), "%.4s…", dev.bindkey.c_str());

        snprintf(msg, sizeof(msg),
                 "{\"type\":\"ble_status\","
                 "\"paired\":true,"
                 "\"state\":\"%s\","
                 "\"name\":\"%s\","
                 "\"address\":\"%s\","
                 "\"continuous_scan_active\":%s,"
                 "\"sensor_data\":{"
                   "\"valid\":%s,"
                   "\"window_open\":%s,"
                   "\"window_state\":\"%s\","
                   "\"battery\":%d,"
                   "\"rotation\":%d,"
                   "\"illuminance\":%u,"
                   "\"rssi\":%d,"
                   "\"seconds_ago\":%d}}",
                 stateStr,
                 dev.name.c_str(),
                 dev.address.c_str(),
                 bleManager->isContinuousScanEnabled() ? "true" : "false",
                 (hasData && sd.dataValid) ? "true" : "false",
                 (hasData && sd.windowOpen) ? "true" : "false",
                 wsStr,
                 hasData ? sd.battery : 0,
                 hasData ? sd.rotation : 0,
                 hasData ? sd.illuminance : 0u,
                 hasData ? sd.rssi : 0,
                 secondsAgo);
    }

    broadcast_to_all_clients(msg);
    ESP_LOGD(TAG, "broadcastBLEStatus: %s", paired ? "paired" : "not_paired");
  }

  // ============================================================================
  // WebSocket Broadcast: Sensor Data Update
  // ============================================================================

  void WebUIHandler::broadcastSensorDataUpdate(const String& address, const ShellyBLESensorData& data) {
        // Berechne "seconds_ago" mit Validierung
        uint32_t currentMillis = millis();
        uint32_t secondsAgo = 0;
        bool timeValid = false;

        if (data.lastUpdate > 0) {
            if (currentMillis >= data.lastUpdate) {
                secondsAgo = (currentMillis - data.lastUpdate) / 1000;
                timeValid = true;
            } else {
                // millis() overflow (nach ~49 Tagen)
                uint32_t millisToOverflow = (0xFFFFFFFF - data.lastUpdate);
                secondsAgo = (millisToOverflow + currentMillis) / 1000;
                timeValid = true;
            }

            if (secondsAgo > 86400) {
                timeValid = false;
            }
        }

        // Include granular window state from shutter driver
        const char* wsStr = "closed";
        if (handle) {
            WindowState ws = shutter_driver_get_window_state(handle);
            if      (ws == WindowState::PENDING) wsStr = "pending";
            else if (ws == WindowState::TILTED)  wsStr = "tilted";
            else if (ws == WindowState::OPEN)    wsStr = "open";
        }

        char json_buf[560];
        snprintf(json_buf, sizeof(json_buf),
                "{\"type\":\"ble_sensor_update\","
                "\"address\":\"%s\","
                "\"window_open\":%s,"
                "\"window_state\":\"%s\","
                "\"battery\":%d,"
                "\"illuminance\":%u,"
                "\"rotation\":%d,"
                "\"rssi\":%d,"
                "\"packet_id\":%d,"
                "\"has_button_event\":%s,"
                "\"button_event\":%d,"
                "\"seconds_ago\":%d}",
                address.c_str(),
                data.windowOpen ? "true" : "false",
                wsStr,
                data.battery,
                data.illuminance,
                data.rotation,
                data.rssi,
                data.packetId,
                data.hasButtonEvent ? "true" : "false",
                (int)data.buttonEvent,
                timeValid ? (int)secondsAgo : -1);

        ESP_LOGD(TAG, "Sensor update: %s bat=%d%% win=%s pkt=%d",
                 address.c_str(), data.battery,
                 data.windowOpen ? "open" : "closed", data.packetId);
        broadcast_to_all_clients(json_buf);
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

  
void WebUIHandler::disconnect_all_clients() {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ESP_LOGI(TAG, "Disconnecting all clients (%d)", active_clients.size());
        
        for (const auto& client : active_clients) {
            close(client.fd);
        }
        
        active_clients.clear();
        
        xSemaphoreGive(client_mutex);
    }
}

// ════════════════════════════════════════════════════════════════════════
// DEVICE DISCOVERY (HEAP-FREE, ESP32 mDNS API korrigiert)
// ════════════════════════════════════════════════════════════════════════

int WebUIHandler::discoverDevices(DiscoveredDevice* devices, int max_devices) {
    if (!devices || max_devices == 0) {
        return 0;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   DISCOVERING BELTWINDER DEVICES  ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ────────────────────────────────────────────────────────────────────
    // SCHRITT 1: mDNS Query für "beltwinder" Service
    // ────────────────────────────────────────────────────────────────────
    
    ESP_LOGI(TAG, "→ Querying mDNS for 'beltwinder' services...");
    
    // Query für "beltwinder._tcp"
    int mdns_count = MDNS.queryService("beltwinder", "tcp");
    
    ESP_LOGI(TAG, "✓ Found %d beltwinder service(s)", mdns_count);
    
    if (mdns_count == 0) {
        ESP_LOGI(TAG, "ℹ No other BeltWinder devices found");
        return 0;
    }
    
    // ────────────────────────────────────────────────────────────────────
    // SCHRITT 2: WiFi Scan für RSSI (einmal, für alle Geräte)
    // ────────────────────────────────────────────────────────────────────
    
    ESP_LOGI(TAG, "→ WiFi scan for RSSI measurement...");
    
    int16_t n = WiFi.scanNetworks(false, false, false, 300);
    
    if (n < 0) {
        ESP_LOGW(TAG, "⚠ WiFi scan failed, RSSI unavailable");
    } else {
        ESP_LOGI(TAG, "✓ WiFi scan complete (%d networks)", n);
    }
    
    // ────────────────────────────────────────────────────────────────────
    // SCHRITT 3: Process discovered devices
    // ────────────────────────────────────────────────────────────────────
    
    ESP_LOGI(TAG, "→ Processing discovered devices...");
    ESP_LOGI(TAG, "");
    
    int found_count = 0;
    
    for (int i = 0; i < mdns_count && found_count < max_devices; i++) {
        // ────────────────────────────────────────────────────────────────
        // Get Hostname
        // ────────────────────────────────────────────────────────────────
        
        String hostname = MDNS.hostname(i);
        
        // ────────────────────────────────────────────────────────────────
        // Get IP Address (ESP32 mDNS API)
        // ────────────────────────────────────────────────────────────────
        
        IPAddress ip = MDNS.address(i);  // ← ESP32 verwendet address(i), nicht IP(i)!
        
        // Skip if invalid IP
        if (ip == IPAddress(0, 0, 0, 0)) {
            ESP_LOGW(TAG, "  Skipping %s (no IP)", hostname.c_str());
            continue;
        }
        
        // Skip if it's our own IP
        if (ip == WiFi.localIP()) {
            ESP_LOGD(TAG, "  Skipping self: %s", hostname.c_str());
            continue;
        }
        
        // ────────────────────────────────────────────────────────────────
        // Extract TXT Records (room, type)
        // ────────────────────────────────────────────────────────────────
        
        String room = "";
        String type = "";
        
        // ESP32 mDNS: Anzahl der TXT records
        int txt_count = MDNS.numTxt(i);
        
        for (int j = 0; j < txt_count; j++) {
            String key = MDNS.txtKey(i, j);
            String value = MDNS.txt(i, j);
            
            if (key == "room") {
                room = value;
            } else if (key == "type") {
                type = value;
            }
        }
        
        // ────────────────────────────────────────────────────────────────
        // Estimate RSSI from WiFi scan
        // ────────────────────────────────────────────────────────────────
        
        int8_t rssi = -100;  // Default: very weak
        
        if (n > 0) {
            // Verwende aktuelles WiFi RSSI als Schätzung
            // (alle Geräte im gleichen Netzwerk haben ähnliche Werte)
            rssi = WiFi.RSSI();
            
            // Optional: Versuche genaueren Wert durch IP-Matching
            // (funktioniert nur wenn Geräte als WiFi APs sichtbar sind)
            for (int j = 0; j < n; j++) {
                // Compare SSID with hostname (approximation)
                String scan_ssid = WiFi.SSID(j);
                if (scan_ssid.indexOf(hostname) >= 0) {
                    rssi = WiFi.RSSI(j);
                    ESP_LOGD(TAG, "  Found RSSI match for %s: %d dBm", 
                             hostname.c_str(), rssi);
                    break;
                }
            }
        }
        
        // ────────────────────────────────────────────────────────────────
        // Store in static buffer (HEAP-FREE!)
        // ────────────────────────────────────────────────────────────────
        
        DiscoveredDevice* dev = &devices[found_count];
        
        // Hostname
        strncpy(dev->hostname, hostname.c_str(), sizeof(dev->hostname) - 1);
        dev->hostname[sizeof(dev->hostname) - 1] = '\0';
        
        // IP
        snprintf(dev->ip, sizeof(dev->ip), "%s", ip.toString().c_str());
        
        // Room
        strncpy(dev->room, room.c_str(), sizeof(dev->room) - 1);
        dev->room[sizeof(dev->room) - 1] = '\0';
        
        // Type
        strncpy(dev->type, type.c_str(), sizeof(dev->type) - 1);
        dev->type[sizeof(dev->type) - 1] = '\0';
        
        // RSSI
        dev->rssi = rssi;
        
        // Valid
        dev->valid = true;
        
        // Log
        ESP_LOGI(TAG, "  [%d] %s", found_count + 1, dev->hostname);
        ESP_LOGI(TAG, "      IP:   %s", dev->ip);
        ESP_LOGI(TAG, "      Room: %s", dev->room[0] ? dev->room : "(unknown)");
        ESP_LOGI(TAG, "      Type: %s", dev->type[0] ? dev->type : "(unknown)");
        ESP_LOGI(TAG, "      RSSI: %d dBm", dev->rssi);
        ESP_LOGI(TAG, "");
        
        found_count++;
    }
    
    // ────────────────────────────────────────────────────────────────────
    // Cleanup
    // ────────────────────────────────────────────────────────────────────
    
    if (n > 0) {
        WiFi.scanDelete();
    }
    
    ESP_LOGI(TAG, "✓ Discovery complete: Found %d device(s)", found_count);
    ESP_LOGI(TAG, "");
    
    return found_count;
}


// ════════════════════════════════════════════════════════════════════════
// Broadcast Discovered Devices to WebUI
// ════════════════════════════════════════════════════════════════════════

void WebUIHandler::broadcastDiscoveredDevices() {
    ESP_LOGI(TAG, "Broadcasting discovered devices to clients...");
    
    // Heap-Allokation: nur bei Aufruf, gibt 1.3KB permanent RAM frei vs. static-Array
    DiscoveredDevice* devices = new DiscoveredDevice[MAX_DISCOVERED_DEVICES];
    if (!devices) {
        broadcast_to_all_clients("{\"type\":\"device_discovery\",\"devices\":[]}");
        return;
    }
    memset(devices, 0, sizeof(DiscoveredDevice) * MAX_DISCOVERED_DEVICES);

    // Discover
    int count = discoverDevices(devices, MAX_DISCOVERED_DEVICES);
    
    if (count == 0) {
        delete[] devices;
        broadcast_to_all_clients("{\"type\":\"device_discovery\",\"devices\":[]}");
        return;
    }

    char* json_buf = new char[2048];
    if (!json_buf) {
        delete[] devices;
        broadcast_to_all_clients("{\"type\":\"device_discovery\",\"devices\":[]}");
        return;
    }
    int offset = 0;

    offset += snprintf(json_buf + offset, 2048 - offset,
                      "{\"type\":\"device_discovery\",\"devices\":[");

    bool first = true;
    for (int i = 0; i < count && i < MAX_DISCOVERED_DEVICES; i++) {
        if (!devices[i].valid) continue;

        offset += snprintf(json_buf + offset, 2048 - offset,
                          "%s{\"hostname\":\"%s\",\"ip\":\"%s\",\"room\":\"%s\",\"type\":\"%s\",\"rssi\":%d}",
                          first ? "" : ",",
                          devices[i].hostname,
                          devices[i].ip,
                          devices[i].room,
                          devices[i].type,
                          devices[i].rssi);
        first = false;
    }

    snprintf(json_buf + offset, 2048 - offset, "]}");

    delete[] devices;

    broadcast_to_all_clients(json_buf);
    delete[] json_buf;

    ESP_LOGI(TAG, "✓ Broadcasted %d devices", count);
}

