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

// Für AES/CCM Decryption (BTHome)
#include <mbedtls/aes.h>
#include <mbedtls/ccm.h>

static const char* TAG = "ShellyBLE";

// ============================================================================
// Constructor / Destructor
// ============================================================================

ShellyBLEManager::ShellyBLEManager() 
    : initialized(false), scanning(false), continuousScan(false),
      stopOnFirstMatch(false),activeClientTimestamp(0),
      pBLEScan(nullptr), scanCallback(nullptr), sensorDataCallback(nullptr),
      deviceState(STATE_NOT_PAIRED) {
}

ShellyBLEManager::~ShellyBLEManager() {
    end();
}

// ============================================================================
// Initialization
// ============================================================================

bool ShellyBLEManager::begin() {
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }
    
    ESP_LOGI(TAG, "Initializing Shelly BLE Manager...");
    
    pBLEScan = NimBLEDevice::getScan();
    if (!pBLEScan) {
        ESP_LOGE(TAG, "Failed to get BLE scan object");
        return false;
    }
    
    scanCallback = new ScanCallback(this);
    pBLEScan->setScanCallbacks(scanCallback, false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(300);
    pBLEScan->setWindow(50);
    
    loadPairedDevice();
    
    initialized = true;
    ESP_LOGI(TAG, "Initialized successfully");
    
    if (isPaired()) {
        // Paired Device hat Bindkey? → Encrypted
        if (pairedDevice.bindkey.length() > 0) {
            updateDeviceState(STATE_CONNECTED_ENCRYPTED);
        } else {
            updateDeviceState(STATE_CONNECTED_UNENCRYPTED);
        }
    } else {
        updateDeviceState(STATE_NOT_PAIRED);
    }
    
    return true;
}

void ShellyBLEManager::end() {
    if (!initialized) return;
    
    stopScan();
    
    if (scanCallback) {
        delete scanCallback;
        scanCallback = nullptr;
    }
    
    initialized = false;
    ESP_LOGI(TAG, "Shut down");
}

// ============================================================================
// Persistence
// ============================================================================

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
        
        ESP_LOGI(TAG, "Loaded paired device: %s", address.c_str());
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
        ESP_LOGI(TAG, "Saved paired device: %s", pairedDevice.address.c_str());
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

// ============================================================================
// Discovery / Scanning
// ============================================================================

void ShellyBLEManager::startScan(uint16_t durationSeconds, bool stopOnFirst) {
    if (!initialized) {
        ESP_LOGE(TAG, "✗ Cannot start scan: Manager not initialized");
        return;
    }
    
    if (scanning) {
        ESP_LOGW(TAG, "⚠ Scan already in progress");
        return;
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "    BLE SCAN STARTED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Duration: %d seconds", durationSeconds);
    ESP_LOGI(TAG, "Scan type: %s", continuousScan ? "CONTINUOUS" : "DISCOVERY");
    
    stopOnFirstMatch = stopOnFirst;
    if (stopOnFirstMatch) {
        ESP_LOGI(TAG, "Mode: STOP ON FIRST SHELLY BLU DOOR/WINDOW");
    }
    
    ESP_LOGI(TAG, "Target devices: Shelly BLU Door/Window (SBDW-*)");
    
    if (isPaired()) {
        ESP_LOGI(TAG, "Paired device: %s (%s)", 
                 pairedDevice.name.c_str(), pairedDevice.address.c_str());
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    if (!continuousScan) {
        discoveredDevices.clear();
        ESP_LOGI(TAG, "→ Discovery scan: Cleared previous results");
    }
    
    scanning = true;
    
    uint32_t durationMs = durationSeconds * 1000;
    bool started = pBLEScan->start(durationMs, false);
    
    if (!started) {
        ESP_LOGE(TAG, "✗ pBLEScan->start() failed!");
        scanning = false;
        stopOnFirstMatch = false;
    } else {
        ESP_LOGI(TAG, "✓ Scan started successfully");
    }
}

void ShellyBLEManager::stopScan() {
    if (!scanning) {
        ESP_LOGW(TAG, "No scan in progress");
        return;
    }
    
    continuousScan = false;
    stopOnFirstMatch = false;
    
    pBLEScan->stop();
    scanning = false;
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "BLE SCAN STOPPED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Total Shelly BLU devices found: %d", discoveredDevices.size());
    
    if (discoveredDevices.size() > 0) {
        ESP_LOGI(TAG, "Discovered devices:");
        for (size_t i = 0; i < discoveredDevices.size(); i++) {
            const auto& dev = discoveredDevices[i];
            ESP_LOGI(TAG, "  [%d] %s", i+1, dev.name.c_str());
            ESP_LOGI(TAG, "      MAC: %s | RSSI: %d dBm | Encrypted: %s",
                     dev.address.c_str(), dev.rssi, dev.isEncrypted ? "Yes" : "No");
        }
    } else {
        ESP_LOGI(TAG, "⚠ No Shelly BLU Door/Window sensors found");
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
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
        ESP_LOGW(TAG, "Please pair a device first.");
        ESP_LOGW(TAG, "");
        return;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  CONTINUOUS BLE SCAN              ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Paired device: %s (%s)", 
             pairedDevice.name.c_str(), pairedDevice.address.c_str());
    ESP_LOGI(TAG, "Encryption: %s", 
             pairedDevice.bindkey.length() > 0 ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "");
    
    continuousScan = true;
    startScan(30, false);
}

// ============================================================================
// Pairing - Simple (für unencrypted devices)
// ============================================================================

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
    
    // ✅ NEU: Active Connection schließen
    closeActiveConnection();
    
    if (continuousScan) {
        ESP_LOGI(TAG, "→ Stopping continuous scan...");
        continuousScan = false;
        
        if (scanning) {
            stopScan();
        }
    }
    
    clearPairedDevice();
    updateDeviceState(STATE_NOT_PAIRED);
    
    ESP_LOGI(TAG, "✓ Device unpaired successfully");
    ESP_LOGI(TAG, "");
    
    return true;
}

// ============================================================================
// ✅ NEUER 2-PHASEN-WORKFLOW
// ============================================================================

// ────────────────────────────────────────────────────────────────────────────
// PHASE 1: Connect Device (Bonding ohne Encryption)
// ────────────────────────────────────────────────────────────────────────────

