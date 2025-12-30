#include "wifi_manager.h"
#include <Preferences.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <DNSServer.h>

static const char* TAG = "WiFiMgr";

// ════════════════════════════════════════════════════════════════
// WiFi Credentials Namespace
// ════════════════════════════════════════════════════════════════

namespace WiFiCredentials {
    bool load(char* ssid, size_t ssid_len, char* password, size_t pass_len) {
        Preferences prefs;
        if (!prefs.begin("wifi", true)) {  // Read-only
            return false;
        }
        
        String ssid_str = prefs.getString("ssid", "");
        String pass_str = prefs.getString("password", "");
        
        prefs.end();
        
        if (ssid_str.length() == 0) {
            return false;
        }
        
        strncpy(ssid, ssid_str.c_str(), ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
        
        strncpy(password, pass_str.c_str(), pass_len - 1);
        password[pass_len - 1] = '\0';
        
        return true;
    }
    
    void save(const char* ssid, const char* password) {
        Preferences prefs;
        prefs.begin("wifi", false);  // Read-write
        prefs.putString("ssid", ssid);
        prefs.putString("password", password);
        prefs.end();
        
        ESP_LOGI(TAG, "✓ WiFi credentials saved to NVS");
    }
    
    void clear() {
        Preferences prefs;
        prefs.begin("wifi", false);
        prefs.clear();
        prefs.end();
        
        ESP_LOGI(TAG, "WiFi credentials cleared");
    }
    
    bool exists() {
        Preferences prefs;
        if (!prefs.begin("wifi", true)) {
            return false;
        }
        
        bool has_ssid = prefs.isKey("ssid");
        prefs.end();
        
        return has_ssid;
    }
}

// ════════════════════════════════════════════════════════════════
// WiFiManager Implementation
// ════════════════════════════════════════════════════════════════

bool WiFiManager::needsSetup() {
    return !WiFiCredentials::exists();
}

bool WiFiManager::runSetup(const char* ap_ssid, uint32_t timeout_ms) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   WiFi SETUP MODE                 ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Starting Access Point...");
    ESP_LOGI(TAG, "  SSID: %s", ap_ssid);
    ESP_LOGI(TAG, "  IP:   192.168.4.1");
    ESP_LOGI(TAG, "  Timeout: %lu seconds", timeout_ms / 1000);
    ESP_LOGI(TAG, "");
    
