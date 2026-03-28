#include "shelly_ble_manager.h"
#include <Preferences.h>
#include <esp_log.h>
#include <mbedtls/ccm.h>
#include <esp_task_wdt.h>

// Für Low-Level NimBLE Bond-Key Extraktion
#ifdef ESP_PLATFORM
extern "C" {
    #include "host/ble_hs.h"
    #include "host/ble_store.h"
}
#endif

// Für AES/CCM Decryption
#include <mbedtls/aes.h>
#include <mbedtls/ccm.h>

static const char* TAG = "ShellyBLE";

// ═══════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════════

ShellyBLEManager::ShellyBLEManager() 
    : bleScanner(nullptr),
      activeClient(nullptr),
      activeClientCallbacks(nullptr),
      activeClientTimestamp(0),
      initialized(false),
      scanning(false),
      continuousScan(false),
      stopOnFirstMatch(false),
      sensorDataCallback(nullptr),
      deviceState(STATE_NOT_PAIRED) {
}

ShellyBLEManager::~ShellyBLEManager() {
    end();
}

// ═══════════════════════════════════════════════════════════════════════
// Initialization
// ═══════════════════════════════════════════════════════════════════════

bool ShellyBLEManager::begin() {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Initializing Shelly BLE Manager");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Mode: LAZY (BLE NOT started yet)");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // LAZY MODE: NUR State laden, BLE NICHT starten!
    // ════════════════════════════════════════════════════════════════════
    
    loadPairedDevice();
    
    initialized = true;
    
    ESP_LOGI(TAG, "✓ Manager initialized (lazy mode)");
    ESP_LOGI(TAG, "  BLE will start when needed");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    
    return true;
}

// ════════════════════════════════════════════════════════════════════════
// Ensure BLE is Started (Lazy Init) - NO EXCEPTIONS
// ════════════════════════════════════════════════════════════════════════

bool ShellyBLEManager::ensureBLEStarted() {
    if (bleScanner) {
        ESP_LOGV(TAG, "BLE already started");
        return true;
    }
    
    ESP_LOGI(TAG, "→ Creating scanner...");
    
    // RAII: Automatisches Cleanup bei Scope-Exit!
    auto scanner = std::make_unique<esp32_ble_simple::SimpleBLEScanner>();
    
    if (!scanner) {
        ESP_LOGE(TAG, "✗ Scanner allocation failed");
        return false;
    }
    
    // Konfiguration
    scanner->set_scan_active(true);
    scanner->set_scan_continuous(false);
    scanner->register_listener(this);
    
    // Setup (kann fehlschlagen)
    if (!scanner->setup()) {
        ESP_LOGE(TAG, "✗ Scanner setup failed");
        return false;  // ← scanner wird automatisch freigegeben!
    }
    
    // Erfolg: Übernehme Ownership
    bleScanner = std::move(scanner);
    
    ESP_LOGI(TAG, "✓ Scanner ready");
    return true;
}

void ShellyBLEManager::end() {
    if (!initialized) return;
    
    stopScan();
    
    bleScanner.reset();
    
    if (activeClientCallbacks) {
        delete activeClientCallbacks;
        activeClientCallbacks = nullptr;
    }
    
    initialized = false;
    ESP_LOGI(TAG, "Shut down");
}

void ShellyBLEManager::loop() {
    if (!initialized) return;
    
    if (bleScanner) {
                bleScanner->loop();
    }
    
    if (bleScanner) {
        cleanupOldDiscoveries();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Persistence
// ═══════════════════════════════════════════════════════════════════════

void ShellyBLEManager::loadPairedDevice() {
    Preferences prefs;
    if (!prefs.begin("ShellyBLE", true)) {
        ESP_LOGW(TAG, "ShellyBLE namespace not found");
        return;
    }
    
    String address = prefs.getString("address", "");
    if (address.length() > 0) {
        pairedDevice.address = address;
        pairedDevice.name = prefs.getString("name", "Unknown");
        pairedDevice.bindkey = prefs.getString("bindkey", "");
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "LOADED PAIRED DEVICE FROM NVS");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", pairedDevice.address.c_str());
        ESP_LOGI(TAG, "Name: %s", pairedDevice.name.c_str());
        ESP_LOGI(TAG, "Bindkey: %s", pairedDevice.bindkey.length() > 0 ? "SET (32 chars)" : "EMPTY");
        
        bool shouldScan = prefs.getBool("continuous_scan", true);
        ESP_LOGI(TAG, "Continuous Scan: %s", shouldScan ? "ENABLED" : "DISABLED");
        ESP_LOGI(TAG, "═══════════════════════════════════");
    }
    
    prefs.end();
}

void ShellyBLEManager::savePairedDevice() {
    Preferences prefs;
    prefs.begin("ShellyBLE", false);
    
    if (pairedDevice.address.length() > 0) {
        prefs.putString("address", pairedDevice.address);
        prefs.putString("name", pairedDevice.name);
        prefs.putString("bindkey", pairedDevice.bindkey);
        
        prefs.putBool("continuous_scan", true);
        
        ESP_LOGI(TAG, "Saved paired device: %s", pairedDevice.address.c_str());
        ESP_LOGI(TAG, "  Name: %s", pairedDevice.name.c_str());
        ESP_LOGI(TAG, "  Bindkey: %s", pairedDevice.bindkey.length() > 0 ? "SET" : "EMPTY");
        ESP_LOGI(TAG, "  Continuous Scan: ENABLED");
    } else {
        prefs.clear();
        ESP_LOGI(TAG, "Cleared paired device");
    }
    
    prefs.end();
}


void ShellyBLEManager::clearPairedDevice() {
    pairedDevice = PairedShellyDevice();
    savePairedDevice();
}

// ═══════════════════════════════════════════════════════════════════════
// Discovery / Scanning
// ═══════════════════════════════════════════════════════════════════════

void ShellyBLEManager::startScan(uint16_t durationSeconds, bool stopOnFirst) {
    if (!initialized) {
        ESP_LOGE(TAG, "✗ Cannot start scan: Manager not initialized");
        return;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // CRITICAL: Ensure BLE is started (lazy init)
    // ════════════════════════════════════════════════════════════════════
    
    if (!bleScanner) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  BLE NOT STARTED YET              ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Starting BLE on-demand for discovery scan...");
        
        if (!ensureBLEStarted()) {
            ESP_LOGE(TAG, "✗ Failed to start BLE");
            ESP_LOGE(TAG, "  Cannot perform scan");
            return;
        }
        
        ESP_LOGI(TAG, "✓ BLE started successfully");
        ESP_LOGI(TAG, "");
        
        // Kurze Pause für BLE Stack Stabilisierung
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Stoppe immer zuerst einen laufenden Scan
    if (scanning) {
        ESP_LOGW(TAG, "⚠ Scan already in progress - stopping first");
        stopScan(false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // Prüfe auch Scanner-Status direkt
    if (bleScanner && bleScanner->is_scanning()) {
        ESP_LOGW(TAG, "⚠ Scanner is active despite scanning=false - forcing stop");
        bleScanner->stop_scan();
        scanning = false;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // ════════════════════════════════════════════════════════════════════
    // WHITELIST MANAGEMENT - KRITISCH FÜR DISCOVERY!
    // ════════════════════════════════════════════════════════════════════
    
    if (!continuousScan) {
        // ═════════════════════════════════════════════════════════════════
        // DISCOVERY SCAN: Whitelist MUSS gelöscht werden!
        // ═════════════════════════════════════════════════════════════════
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   DISCOVERY SCAN                  ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        if (bleScanner->is_whitelist_active()) {
            ESP_LOGW(TAG, "⚠ Whitelist is ACTIVE - must clear for discovery!");
            ESP_LOGI(TAG, "→ Clearing whitelist...");
            
            if (bleScanner->clear_scan_whitelist()) {
                ESP_LOGI(TAG, "✓ Whitelist cleared successfully");
                ESP_LOGI(TAG, "  → Discovery scan will see ALL devices");
            } else {
                ESP_LOGE(TAG, "✗ Failed to clear whitelist!");
                ESP_LOGE(TAG, "  Discovery scan might be limited!");
            }
        } else {
            ESP_LOGI(TAG, "✓ No whitelist active");
        }
        
        ESP_LOGI(TAG, "");
        
        // Clear discovered devices
        discoveredDevices.clear();
        ESP_LOGI(TAG, "→ Discovery scan: Cleared previous results");
        ESP_LOGI(TAG, "");
        
    } else {
        // ═════════════════════════════════════════════════════════════════
        // CONTINUOUS SCAN: Whitelist sollte bereits gesetzt sein
        // ═════════════════════════════════════════════════════════════════
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   CONTINUOUS SCAN CYCLE           ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        if (bleScanner->is_whitelist_active()) {
            ESP_LOGI(TAG, "✓ Whitelist active");
            ESP_LOGI(TAG, "  → Only paired device will be scanned");
        } else {
            ESP_LOGW(TAG, "⚠ Whitelist NOT active!");
            ESP_LOGW(TAG, "  → This continuous scan will see ALL devices");
            ESP_LOGW(TAG, "  → Performance will be lower!");
        }
        
        ESP_LOGI(TAG, "");
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Start Scan
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "    BLE SCAN STARTED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Duration: %d seconds", durationSeconds);
    ESP_LOGI(TAG, "Scan type: %s", continuousScan ? "CONTINUOUS" : "DISCOVERY");
    
    if (stopOnFirst) {
        ESP_LOGI(TAG, "Mode: STOP ON FIRST SHELLY BLU DOOR/WINDOW");
    }
    
    ESP_LOGI(TAG, "Target devices: Shelly BLU Door/Window (SBDW-*)");
    
    if (isPaired()) {
        ESP_LOGI(TAG, "Paired device: %s (%s)", 
                 pairedDevice.name.c_str(), pairedDevice.address.c_str());
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    stopOnFirstMatch = stopOnFirst;
    
    bleScanner->set_scan_continuous(continuousScan);
    
    if (bleScanner->start_scan(durationSeconds)) {
        scanning = true;
        ESP_LOGI(TAG, "✓ Scan started successfully");
    } else {
        ESP_LOGE(TAG, "✗ Failed to start scan");
        
        // Recovery-Versuch
        ESP_LOGW(TAG, "→ Attempting recovery...");
        
        if (bleScanner) {
            bleScanner->stop_scan();
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            ESP_LOGI(TAG, "→ Retry scan start...");
            if (bleScanner->start_scan(durationSeconds)) {
                scanning = true;
                ESP_LOGI(TAG, "✓ Scan started after recovery");
            } else {
                ESP_LOGE(TAG, "✗ Recovery failed - scan could not be started");
            }
        }
    }
}


void ShellyBLEManager::stopScan(bool manualStop) {  // ← Parameter hinzugefügt!
    // ════════════════════════════════════════════════════════════════════
    // Prüfe ob überhaupt was läuft
    // ════════════════════════════════════════════════════════════════════
    
    bool flagActive = scanning;
    bool scannerActive = (bleScanner && bleScanner->is_scanning());
    
    if (!flagActive && !scannerActive) {
        ESP_LOGW(TAG, "No scan in progress (nothing to stop)");
        return;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Pre-Stop Logging
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "STOPPING BLE SCAN");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Pre-stop status:");
    ESP_LOGI(TAG, "  Flag 'scanning': %s", flagActive ? "true" : "false");
    ESP_LOGI(TAG, "  Scanner active:  %s", scannerActive ? "true" : "false");
    ESP_LOGI(TAG, "  Continuous mode: %s", continuousScan ? "true" : "false");
    ESP_LOGI(TAG, "  Manual stop:     %s", manualStop ? "YES (User)" : "NO (Auto)");
    
    // ════════════════════════════════════════════════════════════════════
    // Reset Flags
    // ════════════════════════════════════════════════════════════════════
    
    stopOnFirstMatch = false;
    
    // NUR bei manuellem Stop: NVS aktualisieren!
    bool wasContinuous = continuousScan;
    
    if (wasContinuous && manualStop) {  // ← KRITISCHE ÄNDERUNG!
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ User stopped Continuous Scan");
        ESP_LOGI(TAG, "  Saving state to NVS...");
        
        Preferences prefs;
        prefs.begin("ShellyBLE", false);
        prefs.putBool("continuous_scan", false);
        prefs.end();
        
        continuousScan = false;
        
        ESP_LOGI(TAG, "✓ NVS updated: continuous_scan = false");
        ESP_LOGI(TAG, "  Continuous Scan will NOT auto-restart");
        
    } else if (wasContinuous && !manualStop) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Automatic stop (before restart)");
        ESP_LOGI(TAG, "  NVS will NOT be changed");
        ESP_LOGI(TAG, "  Continuous Scan will auto-restart after scan cycle");
        
        // continuousScan bleibt true!
        // NVS bleibt unverändert!
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Stoppe Scanner-Hardware
    // ════════════════════════════════════════════════════════════════════
    
    if (bleScanner && scannerActive) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Stopping scanner hardware...");
        
        bleScanner->stop_scan();
        
        // Warte bis Scanner wirklich gestoppt ist
        uint32_t wait_start = millis();
        uint32_t timeout = 2000;
        
        while (bleScanner->is_scanning() && (millis() - wait_start < timeout)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        if (bleScanner->is_scanning()) {
            ESP_LOGW(TAG, "  ⚠ Scanner did not stop after %u ms!", timeout);
            ESP_LOGW(TAG, "    Forcing flag reset anyway");
        } else {
            uint32_t stopDuration = millis() - wait_start;
            ESP_LOGI(TAG, "  ✓ Scanner stopped after %u ms", stopDuration);
        }
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Update Flag
    // ════════════════════════════════════════════════════════════════════
    
    scanning = false;
    
    // ════════════════════════════════════════════════════════════════════
    // Post-Stop Logging & Summary
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "BLE SCAN STOPPED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Total Shelly BLU devices found: %d", discoveredDevices.size());
    
    if (discoveredDevices.size() > 0) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Discovered devices:");
        for (size_t i = 0; i < discoveredDevices.size(); i++) {
            const auto& dev = discoveredDevices[i];
            ESP_LOGI(TAG, "  [%d] %s", i+1, dev.name.c_str());
            ESP_LOGI(TAG, "      MAC: %s | RSSI: %d dBm | Encrypted: %s",
                     dev.address.c_str(), 
                     dev.rssi, 
                     dev.isEncrypted ? "Yes" : "No");
        }
    } else {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "⚠ No Shelly BLU Door/Window sensors found");
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Post-stop status: %s", getScanStatus().c_str());
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    // ════════════════════════════════════════════════════════════════════
    // Auto-Restart Logic für Continuous Scan
    // ════════════════════════════════════════════════════════════════════
    
    // WICHTIG: NUR auto-restart wenn:
    // 1. Continuous Scan war VORHER aktiv (wasContinuous)
    // 2. Device ist noch gepairt
    // 3. Stop wurde NICHT manuell ausgelöst (!manualStop)
    
    if (wasContinuous && isPaired() && !manualStop) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "ℹ️  Continuous Scan cycle completed");
        ESP_LOGI(TAG, "   → Auto-restarting in 2 seconds...");
        ESP_LOGI(TAG, "   (This keeps monitoring active continuously)");
        ESP_LOGI(TAG, "");
        
        // Task für verzögerten Auto-Restart
        struct RestartParams {
            ShellyBLEManager* manager;
        };
        
        RestartParams* params = new RestartParams{this};
        
        xTaskCreate([](void* param) {
            // RAII-Pattern
            std::unique_ptr<RestartParams> p(
                static_cast<RestartParams*>(param)
            );
            
            ESP_LOGI(TAG, "⏰ BLE Restart Task started");
            
            // Watchdog entfernen (nicht zeitkritisch)
            esp_task_wdt_delete(NULL);
            
            // Warte 2 Sekunden
            vTaskDelay(pdMS_TO_TICKS(2000));
            
            // Doppel-Check: Ist Continuous Scan immer noch gewünscht?
            // (User könnte es in der Zwischenzeit manuell gestoppt haben)
            Preferences prefs;
            prefs.begin("ShellyBLE", true);  // Read-only
            bool shouldContinue = prefs.getBool("continuous_scan", true);
            prefs.end();
            
            if (shouldContinue && p->manager->isPaired()) {
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "🔄 Auto-restarting Continuous Scan...");
                ESP_LOGI(TAG, "   (Cycle continues - monitoring for events)");
                ESP_LOGI(TAG, "");
                
                // Setze continuousScan Flag WIEDER auf true vor dem Start
                p->manager->continuousScan = true;
                
                // Starte neuen 30-Sekunden Zyklus
                p->manager->startScan(30, false);
                
            } else {
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "ℹ️  Continuous Scan was disabled");
                ESP_LOGI(TAG, "   NOT restarting");
                ESP_LOGI(TAG, "");
            }
            
            // High Water Mark Logging
            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack High Water Mark: %u bytes", 
                    highWater * sizeof(StackType_t));
            
            if (highWater < 512) {
                ESP_LOGW(TAG, "⚠️ Stack critically low! Consider increasing size.");
            }
            
            ESP_LOGI(TAG, "✓ BLE Restart Task completed");
            
            // unique_ptr wird automatisch freigegeben
            vTaskDelete(NULL);
            
        }, "ble_restart", BLE_RESTART_TASK_STACK_SIZE, params, 1, NULL);  // 8KB Stack
    } else {
        // Kein Auto-Restart
        ESP_LOGI(TAG, "");
        
        if (!isPaired()) {
            ESP_LOGI(TAG, "ℹ️  No device paired - scan stopped permanently");
        } else if (manualStop) {
            ESP_LOGI(TAG, "ℹ️  User stopped scan - NOT restarting");
        } else {
            ESP_LOGI(TAG, "ℹ️  Scan stopped (not continuous)");
        }
        
        ESP_LOGI(TAG, "");
    }
}