bool ShellyBLEManager::connectDevice(const String& address) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PHASE 1: BONDING + CONNECT      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // Prüfe ob NimBLE initialisiert ist
    if (!NimBLEDevice::isInitialized()) {
        ESP_LOGE(TAG, "✗ NimBLE not initialized!");
        return false;
    }
    
    // Prüfe ob bereits eine aktive Connection existiert
    if (activeClient && activeClient->isConnected()) {
        ESP_LOGW(TAG, "⚠ Already connected to a device");
        ESP_LOGI(TAG, "→ Disconnecting current device first...");
        closeActiveConnection();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // ========================================================================
    // DEVICE LOOKUP
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   DEVICE LOOKUP                   ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    String deviceName = "Unknown";
    uint8_t addressType = BLE_ADDR_RANDOM;
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
    
    // ========================================================================
    // SECURITY SETUP
    // ========================================================================
    
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
    
    // ========================================================================
    // GATT CONNECTION
    // ========================================================================
    
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
    activeClient->setConnectTimeout(30);
    
    ESP_LOGI(TAG, "→ Connecting...");
    
    NimBLEAddress peerAddress(address.c_str(), addressType);
    
    bool connected = false;
    
    ESP_LOGI(TAG, "→ Attempt 1/2: %s address type...",
             addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    
    if (activeClient->connect(peerAddress, false)) {
        connected = true;
        ESP_LOGI(TAG, "✓ Connected with %s address",
                 addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    } else {
        ESP_LOGW(TAG, "  Failed, trying alternative address type...");
        
        uint8_t altType = (addressType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
        peerAddress = NimBLEAddress(address.c_str(), altType);
        
        ESP_LOGI(TAG, "→ Attempt 2/2: %s address type...",
                 altType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        
        if (activeClient->connect(peerAddress, false)) {
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
    
    // ========================================================================
    // ✅ EXPLIZIT PAIRING ANFORDERN
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   INITIATE PAIRING                ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Requesting secure connection (bonding)...");
    ESP_LOGI(TAG, "  This will be AUTO-CONFIRMED (Just Works)");
    ESP_LOGI(TAG, "");
    
    // ✅ EXPLIZIT Pairing anfordern!
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
    
    // ========================================================================
    // WARTE AUF BONDING COMPLETE
    // ========================================================================
    
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
    
    // ========================================================================
    // SERVICE DISCOVERY
    // ========================================================================
    
    ESP_LOGI(TAG, "→ Discovering services...");
    
    std::vector<NimBLERemoteService*> services = activeClient->getServices(true);
    
    if (services.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        closeActiveConnection();
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Found %d services", services.size());
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // CONNECTION BLEIBT AKTIV
    // ========================================================================
    
    activeClientTimestamp = millis();
    
    pairedDevice.address = address;
    pairedDevice.name = deviceName;
    pairedDevice.bindkey = "";
    
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

    // ✅ NEU: Auto-Start Continuous Scan
    ESP_LOGI(TAG, "→ Starting continuous scan to monitor broadcasts...");
    ESP_LOGI(TAG, "");

    // Wichtig: Connection NICHT mehr benötigt für Broadcasts
    closeActiveConnection();

    // Start scan
    startContinuousScan();

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
        pClient->setConnectTimeout(25);
        
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
    
    NimBLEUUID passkeyUUID(GATT_UUID_PASSKEY);
    NimBLERemoteCharacteristic* pPasskeyChar = nullptr;
    
    for (auto* pService : services) {
        pPasskeyChar = pService->getCharacteristic(passkeyUUID);
        if (pPasskeyChar) {
            ESP_LOGI(TAG, "✓ Passkey characteristic found");
            break;
        }
    }
    
    if (!pPasskeyChar) {
        ESP_LOGE(TAG, "✗ Passkey characteristic not found!");
        if (needNewConnection) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        return false;
    }
    
    uint8_t passkeyBytes[4];
    passkeyBytes[0] = (passkey) & 0xFF;
    passkeyBytes[1] = (passkey >> 8) & 0xFF;
    passkeyBytes[2] = (passkey >> 16) & 0xFF;
    passkeyBytes[3] = (passkey >> 24) & 0xFF;
    
    ESP_LOGI(TAG, "→ Writing Passkey: %u", passkey);
    ESP_LOGI(TAG, "  Bytes: 0x%02X 0x%02X 0x%02X 0x%02X", 
             passkeyBytes[0], passkeyBytes[1], passkeyBytes[2], passkeyBytes[3]);
    ESP_LOGI(TAG, "");
    
    bool writeSuccess = false;
    
    if (pPasskeyChar->canWrite()) {
        writeSuccess = pPasskeyChar->writeValue(passkeyBytes, 4, true);
        savePasskey(passkey);
    }
    
    if (!writeSuccess && pPasskeyChar->canWriteNoResponse()) {
        writeSuccess = pPasskeyChar->writeValue(passkeyBytes, 4, false);
        if (writeSuccess) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    
    if (!writeSuccess) {
        ESP_LOGE(TAG, "✗ Passkey write failed!");
        if (needNewConnection) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        } else {
            closeActiveConnection();
        }
        return false;
    }
    
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
    
    // ✅ BLOCKING SCAN: true statt false!
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
    int maxAttempts = 5;  // ✅ 5 Versuche!
    
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
        
        pClient->setConnectTimeout(15);  // 15 Sekunden pro Versuch
        
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


// ────────────────────────────────────────────────────────────────────────────
// Quick Encryption Activation (OHNE Button-Hold)
// ────────────────────────────────────────────────────────────────────────────

bool ShellyBLEManager::quickEncryptionActivation(const String& address, uint32_t passkey) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  QUICK ENCRYPTION ACTIVATION");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    
    NimBLEClient* pClient = nullptr;
    bool needsCleanup = false;
    
    // ========================================================================
    // ✅ PRÜFE: Ist Connection noch aktiv?
    // ========================================================================
    
    if (activeClient && activeClient->isConnected()) {
        ESP_LOGI(TAG, "✓ Using ACTIVE connection from Phase 1");
        ESP_LOGI(TAG, "  → NO reconnect needed!");
        ESP_LOGI(TAG, "  → NO button press needed!");
        ESP_LOGI(TAG, "");
        
        pClient = activeClient;
        needsCleanup = false;  // Client wird später verwendet
        
    } else {
        // ========================================================================
        // Fallback: Quick Reconnect (wenn Connection verloren)
        // ========================================================================
        
        ESP_LOGW(TAG, "⚠ Connection was lost - attempting quick reconnect...");
        
        // Scan stoppen
        bool wasScanning = scanning;
        if (wasScanning) {
            stopScan();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            ESP_LOGE(TAG, "✗ Failed to create client");
            if (wasScanning) startScan(30);
            return false;
        }
        
        pClient->setConnectTimeout(5000);  // Nur 5 Sekunden
        
        NimBLEAddress bleAddr(address.c_str(), BLE_ADDR_RANDOM);
        
        if (!pClient->connect(bleAddr, false)) {
            ESP_LOGW(TAG, "✗ Quick reconnect failed");
            NimBLEDevice::deleteClient(pClient);
            if (wasScanning) startScan(30);
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Reconnected successfully");
        ESP_LOGI(TAG, "");
        
        needsCleanup = true;  // Dieser Client muss später gelöscht werden
    }
    
    // ========================================================================
    // Service Discovery (falls nötig)
    // ========================================================================
    
    ESP_LOGI(TAG, "→ Getting services...");
    auto services = pClient->getServices(true);
    
    if (services.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Found %d services", services.size());
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // Passkey Characteristic finden
    // ========================================================================
    
    NimBLERemoteCharacteristic* pPasskeyChar = findCharacteristic(services, GATT_UUID_PASSKEY);
    
    if (!pPasskeyChar) {
        ESP_LOGE(TAG, "✗ Passkey characteristic not found");
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        return false;
    }
    
    // ========================================================================
    // Passkey schreiben
    // ========================================================================
    
    ESP_LOGI(TAG, "→ Writing passkey to activate encryption...");
    ESP_LOGI(TAG, "  Passkey: %06u", passkey);
    
    uint8_t passkeyBytes[4];
    passkeyBytes[0] = (passkey) & 0xFF;
    passkeyBytes[1] = (passkey >> 8) & 0xFF;
    passkeyBytes[2] = (passkey >> 16) & 0xFF;
    passkeyBytes[3] = (passkey >> 24) & 0xFF;
    
    if (!pPasskeyChar->writeValue(passkeyBytes, 4, true)) {
        ESP_LOGE(TAG, "✗ Failed to write passkey");
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Passkey written successfully!");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // Encryption Key Characteristic finden
    // ========================================================================
    
    NimBLERemoteCharacteristic* pEncKeyChar = findCharacteristic(services, GATT_UUID_ENCRYPTION_KEY);
    
    if (!pEncKeyChar || !pEncKeyChar->canRead()) {
        ESP_LOGE(TAG, "✗ Encryption key characteristic not found or not readable");
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        return false;
    }
    
    // ========================================================================
    // ⚠️ KRITISCH: Bindkey SOFORT auslesen (BEVOR Device rebootet!)
    // ========================================================================
    
    ESP_LOGI(TAG, "→ Reading bindkey (FAST - device will reboot soon!)...");
    String bindkey = "";
    
    for (int attempt = 1; attempt <= 5; attempt++) {
        std::string val = pEncKeyChar->readValue();
        
        if (val.length() == 16) {
            for (size_t i = 0; i < val.length(); i++) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", (uint8_t)val[i]);
                bindkey += hex;
            }
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✓ Bindkey retrieved successfully!");
            ESP_LOGI(TAG, "  Bindkey: %s", bindkey.c_str());
            ESP_LOGI(TAG, "");
            break;
        }
        
        if (attempt < 5) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    
    if (bindkey.length() != 32) {
        ESP_LOGE(TAG, "✗ Failed to read bindkey (length: %d)", bindkey.length());
        if (needsCleanup) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
        }
        return false;
    }
    
    // ========================================================================
    // Bindkey SOFORT speichern
    // ========================================================================
    
    pairedDevice.bindkey = bindkey;
    savePairedDevice();
    
    ESP_LOGI(TAG, "✓ Bindkey saved to NVS");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Device will now restart to apply encryption...");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // Monitor Disconnect (Device Restart)
    // ========================================================================
    
    ESP_LOGI(TAG, "→ Monitoring connection for restart...");
    
    for (int i = 0; i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        
        if (!pClient->isConnected()) {
            ESP_LOGI(TAG, "✓ Device disconnected (restart detected after %.1fs)", i * 0.1);
            break;
        }
    }
    
    // ========================================================================
    // Cleanup
    // ========================================================================
    
    if (needsCleanup) {
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
    } else {
        // Active Connection cleanup
        if (activeClient) {
            activeClient->disconnect();
            delete activeClientCallbacks;
            NimBLEDevice::deleteClient(activeClient);
            activeClient = nullptr;
            activeClientCallbacks = nullptr;
        }
    }
    
    ESP_LOGI(TAG, "⏳ Waiting for device to fully restart (8 seconds)...");
    
    for (int i = 8; i > 0; i--) {
        ESP_LOGI(TAG, "  %d seconds...", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // ========================================================================
    // Warten auf verschlüsselte Broadcasts
    // ========================================================================
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   WAITING FOR ENCRYPTED BROADCASTS║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "⏳ Waiting for encrypted BTHome broadcasts...");
    ESP_LOGI(TAG, "  Timeout: 30 seconds");
    ESP_LOGI(TAG, "");
    
    discoveredDevices.clear();
    
    ESP_LOGI(TAG, "→ Starting scan...");
    scanning = true;
    pBLEScan->start(0, false);  // Infinite scan
    
    bool deviceFoundEncrypted = false;
    uint32_t scanStartTime = millis();
    uint32_t maxWaitTime = 30000;  // 30 Sekunden
    uint32_t lastLogTime = 0;
    
    while (!deviceFoundEncrypted && (millis() - scanStartTime) < maxWaitTime) {
        vTaskDelay(pdMS_TO_TICKS(500));
        
        uint32_t elapsedSeconds = (millis() - scanStartTime) / 1000;
        
        for (const auto& dev : discoveredDevices) {
            if (dev.address.equalsIgnoreCase(address)) {
                if (dev.isEncrypted) {
                    deviceFoundEncrypted = true;
                    
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "✓ Device found with ENCRYPTED broadcasts!");
                    ESP_LOGI(TAG, "  Wait time: %u seconds", elapsedSeconds);
                    ESP_LOGI(TAG, "");
                    break;
                } else {
                    if (elapsedSeconds > lastLogTime && elapsedSeconds % 3 == 0) {
                        ESP_LOGI(TAG, "  ⏳ %u seconds: Device found, waiting for encryption...", 
                                elapsedSeconds);
                        lastLogTime = elapsedSeconds;
                    }
                }
            }
        }
        
        if (elapsedSeconds > 0 && elapsedSeconds % 5 == 0 && elapsedSeconds != lastLogTime) {
            if (!deviceFoundEncrypted) {
                ESP_LOGI(TAG, "  ⏳ Still waiting... (%u/%u seconds)", 
                        elapsedSeconds, maxWaitTime / 1000);
                lastLogTime = elapsedSeconds;
            }
        }
    }
    
    pBLEScan->stop();
    scanning = false;
    
    if (!deviceFoundEncrypted) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ Device did not send encrypted broadcasts");
        ESP_LOGE(TAG, "  Possible reasons:");
        ESP_LOGE(TAG, "  • Wrong passkey");
        ESP_LOGE(TAG, "  • Device rejected passkey");
        ESP_LOGE(TAG, "");
        return false;
    }
    
    updateDeviceState(STATE_CONNECTED_ENCRYPTED);
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ✅ ENCRYPTION ACTIVATED!         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device: %s (%s)", pairedDevice.name.c_str(), address.c_str());
    ESP_LOGI(TAG, "Passkey: %06u", passkey);
    ESP_LOGI(TAG, "Bindkey: %s", bindkey.c_str());
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ Quick activation successful!");
    ESP_LOGI(TAG, "  Total time: ~15 seconds");
    ESP_LOGI(TAG, "");
    
    return true;
}


// ────────────────────────────────────────────────────────────────────────────
// Full Encryption Setup (MIT Button-Hold)
// ────────────────────────────────────────────────────────────────────────────

bool ShellyBLEManager::fullEncryptionSetup(const String& address, uint32_t passkey) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  FULL ENCRYPTION SETUP");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "");
    ESP_LOGW(TAG, "⚠️  BUTTON PRESS REQUIRED!");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Instructions:");
    ESP_LOGI(TAG, "1. Press button just once (< 1 second)");
    ESP_LOGI(TAG, "");
    
    // Scan stoppen
    bool wasScanning = scanning;
    if (wasScanning) {
        stopScan();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // GATT Connection
    ESP_LOGI(TAG, "→ Creating client...");
    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (!pClient) {
        ESP_LOGE(TAG, "✗ Failed to create client");
        if (wasScanning) startScan(30);
        return false;
    }
    
    pClient->setConnectTimeout(30000);
    
    std::string stdAddress = address.c_str();
    bool connected = false;
    int maxAttempts = 3;
    
    for (int attempt = 1; attempt <= maxAttempts && !connected; attempt++) {
        ESP_LOGI(TAG, "→ Connection attempt %d/%d...", attempt, maxAttempts);
        
        NimBLEAddress bleAddrRnd(stdAddress, BLE_ADDR_RANDOM);
        connected = pClient->connect(bleAddrRnd, false);
        
        if (!connected) {
            ESP_LOGW(TAG, "  RANDOM failed, trying PUBLIC...");
            NimBLEAddress bleAddrPub(stdAddress, BLE_ADDR_PUBLIC);
            connected = pClient->connect(bleAddrPub, false);
        }
        
        if (!connected && attempt < maxAttempts) {
            ESP_LOGW(TAG, "  ✗ Attempt %d failed", attempt);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    
    if (!connected) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ CONNECTION FAILED after %d attempts", maxAttempts);
        ESP_LOGE(TAG, "");
        
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ Connection established!");
    ESP_LOGI(TAG, "✓ You can release the button now");
    ESP_LOGI(TAG, "");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Service Discovery
    ESP_LOGI(TAG, "→ Discovering services...");
    auto services = pClient->getServices(true);
    
    if (services.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Found %d services", services.size());
    ESP_LOGI(TAG, "");
    
    // Passkey Characteristic finden
    NimBLERemoteCharacteristic* pPasskeyChar = findCharacteristic(services, GATT_UUID_PASSKEY);
    
    if (!pPasskeyChar) {
        ESP_LOGE(TAG, "✗ Passkey characteristic not found!");
        ESP_LOGE(TAG, "  Device is NOT in pairing mode");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    // Passkey schreiben
    ESP_LOGI(TAG, "→ Writing passkey...");
    uint8_t passkeyBytes[4];
    passkeyBytes[0] = (passkey) & 0xFF;
    passkeyBytes[1] = (passkey >> 8) & 0xFF;
    passkeyBytes[2] = (passkey >> 16) & 0xFF;
    passkeyBytes[3] = (passkey >> 24) & 0xFF;
    
    if (!pPasskeyChar->writeValue(passkeyBytes, 4, true)) {
        ESP_LOGE(TAG, "✗ Failed to write passkey");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Passkey written!");
    ESP_LOGI(TAG, "");
    
    // Encryption Key lesen
    NimBLERemoteCharacteristic* pEncKeyChar = findCharacteristic(services, GATT_UUID_ENCRYPTION_KEY);
    
    if (!pEncKeyChar || !pEncKeyChar->canRead()) {
        ESP_LOGE(TAG, "✗ Encryption key characteristic not found or not readable");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "→ Reading bindkey (FAST - device may reboot!)...");
    String bindkey = "";
    
    for (int attempt = 1; attempt <= 5; attempt++) {
        std::string val = pEncKeyChar->readValue();
        
        if (val.length() == 16) {
            for (size_t i = 0; i < val.length(); i++) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", (uint8_t)val[i]);
                bindkey += hex;
            }
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✓ Bindkey retrieved!");
            ESP_LOGI(TAG, "  Bindkey: %s", bindkey.c_str());
            ESP_LOGI(TAG, "");
            break;
        }
        
        if (attempt < 5) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    
    if (bindkey.length() != 32) {
        ESP_LOGE(TAG, "✗ Failed to read bindkey");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    // Speichern
    pairedDevice.bindkey = bindkey;
    savePairedDevice();
    
    ESP_LOGI(TAG, "✓ Bindkey saved to NVS");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Device will now restart...");
    
    // Monitor disconnect
    for (int i = 0; i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!pClient->isConnected()) {
            ESP_LOGI(TAG, "✓ Device disconnected (restart detected after %.1fs)", i * 0.1);
            break;
        }
    }
    
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    
    ESP_LOGI(TAG, "⏳ Waiting for device to fully restart (8 seconds)...");
    for (int i = 8; i > 0; i--) {
        ESP_LOGI(TAG, "  %d seconds...", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Warten auf verschlüsselte Broadcasts
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   WAITING FOR ENCRYPTED BROADCASTS║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    discoveredDevices.clear();
    
    ESP_LOGI(TAG, "→ Starting scan...");
    scanning = true;
    pBLEScan->start(0, false);
    
    bool deviceFoundEncrypted = false;
    uint32_t scanStartTime = millis();
    uint32_t maxWaitTime = 30000;
    uint32_t lastLogTime = 0;
    
    while (!deviceFoundEncrypted && (millis() - scanStartTime) < maxWaitTime) {
        vTaskDelay(pdMS_TO_TICKS(500));
        
        uint32_t elapsedSeconds = (millis() - scanStartTime) / 1000;
        
        for (const auto& dev : discoveredDevices) {
            if (dev.address.equalsIgnoreCase(address)) {
                if (dev.isEncrypted) {
                    deviceFoundEncrypted = true;
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "✓ Device found with ENCRYPTED broadcasts!");
                    ESP_LOGI(TAG, "  Wait time: %u seconds", elapsedSeconds);
                    ESP_LOGI(TAG, "");
                    break;
                }
            }
        }
        
        if (elapsedSeconds > 0 && elapsedSeconds % 5 == 0 && elapsedSeconds != lastLogTime) {
            if (!deviceFoundEncrypted) {
                ESP_LOGI(TAG, "  ⏳ Still waiting... (%u/%u seconds)", 
                        elapsedSeconds, maxWaitTime / 1000);
                lastLogTime = elapsedSeconds;
            }
        }
    }
    
    pBLEScan->stop();
    scanning = false;
    
    if (!deviceFoundEncrypted) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ Device did not send encrypted broadcasts");
        ESP_LOGE(TAG, "");
        
        if (wasScanning) startScan(30);
        return false;
    }
    
    updateDeviceState(STATE_CONNECTED_ENCRYPTED);
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ✅ ENCRYPTION ACTIVATED!         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device: %s (%s)", pairedDevice.name.c_str(), address.c_str());
    ESP_LOGI(TAG, "Passkey: %06u", passkey);
    ESP_LOGI(TAG, "Bindkey: %s", bindkey.c_str());
    ESP_LOGI(TAG, "");
    ESP_LOGW(TAG, "⚠️  SAVE THESE CREDENTIALS!");
    ESP_LOGW(TAG, "");
    
    if (wasScanning) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        startScan(30);
    }
    
    return true;
}

// ============================================================================
// Helper: Characteristic finden
// ============================================================================

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

// ============================================================================
// Device State
// ============================================================================

ShellyBLEManager::DeviceState ShellyBLEManager::getDeviceState() const {
    if (!isPaired()) {
        return STATE_NOT_PAIRED;
    }
    
    if (pairedDevice.bindkey.length() > 0) {
        return STATE_CONNECTED_ENCRYPTED;
    }
    
    return STATE_CONNECTED_UNENCRYPTED;
}

// ============================================================================
// Sensor Data Access
// ============================================================================

bool ShellyBLEManager::getSensorData(ShellyBLESensorData& data) const {
    if (!isPaired() || !pairedDevice.sensorData.dataValid) {
        return false;
    }
    
    data = pairedDevice.sensorData;
    return true;
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
// Scan Callback Implementation
// ============================================================================

void ShellyBLEManager::ScanCallback::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    mgr->onAdvertisedDevice(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
}

void ShellyBLEManager::ScanCallback::onScanEnd(const NimBLEScanResults& results, int reason) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "SCAN ENDED (Callback)");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Reason: %d", reason);
    ESP_LOGI(TAG, "Results: %d devices in scan results", results.getCount());
    ESP_LOGI(TAG, "Manager discovered: %d devices", mgr->discoveredDevices.size());
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    mgr->onScanComplete(reason);
}

void ShellyBLEManager::onScanComplete(int reason) {
    scanning = false;
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "SCAN COMPLETE");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Reason: %d", reason);
    ESP_LOGI(TAG, "Scan type: %s", continuousScan ? "CONTINUOUS" : "DISCOVERY");
    ESP_LOGI(TAG, "Found devices: %d", discoveredDevices.size());
    
    if (discoveredDevices.size() > 0) {
        ESP_LOGI(TAG, "Discovered devices:");
        for (size_t i = 0; i < discoveredDevices.size(); i++) {
            const auto& dev = discoveredDevices[i];
            ESP_LOGI(TAG, "  [%d] %s", i+1, dev.name.c_str());
            ESP_LOGI(TAG, "      MAC: %s | RSSI: %d dBm | Encrypted: %s",
                     dev.address.c_str(), dev.rssi, dev.isEncrypted ? "Yes" : "No");
        }
    }
    
    // Auto-Restart für Continuous Scan
    if (continuousScan) {
        if (isPaired()) {
            ESP_LOGI(TAG, "→ Restarting continuous scan in 2 seconds...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            startScan(30);
        } else {
            ESP_LOGW(TAG, "⚠ Device was unpaired during scan - stopping continuous scan");
            continuousScan = false;
        }
    } else {
        ESP_LOGI(TAG, "Discovery scan complete - not restarting");
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
}

void ShellyBLEManager::onAdvertisedDevice(NimBLEAdvertisedDevice* advertisedDevice) {
    String name = advertisedDevice->getName().c_str();
    String address = advertisedDevice->getAddress().toString().c_str();
    
    // Filter: Nur Shelly BLU Door/Window Sensoren
    if (!name.startsWith("SBDW-")) {
        return;
    }
    
    ESP_LOGI(TAG, "┌─────────────────────────────────");
    ESP_LOGI(TAG, "│ 🔍 Shelly BLU Door/Window found");
    ESP_LOGI(TAG, "├─────────────────────────────────");
    ESP_LOGI(TAG, "│ Name: %s", name.c_str());
    ESP_LOGI(TAG, "│ MAC:  %s", address.c_str());
    
    int8_t rssi = advertisedDevice->getRSSI();
    bool isEncrypted = false;
    bool hasServiceData = false;
    
    ESP_LOGI(TAG, "│ RSSI: %d dBm", rssi);

    ESP_LOGI(TAG, "│");
    ESP_LOGI(TAG, "│ ═══════════════════════════════════════");
    ESP_LOGI(TAG, "│ FULL BLE ADVERTISEMENT ANALYSIS");
    ESP_LOGI(TAG, "│ ═══════════════════════════════════════");
    ESP_LOGI(TAG, "│");

    // 1. Service UUIDs
    ESP_LOGI(TAG, "│ 1. Service UUIDs:");
    if (advertisedDevice->haveServiceUUID()) {
        ESP_LOGI(TAG, "│    ✓ Has Service UUIDs");
        
        NimBLEUUID serviceUUID = advertisedDevice->getServiceUUID();
        String uuidStr = serviceUUID.toString().c_str();
        ESP_LOGI(TAG, "│    UUID: %s", uuidStr.c_str());
        
        // Check if BTHome UUID
        if (serviceUUID == NimBLEUUID(BTHOME_SERVICE_UUID)) {
            ESP_LOGI(TAG, "│    ✓ THIS IS BTHOME UUID!");
        } else {
            ESP_LOGW(TAG, "│    ⚠ This is NOT BTHome UUID");
            ESP_LOGW(TAG, "│      Expected: fcd2");
            ESP_LOGW(TAG, "│      Got:      %s", uuidStr.c_str());
        }
    } else {
        ESP_LOGE(TAG, "│    ✗ No Service UUID advertised");
    }

    ESP_LOGI(TAG, "│");

    // 2. BTHome Service Check
    ESP_LOGI(TAG, "│ 2. BTHome Service (UUID: 0xFCD2):");
    if (advertisedDevice->isAdvertisingService(NimBLEUUID(BTHOME_SERVICE_UUID))) {
        ESP_LOGI(TAG, "│    ✓ Device advertises BTHome service");
        
        // Try to get service data
        std::string serviceData = advertisedDevice->getServiceData(NimBLEUUID(BTHOME_SERVICE_UUID));
        
        ESP_LOGI(TAG, "│    Service Data Length: %d bytes", serviceData.length());
        
        if (serviceData.length() > 0) {
            ESP_LOGI(TAG, "│    ✓ Service Data AVAILABLE!");
            
            // Hex dump
            char hex_buf[256];
            int offset = 0;
            offset += snprintf(hex_buf, sizeof(hex_buf), "│    Hex: ");
            for (size_t i = 0; i < min(serviceData.length(), (size_t)32); i++) {
                offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                                "%02X ", (uint8_t)serviceData[i]);
            }
            ESP_LOGI(TAG, "%s", hex_buf);
            
            if (serviceData.length() > 0) {
                uint8_t deviceInfo = (uint8_t)serviceData[0];
                ESP_LOGI(TAG, "│    Device Info byte: 0x%02X", deviceInfo);
                ESP_LOGI(TAG, "│    Encrypted: %s", (deviceInfo & 0x01) ? "YES" : "NO");
                ESP_LOGI(TAG, "│    BTHome Version: %d", (deviceInfo >> 5) & 0x07);
            }
        } else {
            ESP_LOGE(TAG, "│    ✗ Service Data is EMPTY!");
            ESP_LOGE(TAG, "│       → Device advertises service but sends no data");
            ESP_LOGE(TAG, "│       → This means: NO EVENTS or BEACON MODE OFF");
        }
    } else {
        ESP_LOGE(TAG, "│    ✗ Device does NOT advertise BTHome service");
    }

    ESP_LOGI(TAG, "│");

    // 3. Manufacturer Data
    ESP_LOGI(TAG, "│ 3. Manufacturer Data:");
    if (advertisedDevice->haveManufacturerData()) {
        ESP_LOGI(TAG, "│    ✓ Has Manufacturer Data");
        
        std::string mfgData = advertisedDevice->getManufacturerData();
        ESP_LOGI(TAG, "│    Length: %d bytes", mfgData.length());
        
        if (mfgData.length() >= 2) {
            uint16_t companyId = (uint8_t)mfgData[0] | ((uint8_t)mfgData[1] << 8);
            ESP_LOGI(TAG, "│    Company ID: 0x%04X", companyId);
            
            // Hex dump
            if (mfgData.length() > 2) {
                char hex_buf[256];
                int offset = 0;
                offset += snprintf(hex_buf, sizeof(hex_buf), "│    Data: ");
                for (size_t i = 2; i < min(mfgData.length(), (size_t)32); i++) {
                    offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                                    "%02X ", (uint8_t)mfgData[i]);
                }
                ESP_LOGI(TAG, "%s", hex_buf);
            }
        }
    } else {
        ESP_LOGI(TAG, "│    ✗ No Manufacturer Data");
    }

    ESP_LOGI(TAG, "│");

    // 4. Service Data (check all known UUIDs)
    ESP_LOGI(TAG, "│ 4. Service Data Check (common UUIDs):");

    std::vector<const char*> knownUUIDs = {
        "0000fcd2-0000-1000-8000-00805f9b34fb",  // BTHome
        "0000181a-0000-1000-8000-00805f9b34fb",  // Environmental Sensing
        "0000180f-0000-1000-8000-00805f9b34fb",  // Battery Service
    };

    bool foundAnyServiceData = false;

    for (auto& uuidStr : knownUUIDs) {
        std::string sd = advertisedDevice->getServiceData(NimBLEUUID(uuidStr));
        if (sd.length() > 0) {
            foundAnyServiceData = true;
            ESP_LOGI(TAG, "│    ✓ Found for UUID %s:", uuidStr);
            ESP_LOGI(TAG, "│      Length: %d bytes", sd.length());
            
            char hex_buf[128];
            int offset = 0;
            offset += snprintf(hex_buf, sizeof(hex_buf), "│      Data: ");
            for (size_t i = 0; i < min(sd.length(), (size_t)16); i++) {
                offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                                "%02X ", (uint8_t)sd[i]);
            }
            ESP_LOGI(TAG, "%s", hex_buf);
        }
    }

    if (!foundAnyServiceData) {
        ESP_LOGW(TAG, "│    ⚠ No Service Data for any known UUID!");
    }

    ESP_LOGI(TAG, "│");
    ESP_LOGI(TAG, "│ ═══════════════════════════════════════");
    ESP_LOGI(TAG, "│");
    
    // Check for BTHome Service Data
    if (advertisedDevice->haveServiceUUID()) {
        if (advertisedDevice->isAdvertisingService(NimBLEUUID(BTHOME_SERVICE_UUID))) {
            ESP_LOGI(TAG, "│ ✓ BTHome service found");
            
            std::string serviceData = advertisedDevice->getServiceData(NimBLEUUID(BTHOME_SERVICE_UUID));
            if (serviceData.length() > 0) {
                hasServiceData = true;
                uint8_t firstByte = serviceData[0];
                isEncrypted = (firstByte & 0x01) != 0;
                
                ESP_LOGI(TAG, "│ Service data: %d bytes", serviceData.length());
                ESP_LOGI(TAG, "│ Encrypted: %s", isEncrypted ? "YES 🔒" : "NO");
            }
        }
    }
    
    // Add/Update in Discovery List
    bool found = false;
    for (auto& dev : discoveredDevices) {
        if (dev.address == address) {
            dev.rssi = rssi;
            dev.lastSeen = millis();
            dev.addressType = advertisedDevice->getAddress().getType();
            if (hasServiceData) {
                dev.isEncrypted = isEncrypted;
            }
            found = true;
            ESP_LOGI(TAG, "│ Updated existing entry");
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
        device.addressType = advertisedDevice->getAddress().getType();
        
        discoveredDevices.push_back(device);
        
        ESP_LOGI(TAG, "│ ✓ Added to discovered devices");
        ESP_LOGI(TAG, "│   Total SBDW devices: %d", discoveredDevices.size());
    }
    
    ESP_LOGI(TAG, "└─────────────────────────────────");
    
    // ✅ ✅ ✅ KRITISCHES DEBUG LOGGING HIER EINFÜGEN! ✅ ✅ ✅
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PAIRING CHECK FOR CALLBACK      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Conditions for paired device update:");
    ESP_LOGI(TAG, "  1. isPaired():         %s", isPaired() ? "TRUE ✓" : "FALSE ✗");
    ESP_LOGI(TAG, "  2. hasServiceData:     %s", hasServiceData ? "TRUE ✓" : "FALSE ✗");
    ESP_LOGI(TAG, "");
    
    if (isPaired()) {
        ESP_LOGI(TAG, "Paired Device Info:");
        ESP_LOGI(TAG, "  Paired Address:    '%s'", pairedDevice.address.c_str());
        ESP_LOGI(TAG, "  Current Address:   '%s'", address.c_str());
        ESP_LOGI(TAG, "  Addresses Match:   %s", 
                 pairedDevice.address == address ? "TRUE ✓" : "FALSE ✗");
        ESP_LOGI(TAG, "");
        
        // String Comparison Debug
        ESP_LOGI(TAG, "String Comparison Details:");
        ESP_LOGI(TAG, "  Paired length:     %d", pairedDevice.address.length());
        ESP_LOGI(TAG, "  Current length:    %d", address.length());
        
        // Case-insensitive comparison
        bool matchIgnoreCase = pairedDevice.address.equalsIgnoreCase(address);
        ESP_LOGI(TAG, "  Case-Insensitive:  %s", matchIgnoreCase ? "MATCH ✓" : "NO MATCH ✗");
        
        // Hex dump der Adressen
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "  Paired Address bytes:");
        for (size_t i = 0; i < pairedDevice.address.length() && i < 20; i++) {
            ESP_LOGI(TAG, "    [%d] = 0x%02X ('%c')", 
                     i, pairedDevice.address[i], pairedDevice.address[i]);
        }
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "  Current Address bytes:");
        for (size_t i = 0; i < address.length() && i < 20; i++) {
            ESP_LOGI(TAG, "    [%d] = 0x%02X ('%c')", 
                     i, address[i], address[i]);
        }
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Final Decision:");
    
    bool willUpdate = isPaired() && pairedDevice.address == address && hasServiceData;
    
    ESP_LOGI(TAG, "  Will update paired device data: %s", 
             willUpdate ? "YES ✓✓✓" : "NO ✗✗✗");
    ESP_LOGI(TAG, "");
    
    if (!willUpdate) {
        ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGE(TAG, "║   CALLBACK WILL NOT TRIGGER!      ║");
        ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
        
        if (!isPaired()) {
            ESP_LOGE(TAG, "  Reason: Device is NOT paired");
        }
        if (!hasServiceData) {
            ESP_LOGE(TAG, "  Reason: No service data found");
        }
        if (isPaired() && pairedDevice.address != address) {
            ESP_LOGE(TAG, "  Reason: Address mismatch!");
            ESP_LOGE(TAG, "    Expected: '%s'", pairedDevice.address.c_str());
            ESP_LOGE(TAG, "    Got:      '%s'", address.c_str());
        }
    }
    
    ESP_LOGI(TAG, "");
    
    // Update Paired Device Data
    if (isPaired() && pairedDevice.address == address && hasServiceData) {
        ESP_LOGI(TAG, "┌─────────────────────────────────");
        ESP_LOGI(TAG, "│ PAIRED DEVICE DATA UPDATE");
        ESP_LOGI(TAG, "├─────────────────────────────────");
        
        std::string serviceData = advertisedDevice->getServiceData(NimBLEUUID(BTHOME_SERVICE_UUID));
        
        // ✅ NEU: Umfassendes Debug-Logging
        ESP_LOGI(TAG, "│");
        ESP_LOGI(TAG, "│ 📊 Service Data Analysis:");
        ESP_LOGI(TAG, "│   Length: %d bytes", serviceData.length());
        
        if (serviceData.length() > 0) {
            char hex_buf[128];
            int offset = 0;
            offset += snprintf(hex_buf, sizeof(hex_buf), "│   Hex: ");
            for (size_t i = 0; i < std::min(serviceData.length(), (size_t)20); i++) {
                offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                                "%02X ", (uint8_t)serviceData[i]);
            }
            ESP_LOGI(TAG, "%s", hex_buf);
            
            uint8_t deviceInfo = (uint8_t)serviceData[0];
            ESP_LOGI(TAG, "│   Device Info: 0x%02X", deviceInfo);
            ESP_LOGI(TAG, "│   Encrypted: %s", (deviceInfo & 0x01) ? "YES" : "NO");
        }
        
        ESP_LOGI(TAG, "│");
        ESP_LOGI(TAG, "│ 🔑 Paired Device Info:");
        ESP_LOGI(TAG, "│   Address: %s", pairedDevice.address.c_str());
        ESP_LOGI(TAG, "│   Name: %s", pairedDevice.name.c_str());
        ESP_LOGI(TAG, "│   Bindkey: %s (%d chars)", 
                pairedDevice.bindkey.length() > 0 ? "PRESENT" : "EMPTY",
                pairedDevice.bindkey.length());
        
        ESP_LOGI(TAG, "│");
        ESP_LOGI(TAG, "│ → Calling parseBTHomePacket()...");
        ESP_LOGI(TAG, "│");
        
        ShellyBLESensorData newData;
        newData.rssi = rssi;
        
        bool parseSuccess = parseBTHomePacket((uint8_t*)serviceData.data(), serviceData.length(),
                                            pairedDevice.bindkey, address, newData);
        
        ESP_LOGI(TAG, "│");
        ESP_LOGI(TAG, "│ ← parseBTHomePacket() returned: %s", 
                parseSuccess ? "TRUE ✓" : "FALSE ✗");
        ESP_LOGI(TAG, "│");
        
        if (parseSuccess) {
            // ✅ Existing data changed check code
            bool dataChanged = (newData.windowOpen != pairedDevice.sensorData.windowOpen) ||
                            (newData.battery != pairedDevice.sensorData.battery) ||
                            (abs((int)newData.illuminance - (int)pairedDevice.sensorData.illuminance) > 10) ||
                            (newData.rotation != pairedDevice.sensorData.rotation) ||
                            (newData.hasButtonEvent);
            
            if (dataChanged) {
                ESP_LOGI(TAG, "│ ✓ Data changed:");
                
                if (newData.packetId != pairedDevice.sensorData.packetId) {
                    ESP_LOGI(TAG, "│   Packet ID: %d → %d",
                            pairedDevice.sensorData.packetId, newData.packetId);
                }
                
                if (newData.windowOpen != pairedDevice.sensorData.windowOpen) {
                    ESP_LOGI(TAG, "│   Contact: %s → %s",
                            pairedDevice.sensorData.windowOpen ? "OPEN" : "CLOSED",
                            newData.windowOpen ? "OPEN" : "CLOSED");
                }
                
                if (newData.battery != pairedDevice.sensorData.battery) {
                    ESP_LOGI(TAG, "│   Battery: %d%% → %d%%",
                            pairedDevice.sensorData.battery, newData.battery);
                }
                
                if (abs((int)newData.illuminance - (int)pairedDevice.sensorData.illuminance) > 10) {
                    ESP_LOGI(TAG, "│   Illuminance: %d lux → %d lux",
                            pairedDevice.sensorData.illuminance, newData.illuminance);
                }
                
                if (newData.rotation != pairedDevice.sensorData.rotation) {
                    ESP_LOGI(TAG, "│   Rotation: %d° → %d°",
                            pairedDevice.sensorData.rotation, newData.rotation);
                }
                
                if (newData.hasButtonEvent) {
                    const char* eventName;
                    switch (newData.buttonEvent) {
                        case BUTTON_SINGLE_PRESS: eventName = "SINGLE PRESS 👆"; break;
                        case BUTTON_HOLD: eventName = "HOLD ⏸️"; break;
                        default: eventName = "UNKNOWN";
                    }
                    ESP_LOGI(TAG, "│   Button: %s", eventName);
                }
            } else {
                ESP_LOGI(TAG, "│ ℹ No data changes detected");
            }
            
            pairedDevice.sensorData = newData;
            pairedDevice.sensorData.lastUpdate = millis();
            pairedDevice.sensorData.dataValid = true;
            
            // ✅ Callback triggern
            if (dataChanged && sensorDataCallback) {
                ESP_LOGI(TAG, "│");
                ESP_LOGI(TAG, "│ 🔔 Triggering sensorDataCallback...");
                ESP_LOGI(TAG, "│");
                sensorDataCallback(address, newData);
                ESP_LOGI(TAG, "│ ✓ Callback executed");
            } else if (!dataChanged) {
                ESP_LOGI(TAG, "│ ⏭ Skipping callback (no changes)");
            } else if (!sensorDataCallback) {
                ESP_LOGE(TAG, "│ ✗ sensorDataCallback is NULL!");
            }
            
        } else {
            ESP_LOGW(TAG, "│ ✗ Failed to parse BTHome packet!");
            ESP_LOGW(TAG, "│   This means no sensor data was extracted");
            ESP_LOGW(TAG, "│   Check the detailed parse log above");
        }
        
        ESP_LOGI(TAG, "└─────────────────────────────────");
    }
    // ✅✅✅ NEU: FALLBACK - Parse Manufacturer Data wenn keine Service Data! ✅✅✅
    if (isPaired() && pairedDevice.address == address && !hasServiceData) {
        ESP_LOGI(TAG, "┌─────────────────────────────────");
        ESP_LOGI(TAG, "│ FALLBACK: MANUFACTURER DATA PARSE");
        ESP_LOGI(TAG, "├─────────────────────────────────");
        
        // ✅ KRITISCH: mfgData HIER deklarieren (AUSSERHALB des if-Blocks!)
        if (!advertisedDevice->haveManufacturerData()) {
            ESP_LOGW(TAG, "│ ✗ No Manufacturer Data available");
            ESP_LOGI(TAG, "└─────────────────────────────────");
            ESP_LOGI(TAG, "");
            return;  // Exit early
        }
        
        // ✅ Variable ist jetzt im GESAMTEN BLOCK verfügbar!
        std::string mfgData = advertisedDevice->getManufacturerData();
        
        if (mfgData.length() < 2) {
            ESP_LOGW(TAG, "│ ✗ Manufacturer Data too short: %d bytes", mfgData.length());
            ESP_LOGI(TAG, "└─────────────────────────────────");
            ESP_LOGI(TAG, "");
            return;
        }
        
        uint16_t companyId = (uint8_t)mfgData[0] | ((uint8_t)mfgData[1] << 8);
        
        ESP_LOGI(TAG, "│ Company ID: 0x%04X", companyId);
        
        // Check if Shelly (Allterco Robotics)
        if (companyId != 0x0BA9) {
            ESP_LOGW(TAG, "│ ⚠ Not Shelly Company ID: 0x%04X", companyId);
            ESP_LOGI(TAG, "└─────────────────────────────────");
            ESP_LOGI(TAG, "");
            return;
        }
        
        ESP_LOGI(TAG, "│ ✓ This is Shelly (Allterco Robotics)!");
        ESP_LOGI(TAG, "│");
        ESP_LOGI(TAG, "│ Manufacturer Data Length: %d bytes", mfgData.length());
        
        // Hex dump full data
        char hex_buf[256];
        int offset = 0;
        offset += snprintf(hex_buf, sizeof(hex_buf), "│ Raw Data: ");
        for (size_t i = 0; i < std::min(mfgData.length(), (size_t)32); i++) {
            offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                            "%02X ", (uint8_t)mfgData[i]);
        }
        ESP_LOGI(TAG, "%s", hex_buf);
        
        // Check if last 6 bytes are MAC address
        if (mfgData.length() < 8) {
            ESP_LOGW(TAG, "│ ⚠ Data too short to contain MAC (min 8 bytes needed)");
            ESP_LOGI(TAG, "└─────────────────────────────────");
            ESP_LOGI(TAG, "");
            return;
        }
        
        // Extract MAC from last 6 bytes (reversed)
        char macCheck[18];
        snprintf(macCheck, sizeof(macCheck), "%02x:%02x:%02x:%02x:%02x:%02x",
                (uint8_t)mfgData[mfgData.length()-1],
                (uint8_t)mfgData[mfgData.length()-2],
                (uint8_t)mfgData[mfgData.length()-3],
                (uint8_t)mfgData[mfgData.length()-4],
                (uint8_t)mfgData[mfgData.length()-5],
                (uint8_t)mfgData[mfgData.length()-6]);
        
        ESP_LOGI(TAG, "│ MAC in data: %s", macCheck);
        ESP_LOGI(TAG, "│ Expected:    %s", address.c_str());
        
        if (!String(macCheck).equalsIgnoreCase(address)) {
            ESP_LOGW(TAG, "│ ⚠ MAC in data does NOT match device address!");
            ESP_LOGI(TAG, "└─────────────────────────────────");
            ESP_LOGI(TAG, "");
            return;
        }
        
        ESP_LOGI(TAG, "│ ✓ MAC matches!");
        ESP_LOGI(TAG, "│");
        
        // Extract payload (Company ID + Data - MAC)
        size_t payloadLen = mfgData.length() - 2 - 6;  // - Company ID - MAC
        
        if (payloadLen == 0) {
            ESP_LOGW(TAG, "│ ⚠ No payload data (only Company ID + MAC)");
            ESP_LOGI(TAG, "└─────────────────────────────────");
            ESP_LOGI(TAG, "");
            return;
        }
        
        ESP_LOGI(TAG, "│ Payload Length: %d bytes", payloadLen);
        
        // Hex dump payload only (skip Company ID, exclude MAC)
        offset = 0;
        offset += snprintf(hex_buf, sizeof(hex_buf), "│ Payload: ");
        for (size_t i = 2; i < mfgData.length() - 6; i++) {
            offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                            "%02X ", (uint8_t)mfgData[i]);
        }
        ESP_LOGI(TAG, "%s", hex_buf);
        ESP_LOGI(TAG, "│");
        
        // Analyze payload format
        if (payloadLen >= 1) {
            uint8_t* payload = (uint8_t*)mfgData.data() + 2;  // Skip Company ID
            uint8_t firstByte = payload[0];
            
            ESP_LOGI(TAG, "│ Payload Analysis:");
            ESP_LOGI(TAG, "│   First byte: 0x%02X", firstByte);
            
            // Check if it looks like BTHome format
            if ((firstByte & 0x01) == 0x01) {
                ESP_LOGI(TAG, "│   Bit 0 (Encryption): SET");
                ESP_LOGI(TAG, "│   → This looks like encrypted data");
            } else {
                ESP_LOGI(TAG, "│   Bit 0 (Encryption): NOT SET");
                ESP_LOGI(TAG, "│   → This looks like unencrypted data");
            }
            
            uint8_t version = (firstByte >> 5) & 0x07;
            ESP_LOGI(TAG, "│   Bits 5-7 (Version): %d", version);
            
            ESP_LOGI(TAG, "│");
            ESP_LOGI(TAG, "│ ⚠ PROPRIETARY SHELLY FORMAT DETECTED");
            ESP_LOGI(TAG, "│");
            ESP_LOGI(TAG, "│ This is NOT standard BTHome Service Data!");
            ESP_LOGI(TAG, "│ Shelly is using Manufacturer Data instead.");
            ESP_LOGI(TAG, "│");
            ESP_LOGI(TAG, "│ ╔════════════════════════════════════╗");
            ESP_LOGI(TAG, "│ ║  SOLUTION: ENABLE ENCRYPTION       ║");
            ESP_LOGI(TAG, "│ ╚════════════════════════════════════╝");
            ESP_LOGI(TAG, "│");
            ESP_LOGI(TAG, "│ After enabling encryption via WebUI:");
            ESP_LOGI(TAG, "│   → Device will send BTHome Service Data");
            ESP_LOGI(TAG, "│   → Service UUID: 0xFCD2");
            ESP_LOGI(TAG, "│   → Format: D2 FC 41 <encrypted payload>");
            ESP_LOGI(TAG, "│");
            ESP_LOGI(TAG, "│ Steps:");
            ESP_LOGI(TAG, "│   1. Go to WebUI → BLE Sensor tab");
            ESP_LOGI(TAG, "│   2. Click 'Enable Encryption'");
            ESP_LOGI(TAG, "│   3. Enter passkey (e.g. 123456)");
            ESP_LOGI(TAG, "│   4. Wait for device reboot");
            ESP_LOGI(TAG, "│   5. Sensor data will then work!");
        }
        
        ESP_LOGI(TAG, "└─────────────────────────────────");
        ESP_LOGI(TAG, "");
    }
}

// ============================================================================
// Loop & Cleanup
// ============================================================================

void ShellyBLEManager::loop() {
    if (!initialized) return;
    
    cleanupOldDiscoveries();
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

bool ShellyBLEManager::isContinuousScanActive() const {
    return continuousScan && scanning;
}

String ShellyBLEManager::getScanStatus() const {
    if (!initialized) return "Not initialized";
    if (continuousScan && scanning) return "Continuous scan active";
    if (scanning) return "Discovery scan active";
    if (continuousScan && !scanning) return "Continuous scan (between cycles)";
    return "Idle";
}

// ============================================================================
// BTHome v2 Parser
// ============================================================================

bool ShellyBLEManager::parseBTHomePacket(const uint8_t* data, size_t length,
                                         const String& bindkey, 
                                         const String& macAddress,
                                         ShellyBLESensorData& sensorData) {
    ESP_LOGI(TAG, "───────────────────────────────────");
    ESP_LOGI(TAG, "📦 PARSING BTHOME PACKET");
    ESP_LOGI(TAG, "───────────────────────────────────");
    
    // ✅ KRITISCHES DEBUG LOGGING
    ESP_LOGI(TAG, "Input:");
    ESP_LOGI(TAG, "  Length: %d bytes", length);
    ESP_LOGI(TAG, "  MAC: %s", macAddress.c_str());
    ESP_LOGI(TAG, "  Bindkey: %s (%d chars)", 
             bindkey.length() > 0 ? "PROVIDED" : "EMPTY", 
             bindkey.length());
    
    // Hex Dump
    if (length > 0) {
        char hex_buf[256];
        int offset = 0;
        offset += snprintf(hex_buf, sizeof(hex_buf), "  Raw Data: ");
        for (size_t i = 0; i < min(length, (size_t)32); i++) {
            offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                             "%02X ", data[i]);
        }
        ESP_LOGI(TAG, "%s", hex_buf);
    }
    
    if (length < 2) {
        ESP_LOGW(TAG, "✗ Packet too short: %d bytes (min: 2)", length);
        ESP_LOGI(TAG, "───────────────────────────────────");
        return false;
    }
    
    uint8_t deviceInfo = data[0];
    bool encrypted = (deviceInfo & 0x01) != 0;
    
    ESP_LOGI(TAG, "Packet Info:");
    ESP_LOGI(TAG, "  Device Info: 0x%02X", deviceInfo);
    ESP_LOGI(TAG, "  Encryption: %s", encrypted ? "YES (0x41)" : "NO (0x40)");
    
    uint8_t* payload = nullptr;
    size_t payloadLength = 0;
    uint8_t decryptedBuffer[256];
    
    if (encrypted) {
        ESP_LOGI(TAG, "→ Encrypted packet detected");
        
        if (bindkey.length() != 32) {
            ESP_LOGW(TAG, "✗ No valid bindkey for encrypted packet!");
            ESP_LOGW(TAG, "  Bindkey length: %d (expected: 32)", bindkey.length());
            ESP_LOGI(TAG, "───────────────────────────────────");
            return false;
        }
        
        ESP_LOGI(TAG, "→ Attempting decryption...");
        size_t decryptedLen = 0;
        
        if (!decryptBTHome(data, length, bindkey, macAddress, decryptedBuffer, decryptedLen)) {
            ESP_LOGW(TAG, "✗ Decryption failed!");
            ESP_LOGI(TAG, "───────────────────────────────────");
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Decryption successful (%d bytes)", decryptedLen);
        payload = decryptedBuffer + 1;
        payloadLength = decryptedLen - 1;
        
    } else {
        ESP_LOGI(TAG, "→ Unencrypted packet - reading directly");
        payload = (uint8_t*)data + 1;
        payloadLength = length - 1;
    }
    
    ESP_LOGI(TAG, "Payload:");
    ESP_LOGI(TAG, "  Length: %d bytes", payloadLength);
    
    // Hex Dump Payload
    if (payloadLength > 0) {
        char hex_buf[256];
        int offset = 0;
        offset += snprintf(hex_buf, sizeof(hex_buf), "  Data: ");
        for (size_t i = 0; i < min(payloadLength, (size_t)32); i++) {
            offset += snprintf(hex_buf + offset, sizeof(hex_buf) - offset, 
                             "%02X ", payload[i]);
        }
        ESP_LOGI(TAG, "%s", hex_buf);
    }
    
    // Parse payload objects
    size_t offset = 0;
    bool hasData = false;
    sensorData.hasButtonEvent = false;
    
    ESP_LOGI(TAG, "Parsing objects:");
    
    while (offset < payloadLength) {
        if (offset >= payloadLength) break;
        
        uint8_t objectId = payload[offset++];
        size_t objectLen = getBTHomeObjectLength(objectId);
        
        ESP_LOGI(TAG, "  [%d] Object ID: 0x%02X, Length: %d", offset-1, objectId, objectLen);
        
        if (objectLen == 0) {
            ESP_LOGW(TAG, "  ⚠ Unknown Object ID: 0x%02X (stopping parse)", objectId);
            break;
        }
        
        if (offset + objectLen > payloadLength) {
            ESP_LOGW(TAG, "  ✗ Object 0x%02X: insufficient data (%d/%d bytes)", 
                     objectId, payloadLength - offset, objectLen);
            break;
        }
        
        // Hex dump object data
        char obj_hex[64];
        int obj_offset = 0;
        for (size_t i = 0; i < objectLen && i < 8; i++) {
            obj_offset += snprintf(obj_hex + obj_offset, sizeof(obj_hex) - obj_offset,
                                 "%02X ", payload[offset + i]);
        }
        ESP_LOGI(TAG, "      Data: %s", obj_hex);
        
        switch (objectId) {
            case BTHOME_OBJ_PACKET_ID: {
                sensorData.packetId = payload[offset];
                hasData = true;
                ESP_LOGI(TAG, "      → Packet ID: %d", sensorData.packetId);
                break;
            }
            
            case BTHOME_OBJ_BATTERY: {
                sensorData.battery = payload[offset];
                hasData = true;
                ESP_LOGI(TAG, "      → Battery: %d%%", sensorData.battery);
                break;
            }
            
            case BTHOME_OBJ_ILLUMINANCE: {
                uint32_t lux_raw = payload[offset] | 
                                  (payload[offset+1] << 8) | 
                                  (payload[offset+2] << 16);
                sensorData.illuminance = lux_raw / 100;
                hasData = true;
                ESP_LOGI(TAG, "      → Illuminance: %d lux (raw: %u)", 
                         sensorData.illuminance, lux_raw);
                break;
            }
            
            case BTHOME_OBJ_WINDOW: {
                sensorData.windowOpen = (payload[offset] != 0);
                hasData = true;
                ESP_LOGI(TAG, "      → Window: %s", 
                         sensorData.windowOpen ? "OPEN 🔓" : "CLOSED 🔒");
                break;
            }
            
            case BTHOME_OBJ_BUTTON: {
                uint16_t btnValue = payload[offset] | (payload[offset+1] << 8);
                sensorData.buttonEvent = static_cast<ShellyButtonEvent>(btnValue & 0xFF);
                sensorData.hasButtonEvent = true;
                hasData = true;
                ESP_LOGI(TAG, "      → Button: 0x%04X", btnValue);
                break;
            }
            
            case BTHOME_OBJ_ROTATION: {
                int16_t rot_raw = payload[offset] | (payload[offset+1] << 8);
                sensorData.rotation = rot_raw / 10;
                hasData = true;
                ESP_LOGI(TAG, "      → Rotation: %d° (raw: %d)", 
                                                  sensorData.rotation, rot_raw);
                break;
            }
            
            default:
                ESP_LOGW(TAG, "      ⚠ Unhandled Object ID: 0x%02X", objectId);
                break;
        }
        
        offset += objectLen;
    }
    
    ESP_LOGI(TAG, "───────────────────────────────────");
    ESP_LOGI(TAG, "Parse Result: %s", hasData ? "SUCCESS ✓" : "FAILED ✗");
    ESP_LOGI(TAG, "  Has Data: %s", hasData ? "YES" : "NO");
    ESP_LOGI(TAG, "  Objects parsed: %d", offset > 0 ? "some" : "none");
    ESP_LOGI(TAG, "───────────────────────────────────");
    
    return hasData;
}


size_t ShellyBLEManager::getBTHomeObjectLength(uint8_t objectId) {
    switch (objectId) {
        case 0x00: return 1;  // Packet ID
        case 0x01: return 1;  // Battery
        case 0x05: return 3;  // Illuminance
        case 0x2D: return 1;  // Window
        case 0x3A: return 2;  // Button
        case 0x3F: return 2;  // Rotation
        default: return 0;    // Unknown
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
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG, "AES-CCM Decryption");
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    if (length < 13) {
        ESP_LOGW(TAG, "✗ Encrypted packet too short: %d bytes (min: 13)", length);
        return false;
    }
    
    ESP_LOGI(TAG, "Input length: %d bytes", length);
    ESP_LOGI(TAG, "MAC Address: %s", macAddress.c_str());
    
    const uint8_t* counter = encryptedData + 1;
    const uint8_t* payload = encryptedData + 5;
    size_t payloadLen = length - 9;
    const uint8_t* tag = encryptedData + length - 4;
    
    // Bindkey konvertieren
    if (bindkey.length() != 32) {
        ESP_LOGE(TAG, "✗ Invalid bindkey length: %d", bindkey.length());
        return false;
    }
    
    uint8_t key[16];
    for (int i = 0; i < 16; i++) {
        char hex[3] = {bindkey[i*2], bindkey[i*2+1], 0};
        key[i] = (uint8_t)strtol(hex, NULL, 16);
    }
    ESP_LOGI(TAG, "✓ Bindkey converted to binary (16 bytes)");
    
    // MAC-Adresse parsen
    uint8_t mac[6];
    if (!parseMacAddress(macAddress, mac)) {
        ESP_LOGE(TAG, "✗ Invalid MAC address format: %s", macAddress.c_str());
        return false;
    }
    
    ESP_LOGI(TAG, "MAC parsed: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // Build NONCE (13 bytes)
    uint8_t nonce[13];
    
    // MAC-Adresse (6 bytes) - NICHT reversen!
    memcpy(nonce, mac, 6);
    
    // UUID in Little Endian (0xFCD2 → D2 FC)
    nonce[6] = 0xD2;
    nonce[7] = 0xFC;
    
    // Device Info byte
    nonce[8] = encryptedData[0];
    
    // Counter (4 bytes, already Little Endian)
    memcpy(nonce + 9, counter, 4);
    
    ESP_LOGI(TAG, "Nonce constructed (13 bytes):");
    ESP_LOGI(TAG, "  MAC:     %02X:%02X:%02X:%02X:%02X:%02X", 
             nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5]);
    ESP_LOGI(TAG, "  UUID:    %02X %02X (0xFCD2 = BTHome v2)", nonce[6], nonce[7]);
    ESP_LOGI(TAG, "  DevInfo: %02X", nonce[8]);
    ESP_LOGI(TAG, "  Counter: %02X %02X %02X %02X", 
             nonce[9], nonce[10], nonce[11], nonce[12]);
    
    // mbedTLS CCM Decryption
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    
    int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "✗ CCM setkey failed: -0x%04X", -ret);
        mbedtls_ccm_free(&ctx);
        return false;
    }
    
    ret = mbedtls_ccm_auth_decrypt(&ctx, payloadLen, 
                                    nonce, 13,
                                    nullptr, 0,
                                    payload, decrypted + 1,
                                    tag, 4);
    
    mbedtls_ccm_free(&ctx);
    
    if (ret != 0) {
        ESP_LOGW(TAG, "✗ CCM decrypt/verify failed: -0x%04X", -ret);
        return false;
    }
    
    decrypted[0] = encryptedData[0] & 0xFE;
    decryptedLen = payloadLen + 1;
    
    ESP_LOGI(TAG, "✓ Decryption successful: %d bytes", decryptedLen);
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
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

// ============================================================================
// PairingCallbacks Implementation
// ============================================================================

void ShellyBLEManager::PairingCallbacks::onConnect(NimBLEClient* pClient) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  CLIENT CONNECTED                 ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Peer: %s", pClient->getPeerAddress().toString().c_str());
    ESP_LOGI(TAG, "MTU: %d bytes", pClient->getMTU());
    ESP_LOGI(TAG, "");
    
    pClient->updateConnParams(120, 120, 0, 60);
}

void ShellyBLEManager::PairingCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    String address = pClient->getPeerAddress().toString().c_str();
    mgr->recentConnections[address] = millis();
    
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
    
    if (connInfo.isBonded() && connInfo.isEncrypted()) {
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "EXTRACTING AND SAVING BINDKEY");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        
        String peerAddr = String(connInfo.getAddress().toString().c_str());
        ESP_LOGI(TAG, "Peer Address: %s", peerAddr.c_str());
        
        String bindkey = "";
        
        #ifdef ESP_PLATFORM
        NimBLEAddress address = connInfo.getAddress();
        
        const ble_addr_t* peer_ble_addr_ptr = reinterpret_cast<const ble_addr_t*>(&address);
        
        ble_store_key_sec key_sec;
        ble_store_value_sec value_sec;
        
        memset(&key_sec, 0, sizeof(key_sec));
        key_sec.peer_addr = *peer_ble_addr_ptr;  // Kopiere die Adresse
        
        if (ble_store_read_peer_sec(&key_sec, &value_sec) == 0) {
            // ✅ LTK als Bindkey
            char bindkeyHex[33];
            for (int i = 0; i < 16; i++) {
                sprintf(&bindkeyHex[i * 2], "%02x", value_sec.ltk[i]);
            }
            bindkeyHex[32] = '\0';
            bindkey = String(bindkeyHex);
            
            ESP_LOGI(TAG, "✓ Extracted LTK as bindkey from NimBLE store");
            ESP_LOGI(TAG, "  Bindkey: %s", bindkey.c_str());
        } else {
            ESP_LOGW(TAG, "⚠ ble_store_read_peer_sec failed");
            ESP_LOGW(TAG, "  Error code: %d", errno);
        }
        #endif
        
        // Bindkey speichern
        if (bindkey.length() > 0) {
            Preferences prefs;
            if (prefs.begin("shelly_ble", false)) {
                String bondKey = "bond_" + peerAddr;
                prefs.putString(bondKey.c_str(), bindkey.c_str());
                prefs.end();
                
                ESP_LOGI(TAG, "✓ Bindkey saved to Preferences");
                ESP_LOGI(TAG, "  Namespace: shelly_ble");
                ESP_LOGI(TAG, "  Key: %s", bondKey.c_str());
            }
        } else {
            ESP_LOGW(TAG, "⚠ Could not extract bindkey");
            ESP_LOGW(TAG, "  Will try to read from device characteristic");
        }
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "");
    }
    
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

void ShellyBLEManager::closeActiveConnection() {
    if (activeClient) {
        ESP_LOGI(TAG, "→ Closing active GATT connection...");
        
        if (activeClient->isConnected()) {
            activeClient->disconnect();
            
            // Warte kurz auf Disconnect
            uint32_t wait_start = millis();
            while (activeClient->isConnected() && (millis() - wait_start < 2000)) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        
        // Client löschen
        NimBLEDevice::deleteClient(activeClient);
        activeClient = nullptr;
        
        ESP_LOGI(TAG, "✓ Connection closed and cleaned up");
    }
    
    if (activeClientCallbacks) {
        delete activeClientCallbacks;
        activeClientCallbacks = nullptr;
    }
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
            ESP_LOGI(TAG, "→ Triggering state change callback...");
            stateChangeCallback(oldState, newState);
        } else {
            ESP_LOGW(TAG, "⚠ stateChangeCallback is NULL!");
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