    // ────────────────────────────────────────────────────────────
    // 1. Start Access Point
    // ────────────────────────────────────────────────────────────
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid);
    
    IPAddress ip = WiFi.softAPIP();
    ESP_LOGI(TAG, "✓ AP started: %s", ip.toString().c_str());
    
    // ────────────────────────────────────────────────────────────
    // 2. DNS Server für Captive Portal
    // ────────────────────────────────────────────────────────────
    
    DNSServer dnsServer;
    dnsServer.start(53, "*", ip);
    
    ESP_LOGI(TAG, "✓ DNS server started (captive portal)");
    
    // ────────────────────────────────────────────────────────────
    // 3. HTTP Server
    // ────────────────────────────────────────────────────────────
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 8;
    config.stack_size = 4096;
    
    httpd_handle_t server = nullptr;
    
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "✗ Failed to start HTTP server");
        WiFi.softAPdisconnect(true);
        return false;
    }
    
    // Register handlers
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handleRoot,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &root_uri);
    
    httpd_uri_t scan_uri = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = handleScan,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &scan_uri);
    
    httpd_uri_t connect_uri = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = handleConnect,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &connect_uri);
    
    ESP_LOGI(TAG, "✓ HTTP server started");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Setup Instructions:");
    ESP_LOGI(TAG, "1. Connect to WiFi: %s", ap_ssid);
    ESP_LOGI(TAG, "2. Open browser (should auto-redirect)");
    ESP_LOGI(TAG, "3. Or visit: http://192.168.4.1");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    
    // ────────────────────────────────────────────────────────────
    // 4. Wait Loop
    // ────────────────────────────────────────────────────────────
    
    uint32_t start_time = millis();
    bool success = false;
    
    while (millis() - start_time < timeout_ms) {
        dnsServer.processNextRequest();
        
        if (WiFiCredentials::exists()) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✓ WiFi credentials received!");
            success = true;
            break;
        }
        
        delay(10);
    }
    
    // ════════════════════════════════════════════════════════════════
    // 5. KRITISCH: Cleanup - WiFi komplett zurücksetzen!
    // ════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   RESETTING WiFi STACK            ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // 1. Stop HTTP Server
    httpd_stop(server);
    ESP_LOGI(TAG, "  ✓ HTTP server stopped");
    
    // 2. Stop DNS Server
    dnsServer.stop();
    ESP_LOGI(TAG, "  ✓ DNS server stopped");
    
    // 3. Disconnect AP
    WiFi.softAPdisconnect(true);
    ESP_LOGI(TAG, "  ✓ AP disconnected");
    
    // 4. WiFi stoppen (auf ESP-IDF Ebene!)
    esp_err_t ret = esp_wifi_stop();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "  ✓ WiFi stopped");
    } else {
        ESP_LOGW(TAG, "  ⚠ esp_wifi_stop: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 5. WiFi deinitialisieren!
    ret = esp_wifi_deinit();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "  ✓ WiFi driver deinitialized");
    } else {
        ESP_LOGW(TAG, "  ⚠ esp_wifi_deinit: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 6. Arduino WiFi Wrapper zurücksetzen
    WiFi.mode(WIFI_OFF);
    ESP_LOGI(TAG, "  ✓ WiFi mode set to OFF");
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 7. Disconnect (nochmal zur Sicherheit)
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 8. Lange Wartezeit für Hardware-Reset!
    ESP_LOGI(TAG, "  → Waiting for WiFi hardware to reset...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ WiFi stack fully reset");
    ESP_LOGI(TAG, "");
    
    if (success) {
        ESP_LOGI(TAG, "→ Rebooting to apply WiFi settings...");
        ESP_LOGI(TAG, "");
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP.restart();
    } else {
        ESP_LOGW(TAG, "⚠ Timeout - no credentials received");
    }
    
    return success;
}

// ════════════════════════════════════════════════════════════════
// HTTP Handlers
// ════════════════════════════════════════════════════════════════

esp_err_t WiFiManager::handleRoot(httpd_req_t* req) {
    // Minimal HTML (inline, kein SPIFFS!)
    const char* html = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>BeltWinder WiFi Setup</title>
    <style>
        body { font-family: Arial; margin: 20px; background: #f0f0f0; }
        .container { max-width: 400px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #2196F3; text-align: center; }
        .network { padding: 10px; margin: 5px 0; background: #f5f5f5; border-radius: 5px; cursor: pointer; display: flex; justify-content: space-between; }
        .network:hover { background: #e0e0e0; }
        .network.selected { background: #2196F3; color: white; }
        .signal { font-weight: bold; }
        input { width: 100%; padding: 10px; margin: 5px 0; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; }
        button { width: 100%; padding: 12px; background: #2196F3; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; }
        button:hover { background: #1976D2; }
        button:disabled { background: #ccc; cursor: not-allowed; }
        #status { margin-top: 10px; padding: 10px; border-radius: 5px; display: none; }
        .success { background: #4CAF50; color: white; }
        .error { background: #f44336; color: white; }
        .loading { text-align: center; color: #666; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🌐 WiFi Setup</h1>
        <div class="loading" id="loading">Scanning networks...</div>
        <div id="networks"></div>
        <div id="form" style="display: none;">
            <h3>Connect to: <span id="selected-ssid"></span></h3>
            <input type="password" id="password" placeholder="WiFi Password" />
            <button onclick="connect()">Connect</button>
        </div>
        <div id="status"></div>
    </div>
    <script>
        let selectedSSID = '';
        
        async function scanNetworks() {
            try {
                const response = await fetch('/scan');
                const data = await response.json();
                
                document.getElementById('loading').style.display = 'none';
                
                const container = document.getElementById('networks');
                container.innerHTML = '';
                
                                data.networks.forEach(network => {
                    const div = document.createElement('div');
                    div.className = 'network';
                    div.onclick = () => selectNetwork(network.ssid, div);
                    
                    const ssid = document.createElement('span');
                    ssid.textContent = network.ssid;
                    
                    const signal = document.createElement('span');
                    signal.className = 'signal';
                    signal.textContent = getSignalIcon(network.rssi);
                    
                    div.appendChild(ssid);
                    div.appendChild(signal);
                    container.appendChild(div);
                });
                
            } catch (error) {
                showStatus('Scan failed: ' + error.message, 'error');
            }
        }
        
        function getSignalIcon(rssi) {
            if (rssi > -50) return '📶 Excellent';
            if (rssi > -60) return '📶 Good';
            if (rssi > -70) return '📶 Fair';
            return '📶 Weak';
        }
        
        function selectNetwork(ssid, element) {
            // Deselect all
            document.querySelectorAll('.network').forEach(el => {
                el.classList.remove('selected');
            });
            
            // Select clicked
            element.classList.add('selected');
            selectedSSID = ssid;
            
            // Show form
            document.getElementById('selected-ssid').textContent = ssid;
            document.getElementById('form').style.display = 'block';
            document.getElementById('password').focus();
        }
        
        async function connect() {
            if (!selectedSSID) {
                showStatus('Please select a network', 'error');
                return;
            }
            
            const password = document.getElementById('password').value;
            
            if (!password) {
                showStatus('Please enter password', 'error');
                return;
            }
            
            showStatus('Connecting...', 'loading');
            
            try {
                const response = await fetch('/connect', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: `ssid=${encodeURIComponent(selectedSSID)}&password=${encodeURIComponent(password)}`
                });
                
                const data = await response.json();
                
                if (data.success) {
                    showStatus('✓ Connected! Device will reboot...', 'success');
                    setTimeout(() => {
                        window.location.href = 'http://' + data.ip;
                    }, 3000);
                } else {
                    showStatus('Connection failed: ' + data.error, 'error');
                }
                
            } catch (error) {
                showStatus('Error: ' + error.message, 'error');
            }
        }
        
        function showStatus(message, type) {
            const status = document.getElementById('status');
            status.textContent = message;
            status.className = type;
            status.style.display = 'block';
        }
        
        // Start scan on load
        scanNetworks();
    </script>
</body>
</html>
)html";
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    
    return ESP_OK;
}

esp_err_t WiFiManager::handleScan(httpd_req_t* req) {
    ESP_LOGI(TAG, "Scanning networks...");
    
    // ────────────────────────────────────────────────────────────
    // Scan networks (blockiert ~3 Sekunden)
    // ────────────────────────────────────────────────────────────
    
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    
    ESP_LOGI(TAG, "Found %d networks", n);
    
    // ────────────────────────────────────────────────────────────
    // Build JSON (auf Stack, max 2KB)
    // ────────────────────────────────────────────────────────────
    
    char json[2048];
    int offset = 0;
    
    offset += snprintf(json + offset, sizeof(json) - offset, "{\"networks\":[");
    
    for (int i = 0; i < n && i < 20; i++) {  // Max 20 networks
        if (i > 0) {
            offset += snprintf(json + offset, sizeof(json) - offset, ",");
        }
        
        String ssid = WiFi.SSID(i);
        int32_t rssi = WiFi.RSSI(i);
        wifi_auth_mode_t encryption = WiFi.encryptionType(i);
        
        // Escape SSID (simple, nur Quotes)
        ssid.replace("\"", "\\\"");
        
        offset += snprintf(json + offset, sizeof(json) - offset,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"encryption\":%d}",
            ssid.c_str(), rssi, (int)encryption
        );
    }
    
    offset += snprintf(json + offset, sizeof(json) - offset, "],\"count\":%d}", n);
    
    WiFi.scanDelete();
    WiFi.mode(WIFI_AP);  // Back to AP mode
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, offset);
    
    return ESP_OK;
}

esp_err_t WiFiManager::handleConnect(httpd_req_t* req) {
    // ────────────────────────────────────────────────────────────
    // Parse POST data (auf Stack!)
    // ────────────────────────────────────────────────────────────
    
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    
    if (ret <= 0) {
        const char* error_json = "{\"success\":false,\"error\":\"No data received\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_json, strlen(error_json));
        return ESP_OK;
    }
    
    content[ret] = '\0';
    
    ESP_LOGI(TAG, "Received connect request: %s", content);
    
    // ────────────────────────────────────────────────────────────
    // Extract SSID and Password
    // ────────────────────────────────────────────────────────────
    
    char ssid[64] = {0};
    char password[64] = {0};
    
    // Simple parsing (ssid=XXX&password=YYY)
    char* ssid_start = strstr(content, "ssid=");
    char* pass_start = strstr(content, "password=");
    
    if (!ssid_start || !pass_start) {
        const char* error_json = "{\"success\":false,\"error\":\"Invalid format\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_json, strlen(error_json));
        return ESP_OK;
    }
    
    ssid_start += 5;  // Skip "ssid="
    char* ssid_end = strchr(ssid_start, '&');
    if (ssid_end) {
        int len = ssid_end - ssid_start;
        if (len > 63) len = 63;
        strncpy(ssid, ssid_start, len);
        ssid[len] = '\0';
    }
    
    pass_start += 9;  // Skip "password="
    strncpy(password, pass_start, 63);
    password[63] = '\0';
    
    // URL decode (simple: nur %XX)
    // TODO: Full URL decode wenn nötig
    
    ESP_LOGI(TAG, "Attempting to connect to: %s", ssid);
    
    // ────────────────────────────────────────────────────────────
    // Test WiFi connection
    // ────────────────────────────────────────────────────────────
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        timeout++;
        ESP_LOGI(TAG, "Connecting... (%d/20)", timeout);
    }
    
    char response[256];
    
    if (WiFi.status() == WL_CONNECTED) {
        ESP_LOGI(TAG, "✓ Connected successfully!");
        ESP_LOGI(TAG, "  IP: %s", WiFi.localIP().toString().c_str());
        
        // Save credentials
        WiFiCredentials::save(ssid, password);
        
        snprintf(response, sizeof(response),
            "{\"success\":true,\"ip\"%s\"}",
            WiFi.localIP().toString().c_str()
        );
        
    } else {
        ESP_LOGE(TAG, "✗ Connection failed!");
        
        snprintf(response, sizeof(response),
            "{\"success\":false,\"error\":\"Connection timeout\"}"
        );
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    
    return ESP_OK;
}

