#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>

// ════════════════════════════════════════════════════════════════
// Minimal WiFi Manager - ZERO heap im Betrieb!
// 
// Verwendet werden:
// - Stack-Variablen (werden automatisch freigegeben)
// - Statische Buffers (nur während AP-Modus)
// - NVS für Persistenz
// 
// Nach erfolgreichem Setup: REBOOT → Manager wird nicht mehr geladen!
// ════════════════════════════════════════════════════════════════

class WiFiManager {
public:
    // ────────────────────────────────────────────────────────────
    // Prüfe ob WiFi Setup nötig ist
    // ────────────────────────────────────────────────────────────
    static bool needsSetup();
    
    // ────────────────────────────────────────────────────────────
    // Starte WiFi Setup (blockiert bis Setup fertig oder Timeout)
    // Gibt true zurück wenn erfolgreich konfiguriert
    // ────────────────────────────────────────────────────────────
    static bool runSetup(const char* ap_ssid = "BeltWinder-Setup", 
                        uint32_t timeout_ms = 300000);  // 5 Min Timeout
    
private:
    // HTTP Handler (statisch, kein Member-Zugriff nötig)
    static esp_err_t handleRoot(httpd_req_t* req);
    static esp_err_t handleScan(httpd_req_t* req);
    static esp_err_t handleConnect(httpd_req_t* req);
    
    // Helper
    static void saveCredentials(const char* ssid, const char* password);
    static String scanNetworks();
};

// ════════════════════════════════════════════════════════════════
// WiFi Credentials Storage (NVS)
// ════════════════════════════════════════════════════════════════

namespace WiFiCredentials {
    bool load(char* ssid, size_t ssid_len, char* password, size_t pass_len);
    void save(const char* ssid, const char* password);
    void clear();
    bool exists();
}

#endif // WIFI_MANAGER_H