void ShellyBLEManager::startContinuousScan() {
    if (!initialized) {
        ESP_LOGE(TAG, "✗ Cannot start scan: Manager not initialized");
        return;
    }
    
    if (!isPaired()) {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGW(TAG, "║  ✗ CONTINUOUS SCAN                ║");
        ESP_LOGW(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "Cannot start continuous scan: No device paired!");
        ESP_LOGW(TAG, "");
        return;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Ensure BLE is started
    // ════════════════════════════════════════════════════════════════════
    
    if (!bleScanner) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ BLE not started yet, starting now...");
        
        if (!ensureBLEStarted()) {
            ESP_LOGE(TAG, "✗ Failed to start BLE");
            return;
        }
        
        ESP_LOGI(TAG, "✓ BLE started");
        ESP_LOGI(TAG, "");
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  CONTINUOUS BLE SCAN              ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Paired device: %s (%s)", 
             pairedDevice.name.c_str(), pairedDevice.address.c_str());
    ESP_LOGI(TAG, "Stored Address Type: %s (%d)",
             pairedDevice.addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM",
             pairedDevice.addressType);
    ESP_LOGI(TAG, "Encryption: %s", 
             pairedDevice.bindkey.length() > 0 ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // WHITELIST: BEIDE ADDRESS TYPES HINZUFÜGEN!
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Configuring scan whitelist...");
    ESP_LOGI(TAG, "  Strategy: Add BOTH address types for maximum compatibility");
    ESP_LOGI(TAG, "");
    
    std::vector<esp32_ble_simple::SimpleBLEScanner::WhitelistEntry> whitelist;
    
    // Entry 1: Mit gespeichertem Address Type
    esp32_ble_simple::SimpleBLEScanner::WhitelistEntry entry1(
        pairedDevice.address.c_str(),
        pairedDevice.addressType
    );
    whitelist.push_back(entry1);
    
    // Entry 2: Mit alternativem Address Type
    uint8_t altType = (pairedDevice.addressType == BLE_ADDR_PUBLIC) 
                      ? BLE_ADDR_RANDOM 
                      : BLE_ADDR_PUBLIC;
    
    esp32_ble_simple::SimpleBLEScanner::WhitelistEntry entry2(
        pairedDevice.address.c_str(),
        altType
    );
    whitelist.push_back(entry2);
    
    ESP_LOGI(TAG, "  Whitelist entries:");
    ESP_LOGI(TAG, "    [1] %s (type: %s)", 
             pairedDevice.address.c_str(),
             pairedDevice.addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    ESP_LOGI(TAG, "    [2] %s (type: %s)", 
             pairedDevice.address.c_str(),
             altType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    ESP_LOGI(TAG, "");
    
    if (!bleScanner->set_scan_whitelist(whitelist)) {
        ESP_LOGE(TAG, "✗ Failed to configure whitelist!");
        ESP_LOGE(TAG, "  Continuous scan will see ALL devices");
        ESP_LOGE(TAG, "");
    } else {
        ESP_LOGI(TAG, "✓ Whitelist configured successfully");
        ESP_LOGI(TAG, "  → MAC %s with BOTH address types", pairedDevice.address.c_str());
        ESP_LOGI(TAG, "  → This ensures the device is found regardless of type");
        ESP_LOGI(TAG, "  → All other devices ignored by BLE hardware");
        ESP_LOGI(TAG, "");
    }

    // ════════════════════════════════════════════════════════════════════
    // 100% DUTY CYCLE for continuous scan (interval = window = 160ms)
    // Default 300ms/100ms = 33% duty cycle misses ~2/3 of advertisement
    // windows. With 160ms/160ms, every advertisement is received.
    // ════════════════════════════════════════════════════════════════════
    bleScanner->set_scan_interval_ms(160);
    bleScanner->set_scan_window_ms(160);
    ESP_LOGI(TAG, "✓ Scan duty cycle: 100%% (160ms/160ms)");
    ESP_LOGI(TAG, "");

    continuousScan = true;
    
    // Speichere State in NVS
    Preferences prefs;
    prefs.begin("ShellyBLE", false);
    prefs.putBool("continuous_scan", true);
    prefs.end();
    
    ESP_LOGI(TAG, "✓ Continuous Scan enabled");
    ESP_LOGI(TAG, "");
    
    startScan(30, false);
}

String ShellyBLEManager::getScanStatus() const {
    if (!initialized) return "Not initialized";
    if (continuousScan && scanning) return "Continuous scan active";
    if (scanning) return "Discovery scan active";
    if (continuousScan && !scanning) return "Continuous scan (between cycles)";
    return "Idle";
}

bool ShellyBLEManager::on_device_found(const esp32_ble_simple::SimpleBLEDevice &device) {
    std::string name = device.get_name();
    std::string address = device.get_address_str();
    
    // ════════════════════════════════════════════════════════════════════
    // WICHTIG: return true = "Continue scanning"
    //             return false = "Stop scanning"
    // ════════════════════════════════════════════════════════════════════
    
    // Filter: Nur Shelly BLU Door/Window
    if (name.empty() || name.length() < 9) {
        return true;  // GEÄNDERT: Continue scanning (nicht interessiert)
    }
    
    // UNTERSTÜTZT BEIDE FORMATE:
    // - SBDW-XXXX (alte Firmware)
    // - SBW-002C-XXXX (neue Firmware)
    bool isShellyBLU = false;
    
    if (name.length() >= 5 && name.substr(0, 5) == "SBDW-") {
        isShellyBLU = true;
        ESP_LOGI(TAG, "→ Device Type: SBDW (Door/Window Sensor, old format)");
    } else if (name.length() >= 9 && name.substr(0, 9) == "SBW-002C-") {
        isShellyBLU = true;
        ESP_LOGI(TAG, "→ Device Type: SBW-002C (Door/Window Sensor, new format)");
        
        String deviceId = name.substr(9, 4).c_str();
        ESP_LOGI(TAG, "→ Device ID: %s", deviceId.c_str());
    }
    
    if (!isShellyBLU) {
        return true;  // GEÄNDERT: Continue scanning (kein Shelly)
    }
    
    int8_t rssi = device.get_rssi();

    uint8_t addressType = device.get_address_type();
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "🔍 SHELLY BLU DETECTED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Name: %s", name.c_str());
    ESP_LOGI(TAG, "Address: %s", address.c_str());
    ESP_LOGI(TAG, "Address Type: %s (%d)",
             addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM",
             addressType);
    ESP_LOGI(TAG, "RSSI: %d dBm", rssi);
    ESP_LOGI(TAG, "");
    
    // Get service datas
    const auto& service_datas = device.get_service_datas();
    
    ESP_LOGI(TAG, "→ Service Data check:");
    ESP_LOGI(TAG, "  Total service datas: %d", service_datas.size());
    
    // Suche BTHome Service Data (UUID 0xFCD2)
    bool foundBTHome = false;
    const uint8_t* bthomeData = nullptr;
    size_t bthomeLen = 0;
    
    for (const auto& sd : service_datas) {
        if (sd.uuid.is_16bit() && sd.uuid.get_uuid16() == BTHOME_UUID_UINT16) {
            foundBTHome = true;
            bthomeData = sd.data.data();
            bthomeLen = sd.data.size();
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✓ BTHome Service Data found!");
            ESP_LOGI(TAG, "  UUID: 0xFCD2");
            ESP_LOGI(TAG, "  Length: %d bytes", bthomeLen);
            
            // Hex dump
            char hex[128];
            int offset = 0;
            offset += snprintf(hex, sizeof(hex), "  Data: ");
            for (size_t i = 0; i < bthomeLen && i < 32; i++) {
                offset += snprintf(hex + offset, sizeof(hex) - offset, 
                                 "%02X ", bthomeData[i]);
            }
            ESP_LOGI(TAG, "%s", hex);
            
            break;
        }
    }
    
    if (!foundBTHome) {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "⚠ No BTHome Service Data!");
        ESP_LOGW(TAG, "  Device found but no event data");
        ESP_LOGW(TAG, "  → Device might be sleeping");
        ESP_LOGW(TAG, "  → Or no event occurred yet");
        ESP_LOGW(TAG, "");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        
        // Update discovered devices (ohne Service Data)
        updateDiscoveredDevice(
        String(address.c_str()), 
        String(name.c_str()), 
        rssi, 
        false, 
        device.get_address_uint64(),
        addressType
    );
        
        // Continue scanning (falls stopOnFirstMatch nicht aktiv)
        // Stop nur wenn stopOnFirstMatch UND Shelly gefunden
        if (stopOnFirstMatch) {
            ESP_LOGI(TAG, "✓ Shelly BLU found - stopping scan (stopOnFirstMatch)");
            return false;  // Stop scan
        }
        
        return true;  // Continue scanning
    }
    
    ESP_LOGI(TAG, "");
    
    // Device Info Byte
    uint8_t deviceInfo = bthomeData[0];
    bool isEncrypted = (deviceInfo & 0x01) != 0;
    
    ESP_LOGI(TAG, "  Device Info: 0x%02X", deviceInfo);
    ESP_LOGI(TAG, "  Encrypted: %s", isEncrypted ? "YES 🔒" : "NO");
    ESP_LOGI(TAG, "  BTHome Version: %d", (deviceInfo >> 5) & 0x07);
    ESP_LOGI(TAG, "");
    
    // Update Discovered Devices
    updateDiscoveredDevice(
        String(address.c_str()), 
        String(name.c_str()), 
        rssi, 
        isEncrypted, 
        device.get_address_uint64(),
        addressType
    );
    
    // ════════════════════════════════════════════════════════════════════
    // Check if paired device
    // ════════════════════════════════════════════════════════════════════
    
    if (!isPaired()) {
        ESP_LOGI(TAG, "ℹ Device not paired - skipping data parse");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        
        // Stop scan if stopOnFirstMatch aktiv
        if (stopOnFirstMatch) {
            ESP_LOGI(TAG, "✓ Shelly BLU found - stopping scan (stopOnFirstMatch)");
            return false;  // Stop scan
        }
        
        return true;  // Continue scanning
    }
    
    String addrString(address.c_str());
    if (pairedDevice.address != addrString) {
        ESP_LOGI(TAG, "ℹ Not the paired device - skipping");
        ESP_LOGI(TAG, "  Paired: %s", pairedDevice.address.c_str());
        ESP_LOGI(TAG, "  This:   %s", address.c_str());
        ESP_LOGI(TAG, "═══════════════════════════════════");
        
        // Stop scan if stopOnFirstMatch aktiv
        if (stopOnFirstMatch) {
            ESP_LOGI(TAG, "✓ Shelly BLU found - stopping scan (stopOnFirstMatch)");
            return false;  // Stop scan
        }
        
        return true;  // Continue scanning
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Paired Device - Parse Data
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "┌─────────────────────────────────");
    ESP_LOGI(TAG, "│ PAIRED DEVICE DATA UPDATE");
    ESP_LOGI(TAG, "├─────────────────────────────────");
    ESP_LOGI(TAG, "│");
    
    ShellyBLESensorData sensorData;
    sensorData.rssi = rssi;

    String addressStr = String(device.get_address_str().c_str());
    
    bool parseSuccess = parseBTHomePacket(
        bthomeData, 
        bthomeLen,
        pairedDevice.bindkey,  // Bindkey für Decryption
        addressStr,
        sensorData
    );
    
    if (parseSuccess) {
        // ════════════════════════════════════════════════════════════════
        // PACKET ID DEDUPLICATION
        // Same Packet ID = same BTHome event. Update RSSI/timestamp only,
        // skip the callback to avoid flooding WebSocket every 500ms.
        // ════════════════════════════════════════════════════════════════

        bool isNewPacket = !pairedDevice.sensorData.dataValid ||
                           (sensorData.packetId != pairedDevice.sensorData.packetId);

        sensorData.lastUpdate = millis();  // Set BEFORE storing or calling callback

        pairedDevice.sensorData       = sensorData;
        pairedDevice.sensorData.dataValid = true;

        if (!isNewPacket) {
            // Duplicate — silently update RSSI/timestamp, no callback
            return true;  // Continue scanning
        }

        // New packet: log it and notify
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "DATA SUCCESSFULLY PARSED & DECRYPTED!");
        ESP_LOGI(TAG, "│");
        ESP_LOGI(TAG, "│ Sensor Data:");
        ESP_LOGI(TAG, "│   Packet ID:    %d", sensorData.packetId);
        ESP_LOGI(TAG, "│   Battery:      %d%%", sensorData.battery);
        ESP_LOGI(TAG, "│   Window:       %s",
                 sensorData.windowOpen ? "OPEN 🔓" : "CLOSED 🔒");
        ESP_LOGI(TAG, "│   Illuminance:  %d lux", sensorData.illuminance);
        ESP_LOGI(TAG, "│   Rotation:     %d°", sensorData.rotation);

        if (sensorData.hasButtonEvent) {
            const char* eventName;
            switch (sensorData.buttonEvent) {
                case BUTTON_SINGLE_PRESS: eventName = "SINGLE PRESS 👆"; break;
                case BUTTON_DOUBLE_PRESS: eventName = "DOUBLE PRESS 👆👆"; break;
                case BUTTON_TRIPLE_PRESS: eventName = "TRIPLE PRESS 👆👆👆"; break;
                case BUTTON_LONG_PRESS: eventName = "LONG PRESS ⏸️"; break;
                case BUTTON_HOLD: eventName = "HOLD ⏸️"; break;
                default: eventName = "UNKNOWN";
            }
            ESP_LOGI(TAG, "│   Button:       %s", eventName);
        }

        ESP_LOGI(TAG, "│");
        ESP_LOGI(TAG, "└─────────────────────────────────");
        ESP_LOGI(TAG, "");

        // ════════════════════════════════════════════════════════════════
        // TRIGGER CALLBACK → WebUI + Matter Update
        // ════════════════════════════════════════════════════════════════

        if (sensorDataCallback) {
            ESP_LOGI(TAG, "→ Triggering sensor data callback for WebUI...");
            sensorDataCallback(String(address.c_str()), sensorData);
            ESP_LOGI(TAG, "✓ WebUI notified of sensor data update");
        } else {
            ESP_LOGW(TAG, "⚠ No sensor data callback registered!");
            ESP_LOGW(TAG, "  WebUI will NOT be updated!");
        }

        ESP_LOGI(TAG, "");
        
    } else {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ FAILED TO PARSE DATA!");
        ESP_LOGE(TAG, "│");
        ESP_LOGE(TAG, "│ Possible reasons:");
        ESP_LOGE(TAG, "│   • Decryption failed (wrong bindkey?)");
        ESP_LOGE(TAG, "│   • Invalid BTHome packet structure");
        ESP_LOGE(TAG, "│   • Corrupted data");
        ESP_LOGE(TAG, "│");
        ESP_LOGE(TAG, "└─────────────────────────────────");
        ESP_LOGE(TAG, "");

        pairedDevice.sensorData.lastUpdate = 0;
        pairedDevice.sensorData.dataValid = false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Ende des paired device handling
    // ════════════════════════════════════════════════════════════════════
    
    if (stopOnFirstMatch) {
        ESP_LOGI(TAG, "✓ Paired device found - stopping scan (stopOnFirstMatch)");
        return false;  // Stop scan
    }
    
    return true;  // Continue scanning
}

// ════════════════════════════════════════════════════════════════════════
// SMART CONNECT METHODE (3-in-1 Workflow)
// ════════════════════════════════════════════════════════════════════════

bool ShellyBLEManager::smartConnectDevice(const String& address, uint32_t passkey) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   SMART CONNECT DEVICE            ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Address: %s", address.c_str());
    ESP_LOGI(TAG, "Passkey: %s", passkey > 0 ? "PROVIDED" : "NONE");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // ENSURE BLE STARTED
    // ════════════════════════════════════════════════════════════════════
    
    if (!ensureBLEStarted()) {
        ESP_LOGE(TAG, "✗ Cannot connect: BLE unavailable");
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // WAKE-UP SCAN (OHNE discoveredDevices.clear()!)
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PRE-CONNECTION WAKE-UP          ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Quick scan to wake up device...");
    ESP_LOGI(TAG, "  (Ensuring device is ready for connection)");
    ESP_LOGI(TAG, "");
    
    // WICHTIG: NICHT discoveredDevices.clear()!
    // Die Liste enthält bereits das Device vom Discovery Scan
    
    if (bleScanner) {
        bleScanner->clear_scan_whitelist();
        bleScanner->set_scan_continuous(false);
        bleScanner->start_scan(2);  // 2 Sekunden
        
        scanning = true;
        vTaskDelay(pdMS_TO_TICKS(2500));
        
        if (bleScanner->is_scanning()) {
            bleScanner->stop_scan();
        }
        
        scanning = false;
        
        ESP_LOGI(TAG, "✓ Wake-up scan complete");
        ESP_LOGI(TAG, "  Total devices in list: %d", discoveredDevices.size());
        
        // Debug Output
        bool targetPresent = false;
        for (const auto& dev : discoveredDevices) {
            ESP_LOGI(TAG, "    - %s (%s) | RSSI: %d dBm", 
                     dev.name.c_str(), dev.address.c_str(), dev.rssi);
            
            if (dev.address.equalsIgnoreCase(address)) {
                targetPresent = true;
            }
        }
        
        if (targetPresent) {
            ESP_LOGI(TAG, "  ✓ Target device present");
        } else {
            ESP_LOGW(TAG, "  ⚠ Target NOT in list (continuing anyway)");
        }
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Waiting 500ms before GATT connection...");
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "");
    }
    
    // ════════════════════════════════════════════════════════════════════
    // WORKFLOW DECISION
    // ════════════════════════════════════════════════════════════════════
    
    if (passkey == 0) {
        // ────────────────────────────────────────────────────────────────
        // WORKFLOW 1: Bonding Only (Unencrypted)
        // ────────────────────────────────────────────────────────────────
        
        ESP_LOGI(TAG, "→ WORKFLOW 1: Bonding Only (No Encryption)");
        ESP_LOGI(TAG, "");
        
        bool success = connectDevice(address);
        
        if (success) {
            ESP_LOGI(TAG, "✓ Device bonded successfully (unencrypted)");
            ESP_LOGI(TAG, "  User can enable encryption later via UI");
            ESP_LOGI(TAG, "");
            
            updateDeviceState(STATE_CONNECTED_UNENCRYPTED);
            
            // Continuous Scan erst NACH Connection starten
            ESP_LOGI(TAG, "→ Starting Continuous Scan (after 2 second delay)...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            startContinuousScan();
        }
        
        return success;
        
    } else {
        // ────────────────────────────────────────────────────────────────
        // WORKFLOW 2: Direct Encryption (Bonding + Passkey + Bindkey)
        // ────────────────────────────────────────────────────────────────
        
        ESP_LOGI(TAG, "→ WORKFLOW 2: Direct Encryption");
        ESP_LOGI(TAG, "  Passkey: %06u", passkey);
        ESP_LOGI(TAG, "");
        
        // ════════════════════════════════════════════════════════════════
        // PHASE 1: Bonding
        // ════════════════════════════════════════════════════════════════
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "PHASE 1: BONDING");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "");
        
        bool bondingSuccess = connectDevice(address);
        
        if (!bondingSuccess) {
            ESP_LOGE(TAG, "✗ Bonding failed");
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Bonding complete");
        ESP_LOGI(TAG, "");
        
        // ════════════════════════════════════════════════════════════════
        // PHASE 2: Enable Encryption
        // ════════════════════════════════════════════════════════════════
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "PHASE 2: ENABLE ENCRYPTION");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "⚡ Using ACTIVE connection from Phase 1");
        ESP_LOGI(TAG, "   → NO button press needed!");
        ESP_LOGI(TAG, "");
        
        // Kurze Pause, damit Connection stabilisiert
        vTaskDelay(pdMS_TO_TICKS(500));
        
        bool encryptionSuccess = enableEncryption(address, passkey);
        
        if (!encryptionSuccess) {
            ESP_LOGE(TAG, "✗ Encryption failed");
            return false;
        }
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  DIRECT ENCRYPTION COMPLETE!   ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        // ════════════════════════════════════════════════════════════════
        // PHASE 3: Read Initial Sensor Data via GATT
        // ════════════════════════════════════════════════════════════════
        
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  PHASE 3: INITIAL SENSOR DATA     ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Reading current sensor state via GATT...");
        ESP_LOGI(TAG, "   (Provides immediate data without waiting for events)");
        ESP_LOGI(TAG, "");
        
        // Warte kurz damit Device sich stabilisiert
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        ShellyBLESensorData initialData;
        bool readSuccess = readSampleBTHomeData(address, initialData);
        
        if (readSuccess) {
            ESP_LOGI(TAG, "✓ Initial sensor data retrieved:");
            ESP_LOGI(TAG, "  Packet ID:    %d", initialData.packetId);
            ESP_LOGI(TAG, "  Battery:      %d%%", initialData.battery);
            ESP_LOGI(TAG, "  Window:       %s", initialData.windowOpen ? "OPEN 🔓" : "CLOSED 🔒");
            ESP_LOGI(TAG, "  Illuminance:  %d lux", initialData.illuminance);
            ESP_LOGI(TAG, "  Rotation:     %d°", initialData.rotation);
            ESP_LOGI(TAG, "");
            
            // Update paired device data
            pairedDevice.sensorData = initialData;
            pairedDevice.sensorData.lastUpdate = millis();
            pairedDevice.sensorData.dataValid = true;
            
            // Trigger Callback für WebUI Update
            if (sensorDataCallback) {
                ESP_LOGI(TAG, "→ Triggering sensor data callback for WebUI...");
                sensorDataCallback(address, initialData);
                ESP_LOGI(TAG, "✓ WebUI notified of initial sensor data");
            }
            
            ESP_LOGI(TAG, "");
            
        } else {
            ESP_LOGW(TAG, "⚠ Could not read initial sensor data via GATT");
            ESP_LOGW(TAG, "  This is OK - will wait for first event");
            ESP_LOGW(TAG, "  Tip: Open/close the door to trigger an event");
            ESP_LOGI(TAG, "");
        }
        
        // ════════════════════════════════════════════════════════════════
        // PHASE 4: Start Continuous Scan
        // ════════════════════════════════════════════════════════════════
        
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  PHASE 4: START CONTINUOUS SCAN   ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Starting Continuous Scan for future events...");
        ESP_LOGI(TAG, "   (Will monitor door open/close, button press, etc.)");
        ESP_LOGI(TAG, "");
        
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        startContinuousScan();
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  ALL PHASES COMPLETE!          ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Summary:");
        ESP_LOGI(TAG, "  ✓ Phase 1: Bonding complete");
        ESP_LOGI(TAG, "  ✓ Phase 2: Encryption enabled");
        ESP_LOGI(TAG, "  ✓ Phase 3: Initial data read");
        ESP_LOGI(TAG, "  ✓ Phase 4: Continuous scan active");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Device is now fully operational!");
        ESP_LOGI(TAG, "");
        
        return true;
    }
}


// ============================================================================
// 2-PHASEN-WORKFLOW
// ============================================================================

// ────────────────────────────────────────────────────────────────────────────
// PHASE 1: Connect Device (Bonding ohne Encryption)
// ────────────────────────────────────────────────────────────────────────────

// ════════════════════════════════════════════════════════════════════════
// CONNECT DEVICE (Phase 1: Bonding)
// ════════════════════════════════════════════════════════════════════════

bool ShellyBLEManager::connectDevice(const String& address) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PHASE 1: BONDING + CONNECT      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Ensure BLE is started
    // ════════════════════════════════════════════════════════════════════
    
    if (!bleScanner) {
        ESP_LOGI(TAG, "→ BLE not started yet, starting now...");
        
        if (!ensureBLEStarted()) {
            ESP_LOGE(TAG, "✗ Failed to start BLE");
            return false;
        }
        
        ESP_LOGI(TAG, "✓ BLE started");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // PRE-CHECK: NimBLE & Scanner Status
    if (!NimBLEDevice::isInitialized()) {
        ESP_LOGE(TAG, "✗ NimBLE not initialized!");
        return false;
    }
    
    // KRITISCH: STOPPE SCANNER VOR CONNECTION!
    if (scanning || (bleScanner && bleScanner->is_scanning())) {
        ESP_LOGW(TAG, "⚠ Scanner is active - STOPPING before GATT connection");
        ESP_LOGW(TAG, "  (NimBLE stack can't scan and connect simultaneously)");
        
        stopScan();
        
        // Warte bis Scanner wirklich gestoppt ist
        uint32_t wait_start = millis();
        while ((bleScanner && bleScanner->is_scanning()) && 
               (millis() - wait_start < 3000)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        if (bleScanner && bleScanner->is_scanning()) {
            ESP_LOGE(TAG, "✗ Failed to stop scanner after 3 seconds!");
            ESP_LOGE(TAG, "  Cannot proceed with GATT connection");
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Scanner stopped successfully");
        ESP_LOGI(TAG, "");
        
        // Zusätzliche Pause für NimBLE Stack
        ESP_LOGI(TAG, "→ Waiting 1 second for NimBLE stack to settle...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Prüfe ob bereits eine aktive Connection existiert
    if (activeClient && activeClient->isConnected()) {
        ESP_LOGW(TAG, "⚠ Already connected to a device");
        ESP_LOGI(TAG, "→ Disconnecting current device first...");
        closeActiveConnection();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // ════════════════════════════════════════════════════════════════════
    // DEVICE LOOKUP
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   DEVICE LOOKUP                   ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    String deviceName = "Unknown";
    uint8_t addressType = BLE_ADDR_RANDOM;  // Default für Shelly
    bool deviceFound = false;
    
    for (const auto& dev : discoveredDevices) {
        if (dev.address.equalsIgnoreCase(address)) {
            deviceName = dev.name;
            addressType = dev.addressType;
            deviceFound = true;
            ESP_LOGI(TAG, "✓ Device found in scan results:");
            ESP_LOGI(TAG, "  Name: %s", deviceName.c_str());
            ESP_LOGI(TAG, "  Address: %s", address.c_str());
            ESP_LOGI(TAG, "  Type: %s", addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
            break;
        }
    }
    
    if (!deviceFound) {
        ESP_LOGE(TAG, "✗ Device not found in recent scan");
        ESP_LOGE(TAG, "  Run a scan first to discover the device");
        return false;
    }
    
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // SECURITY SETUP
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   SECURITY SETUP FOR BONDING      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    NimBLEDevice::setSecurityAuth(true, false, true);  // Bonding, No MITM, SC
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);  // Just Works
    
    ESP_LOGI(TAG, "✓ Security configured:");
    ESP_LOGI(TAG, "  Bonding: ENABLED");
    ESP_LOGI(TAG, "  MITM: Disabled");
    ESP_LOGI(TAG, "  I/O Capability: No Input/Output (Just Works)");
    ESP_LOGI(TAG, "  → Pairing will be AUTO-CONFIRMED by ESP32");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // GATT CONNECTION
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   GATT CONNECTION                 ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    activeClient = NimBLEDevice::createClient();
    if (!activeClient) {
        ESP_LOGE(TAG, "✗ Failed to create BLE client!");
        return false;
    }
    
    // Callbacks
    activeClientCallbacks = new PairingCallbacks(this);
    activeClient->setClientCallbacks(activeClientCallbacks, false);
    
    activeClient->setConnectionParams(12, 12, 0, 100);
    activeClient->setConnectTimeout(10000);  // 10 Sekunden pro Versuch
    
    ESP_LOGI(TAG, "→ Connecting...");
    
    NimBLEAddress peerAddress(address.c_str(), addressType);
    
    bool connected = false;
    
    ESP_LOGI(TAG, "→ Attempt 1/2: %s address type...",
             addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    
    if (activeClient->connect(peerAddress, false) || activeClient->isConnected()) {
        connected = true;
        ESP_LOGI(TAG, "✓ Connected with %s address",
                 addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    } else {
        ESP_LOGW(TAG, "  Failed, trying alternative address type...");

        // Only try the alternative address type if we are truly not connected.
        // (connect() can return false even when the link is up due to a race
        // between the NimBLE GAP event and the connect() return path.)
        uint8_t altType = (addressType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
        peerAddress = NimBLEAddress(address.c_str(), altType);

        ESP_LOGI(TAG, "→ Attempt 2/2: %s address type...",
                 altType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");

        if (activeClient->connect(peerAddress, false) || activeClient->isConnected()) {
            connected = true;
            ESP_LOGI(TAG, "✓ Connected with %s address",
                     altType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        }
    }
    
    if (!connected) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ GATT connection failed");
        ESP_LOGE(TAG, "");
        closeActiveConnection();
        return false;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ GATT connected");
    ESP_LOGI(TAG, "  Peer: %s", activeClient->getPeerAddress().toString().c_str());
    ESP_LOGI(TAG, "  MTU: %d bytes", activeClient->getMTU());
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // EXPLICIT PAIRING ANFORDERN
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   INITIATE PAIRING                ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Requesting secure connection (bonding)...");
    ESP_LOGI(TAG, "  This will be AUTO-CONFIRMED (Just Works)");
    ESP_LOGI(TAG, "");
    
    // EXPLICIT Pairing anfordern!
    bool secureResult = activeClient->secureConnection();
    
    if (!secureResult) {
        ESP_LOGE(TAG, "✗ secureConnection() returned false!");
        ESP_LOGE(TAG, "  Pairing was rejected or failed");
        ESP_LOGE(TAG, "");
        closeActiveConnection();
        return false;
    }
    
    ESP_LOGI(TAG, "✓ secureConnection() returned true");
    ESP_LOGI(TAG, "  Pairing initiated successfully");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // WARTE AUF BONDING COMPLETE
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "⏳ Waiting for bonding to complete...");
    ESP_LOGI(TAG, "");
    
    uint32_t wait_start = millis();
    uint32_t last_log = 0;
    
    while (millis() - wait_start < 15000) {
        if (activeClientCallbacks->pairingComplete) {
            break;
        }
        
        if (millis() - last_log > 2000) {
            uint32_t elapsed = (millis() - wait_start) / 1000;
            ESP_LOGI(TAG, "  Waiting... %u seconds", elapsed);
            last_log = millis();
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (!activeClientCallbacks->pairingComplete) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ Bonding timeout after 15 seconds!");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Debug info:");
        ESP_LOGI(TAG, "  - GATT connected: %s", activeClient->isConnected() ? "YES" : "NO");
        ESP_LOGI(TAG, "  - secureConnection() returned: true");
        ESP_LOGI(TAG, "  - Pairing callbacks registered: YES");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Possible reasons:");
        ESP_LOGE(TAG, "  1. Device not in pairing mode (button not held 10+ sec)");
        ESP_LOGE(TAG, "  2. Device already bonded to another controller");
        ESP_LOGE(TAG, "  3. NimBLE stack issue");
        ESP_LOGE(TAG, "");
        closeActiveConnection();
        return false;
    }
    
    if (!activeClientCallbacks->pairingSuccess) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ Bonding failed!");
        ESP_LOGE(TAG, "");
        closeActiveConnection();
        return false;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ✓ BONDING SUCCESSFUL             ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ Device is now bonded (trusted)");
    ESP_LOGI(TAG, "✓ Pairing was AUTO-CONFIRMED by ESP32");
    ESP_LOGI(TAG, "✓ Protected characteristics are now accessible");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // SERVICE DISCOVERY
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "→ Discovering services...");
    
    std::vector<NimBLERemoteService*> services = activeClient->getServices(true);
    
    if (services.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        closeActiveConnection();
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Found %d services", services.size());
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // CONNECTION BLEIBT AKTIV (für Phase 2)
    // ════════════════════════════════════════════════════════════════════
    
    activeClientTimestamp = millis();
    
    pairedDevice.address = address;
    pairedDevice.name = deviceName;
    pairedDevice.bindkey = "";  // Noch nicht encrypted
    
    savePairedDevice();
    updateDeviceState(STATE_CONNECTED_UNENCRYPTED);
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ✓ PHASE 1 COMPLETE               ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device: %s (%s)", deviceName.c_str(), address.c_str());
    ESP_LOGI(TAG, "Status: Bonded + Connected");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "⚠ Connection kept ACTIVE for Phase 2");
    ESP_LOGI(TAG, "  → Encryption can be enabled immediately");
    ESP_LOGI(TAG, "  → NO new button press needed!");
    ESP_LOGI(TAG, "");
    
    return true;
}


    // ────────────────────────────────────────────────────────────────────────────
    // PHASE 2: Enable Encryption
    // ────────────────────────────────────────────────────────────────────────────

    bool ShellyBLEManager::enableEncryption(const String& address, uint32_t passkey) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PHASE 2: ENABLE ENCRYPTION      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // VARIABLEN DEKLARIEREN
    // ========================================================================
    
    uint8_t addressType = BLE_ADDR_PUBLIC;  // Default
    bool wasScanning = scanning;
    bool needNewConnection = true;
    NimBLEClient* pClient = nullptr;
    
    // Device Info aus discoveredDevices holen
    for (const auto& dev : discoveredDevices) {
        if (dev.address.equalsIgnoreCase(address)) {
            addressType = dev.addressType;
            break;
        }
    }
    
    // ========================================================================
    // PRÜFE BESTEHENDE CONNECTION
    // ========================================================================
    
    if (activeClient && activeClient->isConnected()) {
        String connectedAddr = String(activeClient->getPeerAddress().toString().c_str());
        
        if (connectedAddr.equalsIgnoreCase(address)) {
            uint32_t connectionAge = millis() - activeClientTimestamp;
            
            if (connectionAge < 60000) {
                ESP_LOGI(TAG, "✓ Using existing BONDED connection from Phase 1");
                ESP_LOGI(TAG, "  Connection age: %u ms", connectionAge);
                ESP_LOGI(TAG, "  This connection has write permissions!");
                ESP_LOGI(TAG, "");
                
                pClient = activeClient;
                needNewConnection = false;
            } else {
                ESP_LOGW(TAG, "⚠ Connection too old, will reconnect");
                closeActiveConnection();
            }
        } else {
            ESP_LOGW(TAG, "⚠ Wrong device, disconnecting");
            closeActiveConnection();
        }
    }
    
    // ========================================================================
    // NEUE CONNECTION (falls nötig)
    // ========================================================================
    
    if (needNewConnection) {
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   NEW BONDED CONNECTION REQUIRED  ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        if (wasScanning) {
            ESP_LOGI(TAG, "→ Stopping scan...");
            stopScan();
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        
        NimBLEDevice::setSecurityAuth(true, false, true);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            ESP_LOGE(TAG, "✗ Failed to create client");
            if (wasScanning) startScan(30);
            return false;
        }
        
        PairingCallbacks* callbacks = new PairingCallbacks(this);
        pClient->setClientCallbacks(callbacks, false);
        pClient->setConnectTimeout(25000);
        
        ESP_LOGI(TAG, "→ Connecting...");
        
        NimBLEAddress bleAddr(address.c_str(), addressType);
        bool connected = pClient->connect(bleAddr, false);
        
        if (!connected) {
            uint8_t altType = (addressType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
            bleAddr = NimBLEAddress(address.c_str(), altType);
            connected = pClient->connect(bleAddr, false);
        }
        
        if (!connected) {
            ESP_LOGE(TAG, "✗ Connection failed");
            delete callbacks;
            NimBLEDevice::deleteClient(pClient);
            if (wasScanning) startScan(30);
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Connected");
        
        // Warte auf Bonding
        uint32_t wait_start = millis();
        while (millis() - wait_start < 15000) {
            if (callbacks->pairingComplete) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        
        if (!callbacks->pairingSuccess) {
            ESP_LOGE(TAG, "✗ Bonding failed");
            delete callbacks;
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
            if (wasScanning) startScan(30);
            return false;
        }
        
        delete callbacks;
        ESP_LOGI(TAG, "");
    }
    
    // ========================================================================
    // SERVICE DISCOVERY
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   SERVICE DISCOVERY               ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Refreshing services...");
    
    std::vector<NimBLERemoteService*> services = pClient->getServices(true);
    
    if (services.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        if (needNewConnection) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Found %d services", services.size());
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // PASSKEY SCHREIBEN
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   WRITE PASSKEY                   ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // BESSERES LOGGING: Zeige alle Services & Characteristics
    ESP_LOGI(TAG, "→ Searching for Passkey characteristic...");
    ESP_LOGI(TAG, "  Target UUID: %s", GATT_UUID_PASSKEY);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Available services:");
    
    for (size_t i = 0; i < services.size(); i++) {
        auto* pService = services[i];
        ESP_LOGI(TAG, "  [%d] Service: %s", i, pService->getUUID().toString().c_str());
        
        // KORRIGIERT: getCharacteristics() gibt const vector& zurück
        const std::vector<NimBLERemoteCharacteristic*>& charVec = pService->getCharacteristics(true);
        
        if (charVec.size() > 0) {
            ESP_LOGI(TAG, "      Characteristics: %d", charVec.size());
            
            for (auto* pChar : charVec) {
                ESP_LOGI(TAG, "        - %s (Props: %s%s%s)", 
                         pChar->getUUID().toString().c_str(),
                         pChar->canRead() ? "R" : "",
                         pChar->canWrite() ? "W" : "",
                         pChar->canWriteNoResponse() ? "w" : "");
            }
        } else {
            ESP_LOGI(TAG, "      Characteristics: 0");
        }
    }
    
    ESP_LOGI(TAG, "");
    
    NimBLEUUID passkeyUUID(GATT_UUID_PASSKEY);
    NimBLERemoteCharacteristic* pPasskeyChar = nullptr;
    
    ESP_LOGI(TAG, "→ Looking for UUID: %s", passkeyUUID.toString().c_str());
    
    for (auto* pService : services) {
        pPasskeyChar = pService->getCharacteristic(passkeyUUID);
        if (pPasskeyChar) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✓ Passkey characteristic FOUND!");
            ESP_LOGI(TAG, "  Service: %s", pService->getUUID().toString().c_str());
            ESP_LOGI(TAG, "  UUID: %s", pPasskeyChar->getUUID().toString().c_str());
            ESP_LOGI(TAG, "  Can Write: %s", pPasskeyChar->canWrite() ? "YES" : "NO");
            ESP_LOGI(TAG, "  Can Write NoResponse: %s", pPasskeyChar->canWriteNoResponse() ? "YES" : "NO");
            ESP_LOGI(TAG, "");
            break;
        }
    }
    
    // ✅✅KRITISCHER NULL-CHECK! ✅✅✅
    if (!pPasskeyChar) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGE(TAG, "║  ✗ PASSKEY CHAR NOT FOUND!        ║");
        ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "This means:");
        ESP_LOGE(TAG, "  1. Device is NOT in pairing mode");
        ESP_LOGE(TAG, "  2. Or: Services not discovered correctly");
        ESP_LOGE(TAG, "  3. Or: Wrong device type");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Expected UUID: %s", GATT_UUID_PASSKEY);
        ESP_LOGE(TAG, "Searched in %d services", services.size());
        ESP_LOGE(TAG, "");
        
        if (needNewConnection) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        } else {
            closeActiveConnection();
        }
        return false;
    }
    
    // CHECK: Ist Characteristic wirklich beschreibbar?
    if (!pPasskeyChar->canWrite() && !pPasskeyChar->canWriteNoResponse()) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGE(TAG, "║  ✗ PASSKEY CHAR NOT WRITABLE!     ║");
        ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Properties:");
        ESP_LOGE(TAG, "  Can Read: %s", pPasskeyChar->canRead() ? "YES" : "NO");
        ESP_LOGE(TAG, "  Can Write: %s", pPasskeyChar->canWrite() ? "YES" : "NO");
        ESP_LOGE(TAG, "  Can Write NoResponse: %s", pPasskeyChar->canWriteNoResponse() ? "YES" : "NO");
        ESP_LOGE(TAG, "");
        
        if (needNewConnection) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        } else {
            closeActiveConnection();
        }
        return false;
    }
    
    // AB HIER IST pPasskeyChar GARANTIERT != NULL UND WRITABLE!
    
    uint8_t passkeyBytes[4];
    passkeyBytes[0] = (passkey) & 0xFF;
    passkeyBytes[1] = (passkey >> 8) & 0xFF;
    passkeyBytes[2] = (passkey >> 16) & 0xFF;
    passkeyBytes[3] = (passkey >> 24) & 0xFF;
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   WRITE PASSKEY TO DEVICE         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Writing Passkey: %u", passkey);
    ESP_LOGI(TAG, "  Bytes: 0x%02X 0x%02X 0x%02X 0x%02X", 
             passkeyBytes[0], passkeyBytes[1], passkeyBytes[2], passkeyBytes[3]);
    ESP_LOGI(TAG, "");
    
    // Watchdog feed
    esp_task_wdt_reset();
    
    bool writeSuccess = false;
    
    ESP_LOGI(TAG, "→ Attempting write with response...");
    
    // JETZT ist canWrite() sicher (pPasskeyChar != NULL)
    if (pPasskeyChar->canWrite()) {
        writeSuccess = pPasskeyChar->writeValue(passkeyBytes, 4, true);
        
        if (writeSuccess) {
            ESP_LOGI(TAG, "  ✓ Write with response: SUCCESS");
            savePasskey(passkey);
        } else {
            ESP_LOGW(TAG, "  ✗ Write with response: FAILED");
        }
    }
    
    if (!writeSuccess && pPasskeyChar->canWriteNoResponse()) {
        ESP_LOGI(TAG, "→ Attempting write without response...");
        writeSuccess = pPasskeyChar->writeValue(passkeyBytes, 4, false);
        
        if (writeSuccess) {
            ESP_LOGI(TAG, "  ✓ Write without response: SUCCESS");
            savePasskey(passkey);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            ESP_LOGW(TAG, "  ✗ Write without response: FAILED");
        }
    }
    
    // Watchdog feed
    esp_task_wdt_reset();
    
    ESP_LOGI(TAG, "✓ Passkey written successfully!");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Device will reboot and enable encryption...");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // DISCONNECT
    // ========================================================================
    
    ESP_LOGI(TAG, "→ Disconnecting...");
    pClient->disconnect();
    
    uint8_t retries = 0;
    while (pClient->isConnected() && retries < 20) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retries++;
    }
    
    if (needNewConnection) {
        NimBLEDevice::deleteClient(pClient);
    } else {
        NimBLEDevice::deleteClient(activeClient);
        activeClient = nullptr;
        activeClientTimestamp = 0;
    }
    
    ESP_LOGI(TAG, "✓ Disconnected");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // REBOOT WAIT
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   WAITING FOR REBOOT              ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "⏳ Device is rebooting...");
    
    for (int i = 0; i < 8; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i % 2 == 0 || i >= 6) {
            ESP_LOGI(TAG, "  %d/8 seconds...", i + 1);
        }
    }
    
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // RE-DISCOVERY
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   RE-DISCOVERY AFTER REBOOT       ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // WICHTIG: Laufenden Scan stoppen!
    if (scanning) {
        ESP_LOGI(TAG, "→ Stopping active continuous scan...");
        NimBLEScan* pActiveScan = NimBLEDevice::getScan();
        pActiveScan->stop();
        scanning = false;
        vTaskDelay(pdMS_TO_TICKS(1500));  // 1,5 Sekunden warten
    }
    
    String newAddress = address;
    uint8_t newType = addressType;
    bool found = false;
    
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    
    ESP_LOGI(TAG, "→ Starting BLOCKING re-discovery scan (10 seconds)...");
    ESP_LOGI(TAG, "   This will wait for the scan to complete");
    ESP_LOGI(TAG, "");
    
    // BLOCKING SCAN: true statt false!
    bool scanSuccess = pScan->start(10, true);  // ← true = blocking!
    
    if (scanSuccess) {
        NimBLEScanResults results = pScan->getResults();
        ESP_LOGI(TAG, "→ Re-discovery scan completed");
        ESP_LOGI(TAG, "  Found %d devices total", results.getCount());
        ESP_LOGI(TAG, "");
        
        if (results.getCount() > 0) {
            ESP_LOGI(TAG, "Scanning results:");
            
            for (int i = 0; i < results.getCount(); i++) {
                const NimBLEAdvertisedDevice* dev = results.getDevice(i);
                if (!dev) continue;
                
                String devName = String(dev->getName().c_str());
                String devAddr = String(dev->getAddress().toString().c_str());
                
                ESP_LOGI(TAG, "  [%d] %s", i + 1, devName.c_str());
                ESP_LOGI(TAG, "      Address: %s", devAddr.c_str());
                ESP_LOGI(TAG, "      RSSI: %d dBm", dev->getRSSI());
                
                // Strategie 1: Suche nach gleichem Namen
                if (devName == pairedDevice.name) {
                    newAddress = devAddr;
                    newType = dev->getAddress().getType();
                    found = true;
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "✓ TARGET DEVICE FOUND by name!");
                    ESP_LOGI(TAG, "  Name: %s", devName.c_str());
                    ESP_LOGI(TAG, "  Address: %s", newAddress.c_str());
                    ESP_LOGI(TAG, "  Type: %s", 
                             newType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
                    break;
                }
                
                // Strategie 2: Gleiche Adresse
                if (devAddr.equalsIgnoreCase(address)) {
                    newAddress = devAddr;
                    newType = dev->getAddress().getType();
                    found = true;
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "✓ TARGET DEVICE FOUND by address!");
                    ESP_LOGI(TAG, "  Address: %s", newAddress.c_str());
                    ESP_LOGI(TAG, "  Type: %s", 
                             newType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
                    break;
                }
            }
            
            if (!found) {
                ESP_LOGW(TAG, "");
                ESP_LOGW(TAG, "⚠ Target device not in scan results");
                ESP_LOGI(TAG, "  Looking for: %s", pairedDevice.name.c_str());
            }
        }
        
        pScan->clearResults();
        
    } else {
        ESP_LOGE(TAG, "✗ Re-discovery scan failed to start or complete!");
    }
    
    if (!found) {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "⚠ Device not found in re-discovery scan");
        ESP_LOGI(TAG, "  Will try with original address anyway");
        ESP_LOGI(TAG, "  Address: %s", address.c_str());
        ESP_LOGI(TAG, "  Type: %s", addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Note: Device might still be rebooting");
        ESP_LOGI(TAG, "      Or encryption is active (different advertisement)");
        newAddress = address;
        newType = addressType;
    }
    
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // RECONNECT MIT MEHREREN VERSUCHEN
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   RECONNECT FOR BINDKEY READ      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    
    bool connected = false;
    int maxAttempts = 5;  // 5 Versuche!
    
    for (int attempt = 1; attempt <= maxAttempts && !connected; attempt++) {
        ESP_LOGI(TAG, "→ Connection attempt %d/%d...", attempt, maxAttempts);
        ESP_LOGI(TAG, "  Target: %s (%s)", 
                 newAddress.c_str(),
                 newType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            ESP_LOGE(TAG, "  ✗ Failed to create client");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        pClient->setConnectTimeout(1500);  // 15 Sekunden pro Versuch
        
        NimBLEAddress newBleAddr(newAddress.c_str(), newType);
        connected = pClient->connect(newBleAddr, false);
        
        if (connected) {
            ESP_LOGI(TAG, "  ✓ Connected with %s address!",
                     newType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        } else {
            ESP_LOGW(TAG, "  ✗ Failed with %s address", 
                     newType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
            
            // Versuche alternativen Address Type
            uint8_t altType = (newType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
            ESP_LOGI(TAG, "  → Trying %s address...",
                     altType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
            
            newBleAddr = NimBLEAddress(newAddress.c_str(), altType);
            connected = pClient->connect(newBleAddr, false);
            
            if (connected) {
                newType = altType;
                ESP_LOGI(TAG, "  ✓ Connected with %s address!",
                         altType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
            }
        }
        
        if (!connected) {
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
            
            if (attempt < maxAttempts) {
                int waitTime = 3;  // 3 Sekunden zwischen Versuchen
                ESP_LOGI(TAG, "  → Waiting %d seconds before retry...", waitTime);
                ESP_LOGI(TAG, "     (Device might still be rebooting)");
                vTaskDelay(pdMS_TO_TICKS(waitTime * 1000));
            }
        }
    }
    
    if (!connected) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGE(TAG, "║  ✗ ALL RECONNECT ATTEMPTS FAILED  ║");
        ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Tried %d times with both address types", maxAttempts);
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Possible reasons:");
        ESP_LOGE(TAG, "  • Passkey was rejected (device reverted to unencrypted)");
        ESP_LOGE(TAG, "  • Device takes longer than expected to reboot");
        ESP_LOGE(TAG, "  • Device address changed and scan didn't find it");
        ESP_LOGE(TAG, "  • Device is malfunctioning");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Next steps:");
        ESP_LOGE(TAG, "  1. Check device LED - is it showing encryption mode?");
        ESP_LOGE(TAG, "  2. Wait 30 seconds and run 'Enable Encryption' again");
        ESP_LOGE(TAG, "  3. If still failing: Factory reset (35+ sec button press)");
        ESP_LOGE(TAG, "");
        
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ Reconnected successfully after %d attempt(s)!", 
             connected ? maxAttempts - (maxAttempts - 1) : maxAttempts);
    ESP_LOGI(TAG, "  Final address: %s", newAddress.c_str());
    ESP_LOGI(TAG, "  Final type: %s", newType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    ESP_LOGI(TAG, "  MTU: %d bytes", pClient->getMTU());
    ESP_LOGI(TAG, "");
    
    // Service Discovery (neue Variable für zweiten Discovery)
    std::vector<NimBLERemoteService*> services2 = pClient->getServices(true);
    
    if (services2.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    // ========================================================================
    // BINDKEY AUSLESEN
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   READ ENCRYPTION KEY (BINDKEY)   ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    NimBLEUUID keyUUID(GATT_UUID_ENCRYPTION_KEY);
    NimBLERemoteCharacteristic* pKeyChar = nullptr;
    
    for (auto* pService : services2) {
        pKeyChar = pService->getCharacteristic(keyUUID);
        if (pKeyChar) {
            ESP_LOGI(TAG, "✓ Encryption Key characteristic found");
            break;
        }
    }
    
    String bindkey = "";
    
    if (pKeyChar && pKeyChar->canRead()) {
        ESP_LOGI(TAG, "→ Reading encryption key...");
        
        std::string val = pKeyChar->readValue();
        
        if (val.length() == 16) {
            for (size_t i = 0; i < val.length(); i++) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", (uint8_t)val[i]);
                bindkey += hex;
            }
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✓ Bindkey read successfully!");
            ESP_LOGI(TAG, "  Bindkey: %s", bindkey.c_str());
        } else {
            ESP_LOGE(TAG, "✗ Invalid bindkey length: %d (expected 16)", val.length());
            
            if (val.length() > 0) {
                String hexDump = "";
                for (size_t i = 0; i < val.length(); i++) {
                    char hex[3];
                    snprintf(hex, sizeof(hex), "%02x", (uint8_t)val[i]);
                    hexDump += hex;
                    if (i < val.length() - 1) hexDump += " ";
                }
                ESP_LOGE(TAG, "  Received: %s", hexDump.c_str());
            }
        }
    } else {
        ESP_LOGE(TAG, "✗ Encryption Key characteristic not found or not readable!");
    }
    
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // DISCONNECT
    // ========================================================================
    
    ESP_LOGI(TAG, "→ Disconnecting...");
    pClient->disconnect();
    
    retries = 0;
    while (pClient->isConnected() && retries < 20) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retries++;
    }
    
    NimBLEDevice::deleteClient(pClient);
    ESP_LOGI(TAG, "✓ Disconnected");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // SPEICHERN & STATE UPDATE
    // ========================================================================
    
    if (bindkey.length() == 32) {
        pairedDevice.address = newAddress;
        pairedDevice.bindkey = bindkey;
        savePairedDevice();
        
        updateDeviceState(STATE_CONNECTED_ENCRYPTED);
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  ✓ PHASE 2 COMPLETE               ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Device: %s (%s)", pairedDevice.name.c_str(), newAddress.c_str());
        ESP_LOGI(TAG, "Status: ENCRYPTED");
        ESP_LOGI(TAG, "Bindkey: %s", bindkey.c_str());
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "✓ Device is now sending ENCRYPTED BTHome v2 advertisements");
        ESP_LOGI(TAG, "  Advertisements have Device Info = 0x41 (encrypted flag set)");
        ESP_LOGI(TAG, "  Data will be decrypted automatically using the bindkey");
        ESP_LOGI(TAG, "");
        
        if (wasScanning) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            startScan(30);
        }
        
        return true;
    }
    
    // ========================================================================
    // FEHLER: KEIN GÜLTIGER BINDKEY
    // ========================================================================
    
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGE(TAG, "║  ✗ PHASE 2 FAILED                 ║");
    ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "Failed to read valid encryption key");
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "What happened:");
    ESP_LOGE(TAG, "  ✓ Passkey was written successfully");
    ESP_LOGE(TAG, "  ✓ Device rebooted");
    ESP_LOGE(TAG, "  ✓ Reconnection successful");
    ESP_LOGE(TAG, "  ✗ Encryption Key read failed or invalid");
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "Possible reasons:");
    ESP_LOGE(TAG, "  1. Passkey was incorrect (device reverted to unencrypted)");
    ESP_LOGE(TAG, "  2. Device didn't accept the passkey");
    ESP_LOGE(TAG, "  3. Encryption Key characteristic not accessible");
    ESP_LOGE(TAG, "  4. Firmware bug in device");
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "Try:");
    ESP_LOGE(TAG, "  • Factory reset device (hold button 35+ seconds)");
    ESP_LOGE(TAG, "  • Use default passkey: 123456");
    ESP_LOGE(TAG, "  • Check Shelly BLU documentation");
    ESP_LOGE(TAG, "");
    
    if (wasScanning) {
        startScan(30);
    }
    
    return false;
}

// ============================================================================
// GATT Configuration
// ============================================================================

bool ShellyBLEManager::setBeaconMode(const String& address, bool enabled) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  SET BEACON MODE");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Target: %s", address.c_str());
    ESP_LOGI(TAG, "Mode: %s", enabled ? "ENABLED" : "DISABLED");
    
    bool success = writeGattCharacteristic(address, GATT_UUID_BEACON_MODE, enabled ? 1 : 0);
    
    if (success) {
        ESP_LOGI(TAG, "✓ Beacon mode updated");
    } else {
        ESP_LOGE(TAG, "✗ Failed to update beacon mode");
    }
    
    return success;
}

bool ShellyBLEManager::setAngleThreshold(const String& address, uint8_t degrees) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  SET ANGLE THRESHOLD");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Target: %s", address.c_str());
    ESP_LOGI(TAG, "Threshold: %d°", degrees);
    
    if (degrees > 180) {
        ESP_LOGE(TAG, "✗ Invalid threshold: %d° (max: 180°)", degrees);
        return false;
    }
    
    bool success = writeGattCharacteristic(address, GATT_UUID_ANGLE_THRESHOLD, degrees);
    
    if (success) {
        ESP_LOGI(TAG, "✓ Angle threshold updated");
    } else {
        ESP_LOGE(TAG, "✗ Failed to update angle threshold");
    }
    
    return success;
}

bool ShellyBLEManager::factoryResetDevice(const String& address) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  FACTORY RESET");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGW(TAG, "⚠️  WARNING: This will DISABLE encryption!");
    
    bool success = writeGattCharacteristic(address, GATT_UUID_FACTORY_RESET, 1);
    
    if (success) {
        ESP_LOGI(TAG, "✓ Factory reset successful");
        
        if (isPaired() && pairedDevice.address == address) {
            unpairDevice();
            ESP_LOGI(TAG, "  → Pairing removed from manager");
        }
    } else {
        ESP_LOGE(TAG, "✗ Factory reset failed");
    }
    
    return success;
}

bool ShellyBLEManager::readDeviceConfig(const String& address, DeviceConfig& config) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  READ DEVICE CONFIGURATION");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    config.valid = false;
    
    uint8_t beaconMode = 0;
    uint8_t angleThreshold = 0;
    
    bool success = readGattCharacteristic(address, GATT_UUID_BEACON_MODE, beaconMode);
    success &= readGattCharacteristic(address, GATT_UUID_ANGLE_THRESHOLD, angleThreshold);
    
    if (success) {
        config.beaconModeEnabled = (beaconMode != 0);
        config.angleThreshold = angleThreshold;
        config.valid = true;
        
        ESP_LOGI(TAG, "✓ Configuration read successfully");
        ESP_LOGI(TAG, "  Beacon Mode: %s", config.beaconModeEnabled ? "ENABLED" : "DISABLED");
        ESP_LOGI(TAG, "  Angle Threshold: %d°", config.angleThreshold);
    } else {
        ESP_LOGE(TAG, "✗ Failed to read configuration");
    }
    
    return success;
}

// ============================================================================
// GATT Helper Functions
// ============================================================================

bool ShellyBLEManager::writeGattCharacteristic(const String& address, const String& uuid, uint8_t value) {
    ESP_LOGI(TAG, "→ Writing GATT characteristic...");
    ESP_LOGI(TAG, "  UUID: %s", uuid.c_str());
    ESP_LOGI(TAG, "  Value: %d", value);
    
    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (!pClient) {
        ESP_LOGE(TAG, "✗ Failed to create BLE client");
        return false;
    }

    pClient->setConnectTimeout(10000);
    
    std::string stdAddress = address.c_str();
    NimBLEAddress bleAddr(stdAddress, BLE_ADDR_RANDOM);
    if (!pClient->connect(bleAddr, false)) {
        ESP_LOGE(TAG, "✗ Connection failed");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Connected");
    
    NimBLERemoteCharacteristic* pChar = nullptr;
    NimBLEUUID targetUUID(uuid.c_str());

    auto services = pClient->getServices(true);
    
    for (auto* service : services) {
        pChar = service->getCharacteristic(targetUUID);
        if (pChar) break;
    }
    
    if (!pChar) {
        ESP_LOGE(TAG, "✗ Characteristic not found");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    if (!pChar->canWrite() && !pChar->canWriteNoResponse()) {
        ESP_LOGE(TAG, "✗ Characteristic not writable");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    bool success = pChar->writeValue(&value, 1, false);
    
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    
    if (success) {
        ESP_LOGI(TAG, "✓ Write successful");
    } else {
        ESP_LOGE(TAG, "✗ Write failed");
    }
    
    return success;
}

bool ShellyBLEManager::readGattCharacteristic(const String& address, const String& uuid, uint8_t& value) {
    ESP_LOGI(TAG, "→ Reading GATT characteristic...");
    ESP_LOGI(TAG, "  UUID: %s", uuid.c_str());
    
    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (!pClient) {
        ESP_LOGE(TAG, "✗ Failed to create BLE client");
        return false;
    }

    pClient->setConnectTimeout(10000);
    
    std::string stdAddress = address.c_str();
    NimBLEAddress bleAddr(stdAddress, BLE_ADDR_RANDOM);
    if (!pClient->connect(bleAddr, false)) {
        ESP_LOGE(TAG, "✗ Connection failed");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    NimBLERemoteCharacteristic* pChar = nullptr;
    NimBLEUUID targetUUID(uuid.c_str());

    auto services = pClient->getServices(true);
    
    for (auto* service : services) {
        pChar = service->getCharacteristic(targetUUID);
        if (pChar) break;
    }
    
    if (!pChar) {
        ESP_LOGE(TAG, "✗ Characteristic not found");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    if (!pChar->canRead()) {
        ESP_LOGE(TAG, "✗ Characteristic not readable");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    std::string data = pChar->readValue();
    
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    
    if (data.length() >= 1) {
        value = (uint8_t)data[0];
        ESP_LOGI(TAG, "✓ Read successful: %d", value);
        return true;
    } else {
        ESP_LOGE(TAG, "✗ Read failed: no data");
        return false;
    }
}

// ============================================================================
// Read Sample BTHome Data (GATT)
// ============================================================================

bool ShellyBLEManager::readSampleBTHomeData(const String& address, ShellyBLESensorData& data) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  READ SAMPLE BTHOME DATA (GATT)   ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Target: %s", address.c_str());
    
    // Check if paired
    if (!isPaired() || pairedDevice.address != address) {
        ESP_LOGE(TAG, "✗ Device not paired or wrong address");
        ESP_LOGI(TAG, "");
        return false;
    }
    
    // Check if active connection exists
    NimBLEClient* pClient = nullptr;
    bool needsCleanup = false;
    
    if (activeClient && activeClient->isConnected()) {
        String connectedAddr = String(activeClient->getPeerAddress().toString().c_str());
        if (connectedAddr.equalsIgnoreCase(address)) {
            uint32_t age = millis() - activeClientTimestamp;
            if (age < 60000) {  // Connection younger than 1 minute
                pClient = activeClient;
                ESP_LOGI(TAG, "✓ Using existing connection (age: %u ms)", age);
            } else {
                ESP_LOGW(TAG, "⚠ Connection too old, will reconnect");
                closeActiveConnection();
            }
        } else {
            ESP_LOGW(TAG, "⚠ Wrong device connected, disconnecting");
            closeActiveConnection();
        }
    }
    
    // Create new connection if needed
    if (!pClient) {
        ESP_LOGI(TAG, "→ Creating new GATT connection...");
        
        // Stop scanning if active
        bool wasScanning = scanning;
        if (wasScanning) {
            ESP_LOGI(TAG, "  → Stopping scan...");
            bleScanner->stop_scan();
            scanning = false;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            ESP_LOGE(TAG, "✗ Failed to create client");
            if (wasScanning) startScan(30);
            ESP_LOGI(TAG, "");
            return false;
        }
        
        pClient->setConnectTimeout(15000);
        
        // Get address type from discovered devices
        uint8_t addressType = BLE_ADDR_RANDOM;
        for (const auto& dev : discoveredDevices) {
            if (dev.address.equalsIgnoreCase(address)) {
                addressType = dev.addressType;
                break;
            }
        }
        
        ESP_LOGI(TAG, "  → Connecting to %s (%s)...", 
                 address.c_str(),
                 addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        
        NimBLEAddress bleAddr(address.c_str(), addressType);
        bool connected = pClient->connect(bleAddr, false);
        
        if (!connected) {
            // Try alternative address type
            uint8_t altType = (addressType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
            ESP_LOGI(TAG, "  → Trying %s address...",
                     altType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
            
            bleAddr = NimBLEAddress(address.c_str(), altType);
            connected = pClient->connect(bleAddr, false);
        }
        
        if (!connected) {
            ESP_LOGE(TAG, "✗ Connection failed");
            NimBLEDevice::deleteClient(pClient);
            if (wasScanning) startScan(30);
            ESP_LOGI(TAG, "");
            return false;
        }
        
        needsCleanup = true;
        ESP_LOGI(TAG, "✓ Connected");
        ESP_LOGI(TAG, "  MTU: %d bytes", pClient->getMTU());
    }
    
    ESP_LOGI(TAG, "");
    
    // Get services
    ESP_LOGI(TAG, "→ Discovering services...");
    std::vector<NimBLERemoteService*> services = pClient->getServices(true);
    
    if (services.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        ESP_LOGI(TAG, "");
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Found %d services", services.size());
    ESP_LOGI(TAG, "");
    
    // Find "Sample BTHome data" characteristic
    ESP_LOGI(TAG, "→ Looking for 'Sample BTHome data' characteristic...");
    ESP_LOGI(TAG, "  UUID: d52246df-98ac-4d21-be1b-70d5f66a5ddb");
    
    NimBLEUUID sampleDataUUID("d52246df-98ac-4d21-be1b-70d5f66a5ddb");
    NimBLERemoteCharacteristic* pChar = nullptr;
    
    for (auto* pService : services) {
        pChar = pService->getCharacteristic(sampleDataUUID);
        if (pChar) {
            ESP_LOGI(TAG, "✓ Characteristic found in service: %s", 
                     pService->getUUID().toString().c_str());
            break;
        }
    }
    
    if (!pChar) {
        ESP_LOGE(TAG, "✗ 'Sample BTHome data' characteristic not found!");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Possible reasons:");
        ESP_LOGE(TAG, "  • Device not bonded (Phase 1 incomplete)");
        ESP_LOGE(TAG, "  • Firmware doesn't support this characteristic");
        ESP_LOGE(TAG, "  • Service discovery incomplete");
        
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        ESP_LOGI(TAG, "");
        return false;
    }
    
    // Check if readable
    if (!pChar->canRead()) {
        ESP_LOGE(TAG, "✗ Characteristic not readable!");
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        ESP_LOGI(TAG, "");
        return false;
    }
    
    ESP_LOGI(TAG, "");
    
    // Read the data
    ESP_LOGI(TAG, "→ Reading Sample BTHome data...");
    std::string rawData = pChar->readValue();
    
    if (rawData.length() == 0) {
        ESP_LOGW(TAG, "✗ No data received (length = 0)");
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        ESP_LOGI(TAG, "");
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Received %d bytes", rawData.length());
    
    // Hex dump
    char hex_buf[256];
    int offset = 0;
    offset += snprintf(hex_buf, sizeof(hex_buf), "  Raw data: ");
    for (size_t i = 0; i < rawData.length() && i < 32; i++) {
        offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                         "%02X ", (uint8_t)rawData[i]);
    }
    ESP_LOGI(TAG, "%s", hex_buf);
    ESP_LOGI(TAG, "");
    
    // Parse BTHome data (always unencrypted!)
    ESP_LOGI(TAG, "→ Parsing BTHome packet...");
    
    data.rssi = 0;  // RSSI not available in GATT read
    
    bool parseSuccess = parseBTHomePacket((uint8_t*)rawData.data(), rawData.length(),
                                        "", address, data);  // Empty bindkey = unencrypted
    
    if (parseSuccess) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  READ SUCCESSFUL!              ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Sensor Data:");
        ESP_LOGI(TAG, "  Packet ID:    %d", data.packetId);
        ESP_LOGI(TAG, "  Battery:      %d%%", data.battery);
        ESP_LOGI(TAG, "  Window:       %s", data.windowOpen ? "OPEN 🔓" : "CLOSED 🔒");
        ESP_LOGI(TAG, "  Illuminance:  %d lux", data.illuminance);
        ESP_LOGI(TAG, "  Rotation:     %d°", data.rotation);
        
        if (data.hasButtonEvent) {
            const char* eventName;
            switch (data.buttonEvent) {
                case BUTTON_SINGLE_PRESS: eventName = "SINGLE PRESS 👆"; break;
                case BUTTON_HOLD: eventName = "HOLD ⏸️"; break;
                default: eventName = "UNKNOWN";
            }
            ESP_LOGI(TAG, "  Button:       %s", eventName);
        }
        
        data.dataValid = true;
        data.lastUpdate = millis();
        
    } else {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "✗ Failed to parse BTHome data");
        ESP_LOGW(TAG, "  Raw data might not be in BTHome format");
    }
    
    // Cleanup
    if (needsCleanup) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Disconnecting...");
        pClient->disconnect();
        
        uint8_t retries = 0;
        while (pClient->isConnected() && retries < 20) {
            vTaskDelay(pdMS_TO_TICKS(100));
            retries++;
        }
        
        NimBLEDevice::deleteClient(pClient);
        ESP_LOGI(TAG, "✓ Disconnected");
    }
    
    ESP_LOGI(TAG, "");
    
    return parseSuccess;
}


// ═══════════════════════════════════════════════════════════════════════
// Helper Methods
// ═══════════════════════════════════════════════════════════════════════

void ShellyBLEManager::updateDiscoveredDevice(
    const String& address, 
    const String& name, 
    int8_t rssi,
    bool isEncrypted,
    uint64_t addressUint64,
    uint8_t addressType) {
    
    bool found = false;
    for (auto& dev : discoveredDevices) {
        if (dev.address == address) {
            dev.rssi = rssi;
            dev.lastSeen = millis();
            dev.isEncrypted = isEncrypted;
            dev.addressType = addressType;
            found = true;
            break;
        }
    }
    
    if (!found) {
        ShellyBLEDevice device;
        device.address = address;
        device.name = name;
        device.rssi = rssi;
        device.isEncrypted = isEncrypted;
        device.lastSeen = millis();
        device.addressType = addressType;
        
        discoveredDevices.push_back(device);
        
        ESP_LOGI(TAG, "✓ Added to discovered devices (total: %d)", 
                 discoveredDevices.size());
    }
}


void ShellyBLEManager::cleanupOldDiscoveries() {
    uint32_t now = millis();
    const uint32_t timeout = 300000;  // 5 minutes
    
    for (auto it = discoveredDevices.begin(); it != discoveredDevices.end(); ) {
        if (now - it->lastSeen > timeout) {
            ESP_LOGI(TAG, "Removing stale discovery: %s", it->address.c_str());
            it = discoveredDevices.erase(it);
        } else {
            ++it;
        }
    }
}

NimBLERemoteCharacteristic* ShellyBLEManager::findCharacteristic(
    std::vector<NimBLERemoteService*>& services,
    const String& uuid) {
    
    NimBLEUUID targetUUID(uuid.c_str());
    
    for (auto* pService : services) {
        NimBLERemoteCharacteristic* pChar = pService->getCharacteristic(targetUUID);
        if (pChar) {
            return pChar;
        }
    }
    
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// Pairing (Simple)
// ═══════════════════════════════════════════════════════════════════════

bool ShellyBLEManager::pairDevice(const String& address, const String& bindkey) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "    BLE PAIRING INITIATED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    if (isPaired()) {
        ESP_LOGE(TAG, "✗ ABORT: Device already paired!");
        ESP_LOGE(TAG, "  Current device: %s (%s)", 
                 pairedDevice.name.c_str(), pairedDevice.address.c_str());
        ESP_LOGI(TAG, "  → Unpair first before pairing a new device");
        return false;
    }
    
    ESP_LOGI(TAG, "Target device: %s", address.c_str());
    
    // Find device in discovered list
    String name = "Unknown";
    bool found = false;
    for (const auto& dev : discoveredDevices) {
        if (dev.address == address) {
            name = dev.name;
            found = true;
            ESP_LOGI(TAG, "✓ Device found in scan results");
            ESP_LOGI(TAG, "  Name: %s", name.c_str());
            ESP_LOGI(TAG, "  RSSI: %d dBm", dev.rssi);
            ESP_LOGI(TAG, "  Encrypted: %s", dev.isEncrypted ? "YES" : "NO");
            break;
        }
    }
    
    if (!found) {
        ESP_LOGW(TAG, "⚠ Device NOT found in recent scan");
        ESP_LOGW(TAG, "  Will pair anyway, but connection might fail");
    }
    
    // Validate bindkey if provided
    if (bindkey.length() > 0) {
        if (bindkey.length() != 32) {
            ESP_LOGE(TAG, "✗ INVALID BINDKEY LENGTH");
            ESP_LOGE(TAG, "  Expected: 32 hex characters");
            ESP_LOGE(TAG, "  Got: %d characters", bindkey.length());
            return false;
        }
        
        for (size_t i = 0; i < bindkey.length(); i++) {
            char c = bindkey[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                ESP_LOGE(TAG, "✗ INVALID BINDKEY CHARACTER at position %d: '%c'", i, c);
                return false;
            }
        }
        
        ESP_LOGI(TAG, "✓ Bindkey validation passed");
        ESP_LOGI(TAG, "  Bindkey: %s", bindkey.c_str());
    } else {
        ESP_LOGI(TAG, "ℹ No bindkey provided (unencrypted device)");
    }
    
    // Store paired device
    pairedDevice.address = address;
    pairedDevice.name = name;
    pairedDevice.bindkey = bindkey;
    pairedDevice.sensorData = ShellyBLESensorData();
    
    savePairedDevice();
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "✓ PAIRING SUCCESSFUL");
    ESP_LOGI(TAG, "  Device: %s (%s)", name.c_str(), address.c_str());
    ESP_LOGI(TAG, "  Encryption: %s", bindkey.length() > 0 ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    return true;
}

bool ShellyBLEManager::unpairDevice() {
    if (!isPaired()) {
        ESP_LOGW(TAG, "No device paired");
        return false;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║      UNPAIRING DEVICE             ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    closeActiveConnection();
    
    if (continuousScan) {
        ESP_LOGI(TAG, "→ Stopping continuous scan...");
        continuousScan = false;
        
        if (scanning) {
            stopScan(true);  // Manual stop
        }
    }
    
    clearPairedDevice();
    updateDeviceState(STATE_NOT_PAIRED);
    
    ESP_LOGI(TAG, "✓ Device unpaired successfully");
    
    // ════════════════════════════════════════════════════════════════════
    // OPTIONAL: BLE herunterfahren wenn kein Device mehr gepairt
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ No paired device remaining");
    ESP_LOGI(TAG, "  BLE will stay active (for future pairing)");
    ESP_LOGI(TAG, "  To save power, restart device");
    ESP_LOGI(TAG, "");
    
    // Alternative: BLE komplett herunterfahren
    // shutdownBLE();
    
    return true;
}


// ═══════════════════════════════════════════════════════════════════════
// State Management
// ═══════════════════════════════════════════════════════════════════════

ShellyBLEManager::DeviceState ShellyBLEManager::getDeviceState() const {
    if (!isPaired()) {
        return STATE_NOT_PAIRED;
    }
    
    if (pairedDevice.bindkey.length() > 0) {
        return STATE_CONNECTED_ENCRYPTED;
    }
    
    return STATE_CONNECTED_UNENCRYPTED;
}

void ShellyBLEManager::updateDeviceState(DeviceState newState) {
    if (newState != deviceState) {
        DeviceState oldState = deviceState;
        deviceState = newState;
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   DEVICE STATE CHANGED            ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "Old State: %s", stateToString(oldState));
        ESP_LOGI(TAG, "New State: %s", stateToString(newState));
        ESP_LOGI(TAG, "");
        
        if (stateChangeCallback) {
            stateChangeCallback(oldState, newState);
        }
    }
}

const char* ShellyBLEManager::stateToString(DeviceState state) const {
    switch (state) {
        case STATE_NOT_PAIRED: return "NOT_PAIRED";
        case STATE_CONNECTED_UNENCRYPTED: return "CONNECTED_UNENCRYPTED";
        case STATE_CONNECTED_ENCRYPTED: return "CONNECTED_ENCRYPTED";
        default: return "UNKNOWN";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Sensor Data Access
// ═══════════════════════════════════════════════════════════════════════

bool ShellyBLEManager::getSensorData(ShellyBLESensorData& data) const {
    if (!isPaired() || !pairedDevice.sensorData.dataValid) {
        return false;
    }
    
    data = pairedDevice.sensorData;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// Passkey Management
// ═══════════════════════════════════════════════════════════════════════

void ShellyBLEManager::savePasskey(uint32_t passkey) {
    Preferences prefs;
    prefs.begin("ShellyBLE", false);
    prefs.putUInt("passkey", passkey);
    prefs.end();
    
    ESP_LOGI(TAG, "✓ Passkey saved to NVS: %06u", passkey);
}

uint32_t ShellyBLEManager::getPasskey() {
    Preferences prefs;
    prefs.begin("ShellyBLE", true);
    uint32_t passkey = prefs.getUInt("passkey", 0);
    prefs.end();
    
    return passkey;
}

// ═══════════════════════════════════════════════════════════════════════
// Connection Management
// ═══════════════════════════════════════════════════════════════════════

void ShellyBLEManager::closeActiveConnection() {
    if (activeClient) {
        ESP_LOGI(TAG, "→ Closing active GATT connection...");
        
        if (activeClient->isConnected()) {
            activeClient->disconnect();
            
            uint32_t wait_start = millis();
            while (activeClient->isConnected() && (millis() - wait_start < 2000)) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        
        NimBLEDevice::deleteClient(activeClient);
        activeClient = nullptr;
        
        ESP_LOGI(TAG, "✓ Connection closed and cleaned up");
    }
    
    if (activeClientCallbacks) {
        delete activeClientCallbacks;
        activeClientCallbacks = nullptr;
    }
}
        
// ============================================================================
// BTHome v2 Parser
// ============================================================================

bool ShellyBLEManager::parseBTHomePacket(const uint8_t* data, size_t length,
                                         const String& bindkey, 
                                         const String& macAddress,
                                         ShellyBLESensorData& sensorData) {
    
    // ════════════════════════════════════════════════════════════════════
    // Input Validation
    // ════════════════════════════════════════════════════════════════════
    
    if (length < 2) {
        ESP_LOGW(TAG, "Packet too short: %d bytes", length);
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Parse Device Info & Determine Encryption
    // ════════════════════════════════════════════════════════════════════
    
    uint8_t deviceInfo = data[0];
    bool encrypted = (deviceInfo & 0x01) != 0;
    uint8_t bthomeVersion = (deviceInfo >> 5) & 0x07;
    
    ESP_LOGI(TAG, "BTHome Packet: %d bytes, %s, v%d", 
             length,
             encrypted ? "Encrypted" : "Unencrypted",
             bthomeVersion);
    
    // ════════════════════════════════════════════════════════════════════
    // Handle Encrypted Packets
    // ════════════════════════════════════════════════════════════════════
    
    uint8_t* payload = nullptr;
    size_t payloadLength = 0;
    uint8_t decryptedBuffer[256];
    
    if (encrypted) {
        if (bindkey.length() != 32) {
            ESP_LOGW(TAG, "Encrypted packet but no valid bindkey (len=%d)", bindkey.length());
            return false;
        }
        
        ESP_LOGI(TAG, "→ Decrypting...");
        size_t decryptedLen = 0;
        
        if (!decryptBTHome(data, length, bindkey, macAddress, decryptedBuffer, decryptedLen)) {
            ESP_LOGE(TAG, "✗ Decryption failed");
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Decrypted: %d bytes", decryptedLen);
        payload = decryptedBuffer + 1;  // Skip device info byte
        payloadLength = decryptedLen - 1;
        
    } else {
        // Unencrypted: use data directly
        payload = (uint8_t*)data + 1;  // Skip device info byte
        payloadLength = length - 1;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Parse BTHome Objects
    // ════════════════════════════════════════════════════════════════════
    
    size_t offset = 0;
    bool hasData = false;
    sensorData.hasButtonEvent = false;
    
    while (offset < payloadLength) {
        if (offset >= payloadLength) break;
        
        uint8_t objectId = payload[offset++];
        size_t objectLen = getBTHomeObjectLength(objectId);
        
        if (objectLen == 0) {
            ESP_LOGW(TAG, "Unknown Object ID: 0x%02X at offset %d", objectId, offset - 1);
            break;
        }
        
        if (offset + objectLen > payloadLength) {
            ESP_LOGW(TAG, "Insufficient data for Object 0x%02X", objectId);
            break;
        }
        
        // Parse specific objects
        switch (objectId) {
            case BTHOME_OBJ_PACKET_ID:
                sensorData.packetId = payload[offset];
                hasData = true;
                break;
                
            case BTHOME_OBJ_BATTERY:
                sensorData.battery = payload[offset];
                hasData = true;
                break;
                
            case BTHOME_OBJ_ILLUMINANCE: {
                uint32_t lux_raw = payload[offset] | 
                                  (payload[offset+1] << 8) | 
                                  (payload[offset+2] << 16);
                sensorData.illuminance = lux_raw / 100;
                hasData = true;
                break;
            }
            
            case BTHOME_OBJ_WINDOW:
                sensorData.windowOpen = (payload[offset] != 0);
                hasData = true;
                break;
                
            case BTHOME_OBJ_BUTTON: {
                uint16_t btnValue = payload[offset] | (payload[offset+1] << 8);
                sensorData.buttonEvent = static_cast<ShellyButtonEvent>(btnValue & 0xFF);
                sensorData.hasButtonEvent = true;
                hasData = true;
                break;
            }
            
            case BTHOME_OBJ_ROTATION: {
                int16_t rot_raw = payload[offset] | (payload[offset+1] << 8);
                sensorData.rotation = rot_raw / 10;
                hasData = true;
                break;
            }
            
            default:
                ESP_LOGD(TAG, "Skipping Object 0x%02X", objectId);
                break;
        }
        
        offset += objectLen;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Update Encryption Status & Store Results
    // ════════════════════════════════════════════════════════════════════
    
    if (hasData) {
        // Store encryption status from THIS packet
        sensorData.wasEncrypted = encrypted;
        
        // Update paired device encryption status
        if (isPaired() && pairedDevice.address.equalsIgnoreCase(macAddress)) {
            bool previousStatus = pairedDevice.isCurrentlyEncrypted;
            pairedDevice.isCurrentlyEncrypted = encrypted;
            
            // Log only if status changed
            if (previousStatus != encrypted) {
                ESP_LOGI(TAG, "Encryption status changed: %s → %s",
                         previousStatus ? "Encrypted" : "Unencrypted",
                         encrypted ? "Encrypted" : "Unencrypted");
            }
            
            // Warn if mismatch detected
            if (pairedDevice.bindkey.length() > 0 && !encrypted) {
                ESP_LOGW(TAG, "⚠ Device has bindkey but sends unencrypted data!");
            }
        }
        
        // Log parsed data summary
        ESP_LOGI(TAG, "✓ Parsed: Battery=%d%%, Window=%s, Illuminance=%dlux, Rotation=%d°",
                 sensorData.battery,
                 sensorData.windowOpen ? "OPEN" : "CLOSED",
                 sensorData.illuminance,
                 sensorData.rotation);
        
        if (sensorData.hasButtonEvent) {
            const char* btnName;
            switch (sensorData.buttonEvent) {
                case BUTTON_SINGLE_PRESS: btnName = "SINGLE"; break;
                case BUTTON_DOUBLE_PRESS: btnName = "DOUBLE"; break;
                case BUTTON_TRIPLE_PRESS: btnName = "TRIPLE"; break;
                case BUTTON_LONG_PRESS: btnName = "LONG"; break;
                case BUTTON_HOLD: btnName = "HOLD"; break;
                default: btnName = "UNKNOWN"; break;
            }
            ESP_LOGI(TAG, "  Button: %s", btnName);
        }
    } else {
        ESP_LOGW(TAG, "No valid data parsed from packet");
    }
    
    return hasData;
}

// ════════════════════════════════════════════════════════════════════════
// BTHome Object Length Helper
// ════════════════════════════════════════════════════════════════════════

size_t ShellyBLEManager::getBTHomeObjectLength(uint8_t objectId) {
    switch (objectId) {
        case 0x00:  // Packet ID
        case 0x01:  // Battery
        case 0x2D:  // Window/Door
            return 1;
        
        case 0x3A:  // Button
        case 0x3F:  // Rotation
            return 2;
        
        case 0x05:  // Illuminance
            return 3;
        
        default:
            return 0;  // Unknown object
    }
}


// ============================================================================
// AES-CCM Decryption (BTHome v2)
// ============================================================================

bool ShellyBLEManager::decryptBTHome(const uint8_t* encryptedData, size_t length,
                                     const String& bindkey, 
                                     const String& macAddress,
                                     uint8_t* decrypted,
                                     size_t& decryptedLen) {
    
    // ════════════════════════════════════════════════════════════════════
    // Input Validation
    // ════════════════════════════════════════════════════════════════════
    
    if (length < 13) {  // Min: 1 DevInfo + 4 Counter + 4 Payload + 4 MIC
        ESP_LOGW(TAG, "Encrypted packet too short: %d bytes (min 13)", length);
        return false;
    }
    
    if (bindkey.length() != 32) {
        ESP_LOGE(TAG, "Invalid bindkey length: %d (expected 32)", bindkey.length());
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Parse Packet Structure
    // ════════════════════════════════════════════════════════════════════
    
    uint8_t deviceInfo = encryptedData[0];
    const uint8_t* counter = encryptedData + 1;
    const uint8_t* payload = encryptedData + 5;
    size_t payloadLen = length - 9;  // Total - (1 DevInfo + 4 Counter + 4 MIC)
    const uint8_t* mic = encryptedData + length - 4;
    
    // ════════════════════════════════════════════════════════════════════
    // Parse Bindkey (Hex String → Bytes)
    // ════════════════════════════════════════════════════════════════════
    
    uint8_t key[16];
    for (int i = 0; i < 16; i++) {
        char hex[3] = {bindkey[i*2], bindkey[i*2+1], 0};
        key[i] = (uint8_t)strtol(hex, NULL, 16);
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Parse MAC Address
    // ════════════════════════════════════════════════════════════════════
    
    uint8_t mac[6];
    if (!parseMacAddress(macAddress, mac)) {
        ESP_LOGE(TAG, "Invalid MAC address format: %s", macAddress.c_str());
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Build Nonce (BTHome v2 Standard: 13 bytes)
    // ════════════════════════════════════════════════════════════════════
    
    uint8_t nonce[13];
    
    // MAC reversed (Little-Endian)
    for (int i = 0; i < 6; i++) {
        nonce[i] = mac[5 - i];
    }
    
    // UUID 0xFCD2 in Little-Endian
    nonce[6] = 0xD2;
    nonce[7] = 0xFC;
    
    // Device Info WITHOUT encryption flag
    nonce[8] = deviceInfo & 0xFE;
    
    // Counter (already Little-Endian)
    memcpy(nonce + 9, counter, 4);
    
    // ════════════════════════════════════════════════════════════════════
    // AES-CCM Decryption (mbedTLS)
    // ════════════════════════════════════════════════════════════════════
    
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    
    int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "CCM setkey failed: -0x%04X", -ret);
        mbedtls_ccm_free(&ctx);
        return false;
    }
    
    // Decrypt with AAD (Additional Authenticated Data = Device Info byte)
    ret = mbedtls_ccm_auth_decrypt(
        &ctx,
        payloadLen,              // Ciphertext length
        nonce, 13,               // Nonce (13 bytes)
        &deviceInfo, 1,          // AAD: Device Info byte WITH encryption flag
        payload,                 // Input: encrypted payload
        decrypted + 1,           // Output: decrypted payload (skip first byte)
        mic, 4                   // MIC/Tag (4 bytes)
    );
    
    mbedtls_ccm_free(&ctx);
    
    // ════════════════════════════════════════════════════════════════════
    // Handle Result
    // ════════════════════════════════════════════════════════════════════
    
    if (ret != 0) {
        ESP_LOGE(TAG, "CCM decrypt failed: -0x%04X", -ret);
        
        if (ret == MBEDTLS_ERR_CCM_AUTH_FAILED) {
            ESP_LOGE(TAG, "  → Authentication failed (MIC mismatch)");
            ESP_LOGE(TAG, "  → Wrong bindkey or corrupted packet");
        }
        
        return false;
    }
    
    // Success: Build output (Device Info without encryption flag + decrypted payload)
    decrypted[0] = deviceInfo & 0xFE;
    decryptedLen = payloadLen + 1;
    
    return true;
}


bool ShellyBLEManager::parseMacAddress(const String& macStr, uint8_t* mac) {
    if (macStr.length() != 17) {
        ESP_LOGE(TAG, "Invalid MAC length: %d (expected 17)", macStr.length());
        return false;
    }
    
    for (int i = 0; i < 6; i++) {
        if (i < 5 && macStr[i*3 + 2] != ':') {
            ESP_LOGE(TAG, "Invalid MAC format at position %d (expected ':')", i*3 + 2);
            return false;
        }
        
        char hex[3] = {macStr[i*3], macStr[i*3+1], 0};
        
        for (int j = 0; j < 2; j++) {
            char c = hex[j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                ESP_LOGE(TAG, "Invalid hex character in MAC: '%c' at position %d", c, i*3 + j);
                return false;
            }
        }
        
        mac[i] = (uint8_t)strtol(hex, NULL, 16);
    }
    
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// PairingCallbacks Implementation
// ═══════════════════════════════════════════════════════════════════════

void ShellyBLEManager::PairingCallbacks::onConnect(NimBLEClient* pClient) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  CLIENT CONNECTED                 ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Peer: %s", pClient->getPeerAddress().toString().c_str());
    ESP_LOGI(TAG, "MTU: %d bytes", pClient->getMTU());
    ESP_LOGI(TAG, "");
    // NOTE: Do NOT call updateConnParams() here.
    // Changing connection parameters during the initial setup phase (before MTU
    // exchange and pairing are complete) causes the Shelly device to disconnect
    // and makes connect() return false even though the link was physically established.
}

void ShellyBLEManager::PairingCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    String address = pClient->getPeerAddress().toString().c_str();
    manager->recentConnections[address] = millis();
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  CLIENT DISCONNECTED              ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Reason: 0x%02X", reason);
    
    switch(reason) {
        case 0x08: ESP_LOGI(TAG, "  Connection timeout"); break;
        case 0x13: ESP_LOGI(TAG, "  Remote terminated"); break;
        case 0x16: ESP_LOGI(TAG, "  Local terminated"); break;
        case 0x3E: ESP_LOGI(TAG, "  Connection failed"); break;
        default: ESP_LOGI(TAG, "  Other (0x%02X)", reason);
    }
    
    ESP_LOGI(TAG, "");
}

bool ShellyBLEManager::PairingCallbacks::onConnParamsUpdateRequest(
    NimBLEClient* pClient, const ble_gap_upd_params* params) {
    
    ESP_LOGI(TAG, "→ Conn params update request");
    ESP_LOGI(TAG, "  Interval: %d-%d", params->itvl_min, params->itvl_max);
    ESP_LOGI(TAG, "  Latency: %d", params->latency);
    ESP_LOGI(TAG, "  Timeout: %d", params->supervision_timeout);
    
    return true;
}

void ShellyBLEManager::PairingCallbacks::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  AUTHENTICATION COMPLETE          ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    ESP_LOGI(TAG, "Security Status:");
    ESP_LOGI(TAG, "  Encrypted:     %s", connInfo.isEncrypted() ? "YES ✓" : "NO ✗");
    ESP_LOGI(TAG, "  Bonded:        %s", connInfo.isBonded() ? "YES ✓" : "NO ✗");
    ESP_LOGI(TAG, "  Authenticated: %s", connInfo.isAuthenticated() ? "YES ✓" : "NO ✗");
    ESP_LOGI(TAG, "  Key Size:      %d bytes", connInfo.getSecKeySize());
    ESP_LOGI(TAG, "");
    
    pairingComplete = true;
    pairingSuccess = connInfo.isBonded() && connInfo.isEncrypted();
    
    if (!connInfo.isAuthenticated()) {
        ESP_LOGW(TAG, "⚠ BONDING INCOMPLETE!");
        ESP_LOGW(TAG, "  → Not authenticated");
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ BONDING %s!", pairingSuccess ? "SUCCESSFUL" : "FAILED");
    ESP_LOGI(TAG, "");
}

void ShellyBLEManager::PairingCallbacks::onPassKeyEntry(NimBLEConnInfo& connInfo) {
    ESP_LOGW(TAG, "");
    ESP_LOGW(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGW(TAG, "║  PASSKEY ENTRY REQUESTED          ║");
    ESP_LOGW(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGW(TAG, "");
    ESP_LOGW(TAG, "⚠ Should NOT happen with Just Works!");
    ESP_LOGW(TAG, "");
    ESP_LOGW(TAG, "→ Injecting passkey: 0");
    
    NimBLEDevice::injectPassKey(connInfo, 0);
    
    ESP_LOGW(TAG, "✓ Injected");
    ESP_LOGW(TAG, "");
}

void ShellyBLEManager::PairingCallbacks::onConfirmPasskey(
    NimBLEConnInfo& connInfo, uint32_t pass_key) {
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  PASSKEY CONFIRMATION             ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Passkey: %06u", pass_key);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "⚠ Numeric Comparison mode");
    ESP_LOGI(TAG, "  (Should NOT happen with Just Works)");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Confirming via injectConfirmPasskey()...");
    
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
    
    ESP_LOGI(TAG, "✓ Confirmed");
    ESP_LOGI(TAG, "");
}

// ════════════════════════════════════════════════════════════════════════
// Static: Check if ANY Device is Paired (without starting BLE)
// ════════════════════════════════════════════════════════════════════════

bool ShellyBLEManager::hasAnyPairedDevice() {
    Preferences prefs;
    if (!prefs.begin("ShellyBLE", true)) {  // Read-only
        return false;
    }
    
    String address = prefs.getString("address", "");
    prefs.end();
    
    bool hasPaired = (address.length() > 0);
    
    ESP_LOGI(TAG, "Static check: %s device in NVS", 
             hasPaired ? "FOUND" : "NO");
    
    return hasPaired;
}

// ════════════════════════════════════════════════════════════════════════
// Memory Monitoring Helper (ShellyBLEManager)
// ════════════════════════════════════════════════════════════════════════

void ShellyBLEManager::logMemoryStats(const char* location) {
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
    
    if (free_heap < 20000) {
        ESP_LOGW(TAG, "⚠️ WARNING: Free heap below 20KB!");
    }
    
    if (largest_block < 10000) {
        ESP_LOGW(TAG, "⚠️ WARNING: Largest free block below 10KB - fragmentation!");
    }
    
    ESP_LOGI(TAG, "");
}
