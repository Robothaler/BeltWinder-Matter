#include "shelly_ble_manager.h"
#include <Preferences.h>
#include <esp_log.h>
#include <mbedtls/ccm.h>
#include <mbedtls/md.h>

static const char* TAG = "ShellyBLE";

// ============================================================================
// Constructor / Destructor
// ============================================================================

ShellyBLEManager::ShellyBLEManager() 
    : initialized(false), scanning(false), continuousScan(false),
      stopOnFirstMatch(false),
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
    pBLEScan->setInterval(300);     // 300ms
    pBLEScan->setWindow(50);        // 50ms → 16.7% Duty Cycle
    
    loadPairedDevice();
    
    initialized = true;
    ESP_LOGI(TAG, "Initialized successfully");
    
    if (isPaired()) {
        ESP_LOGI(TAG, "Paired device loaded: %s (%s)", 
                 pairedDevice.name.c_str(), pairedDevice.address.c_str());
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
// Persistence (Single Device!)
// ============================================================================

void ShellyBLEManager::loadPairedDevice() {
    Preferences prefs;
    if (!prefs.begin("ShellyBLE", true)) {  // Read-only
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
    
    if (!NimBLEDevice::isInitialized()) {
        ESP_LOGE(TAG, "✗ NimBLE not initialized!");
        return;
    }
    
    if (!pBLEScan) {
        ESP_LOGE(TAG, "✗ Scan object is NULL!");
        return;
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "    BLE SCAN STARTING");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Duration: %d seconds", durationSeconds);
    ESP_LOGI(TAG, "Scan type: %s", continuousScan ? "CONTINUOUS" : "DISCOVERY");
    
    // ✅ NEU: Stop-on-First-Match Info
    stopOnFirstMatch = stopOnFirst;
    if (stopOnFirstMatch) {
        ESP_LOGI(TAG, "Mode: STOP ON FIRST SHELLY BLU DOOR/WINDOW");
    }
    
    ESP_LOGI(TAG, "Target devices: Shelly BLU Door/Window (SBDW-*)");
    ESP_LOGI(TAG, "Service UUID: %s", BTHOME_SERVICE_UUID);
    
    if (isPaired()) {
        ESP_LOGI(TAG, "Paired device: %s (%s)", 
                 pairedDevice.name.c_str(), pairedDevice.address.c_str());
    } else {
        ESP_LOGI(TAG, "No device paired yet");
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    // ✓ Discovery Scan löscht alte Discoveries
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
    
    startScan(30, false);  // false = kein Auto-Stop
}


// ============================================================================
// Pairing (Single Device!)
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
    
    // Validate bindkey
    if (bindkey.length() > 0) {
        if (bindkey.length() != 32) {
            ESP_LOGE(TAG, "✗ INVALID BINDKEY LENGTH");
            ESP_LOGE(TAG, "  Expected: 32 hex characters");
            ESP_LOGE(TAG, "  Got: %d characters", bindkey.length());
            return false;
        }
        
        // Check if bindkey contains only valid hex characters
        for (size_t i = 0; i < bindkey.length(); i++) {
            char c = bindkey[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                ESP_LOGE(TAG, "✗ INVALID BINDKEY CHARACTER at position %d: '%c'", i, c);
                ESP_LOGE(TAG, "  Bindkey must contain only 0-9, A-F, a-f");
                return false;
            }
        }
        
        ESP_LOGI(TAG, "✓ Bindkey validation passed (32 hex chars)");
        ESP_LOGI(TAG, "  Bindkey: %s", bindkey.c_str());
    } else {
        ESP_LOGI(TAG, "ℹ No bindkey provided (unencrypted device)");
    }
    
    // Store paired device
    pairedDevice.address = address;
    pairedDevice.name = name;
    pairedDevice.bindkey = bindkey;
    pairedDevice.sensorData = ShellyBLESensorData();  // Reset data
    
    savePairedDevice();
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "✓ PAIRING SUCCESSFUL");
    ESP_LOGI(TAG, "  Device: %s (%s)", name.c_str(), address.c_str());
    ESP_LOGI(TAG, "  Encryption: %s", bindkey.length() > 0 ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    return true;
}

bool ShellyBLEManager::pairEncryptedDevice(const String& address, uint32_t passkey, uint16_t timeout) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  ENCRYPTED DEVICE PAIRING");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    if (passkey > 999999) {
        ESP_LOGE(TAG, "✗ Invalid passkey: %u (must be 0-999999)", passkey);
        return false;
    }
    
    if (isPaired()) {
        ESP_LOGE(TAG, "✗ Device already paired!");
        ESP_LOGE(TAG, "  Current: %s (%s)", 
                 pairedDevice.name.c_str(), pairedDevice.address.c_str());
        ESP_LOGE(TAG, "  → Unpair first!");
        return false;
    }
    
    ESP_LOGI(TAG, "Target: %s", address.c_str());
    ESP_LOGI(TAG, "Passkey: %06u", passkey);
    
    // Scan stoppen während GATT-Verbindung
    bool wasScanning = scanning;
    if (wasScanning) {
        ESP_LOGI(TAG, "→ Stopping scan for GATT connection...");
        stopScan();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Encryption Key auslesen
    String bindkey;
    bool success = connectAndReadEncryptionKey(address, passkey, bindkey);
    
    if (!success) {
        ESP_LOGE(TAG, "✗ Failed to read encryption key");
        
        if (wasScanning) {
            startScan(30);
        }
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Encryption key retrieved");
    
    // Reguläres Pairing mit Bindkey
    bool paired = pairDevice(address, bindkey);
    
    if (paired) {
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "✓ ENCRYPTED PAIRING SUCCESSFUL");
        ESP_LOGI(TAG, "  Device: %s", pairedDevice.name.c_str());
        ESP_LOGI(TAG, "  MAC: %s", address.c_str());
        ESP_LOGI(TAG, "  Bindkey: %s", bindkey.c_str());
        ESP_LOGI(TAG, "═══════════════════════════════════");
    }
    
    // Scan neu starten
    if (wasScanning) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        startScan(30);
    }
    
    return paired;
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
    ESP_LOGI(TAG, "Device: %s (%s)", 
             pairedDevice.name.c_str(), pairedDevice.address.c_str());
    
    // ✓ WICHTIG: Stoppe Continuous Scan!
    if (continuousScan) {
        ESP_LOGI(TAG, "→ Stopping continuous scan...");
        continuousScan = false;
        
        if (scanning) {
            stopScan();
        }
    }
    
    clearPairedDevice();
    
    ESP_LOGI(TAG, "✓ Device unpaired successfully");
    ESP_LOGI(TAG, "");
    
    return true;
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
        if (enabled) {
            ESP_LOGI(TAG, "  → Device will send periodic status broadcasts");
        } else {
            ESP_LOGI(TAG, "  → Device will only broadcast on events");
        }
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
        ESP_LOGI(TAG, "  → Rotation reports only when change > %d°", degrees);
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
    ESP_LOGW(TAG, "⚠️  Device will need to be re-paired!");
    
    bool success = writeGattCharacteristic(address, GATT_UUID_FACTORY_RESET, 1);
    
    if (success) {
        ESP_LOGI(TAG, "✓ Factory reset successful");
        ESP_LOGI(TAG, "  → Device is now UNENCRYPTED");
        ESP_LOGI(TAG, "  → Remove pairing and scan again");
        
        // Automatisch unpair wenn es das gepaarte Gerät ist
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

bool ShellyBLEManager::connectAndReadEncryptionKey(const String& address, uint32_t passkey, String& bindkey) {
    ESP_LOGI(TAG, "→ Connecting to device to read key...");
    
    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (!pClient) {
        ESP_LOGE(TAG, "✗ Failed to create BLE client");
        return false;
    }
    
    // ✅ KORRIGIERT: 20 SEKUNDEN = 20000 Millisekunden!
    pClient->setConnectTimeout(20000);  // ✓ 20 Sekunden

    std::string stdAddress = address.c_str();
    NimBLEAddress bleAddr(stdAddress, BLE_ADDR_RANDOM);
    
    if (!pClient->connect(bleAddr, false)) {
        ESP_LOGW(TAG, "  Direct connect failed. Trying with explicit address type PUBLIC...");
        
        bleAddr = NimBLEAddress(stdAddress, BLE_ADDR_PUBLIC);
        
        if (!pClient->connect(bleAddr, false)) {
            ESP_LOGE(TAG, "✗ Connection failed completely.");
            NimBLEDevice::deleteClient(pClient);
            return false;
        }
    }
    
    ESP_LOGI(TAG, "✓ Connected");
    
    // Services suchen
    ESP_LOGI(TAG, "→ Discovering services...");
    auto services = pClient->getServices(true);
    if (services.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }

    // 1. Passkey Characteristic finden (0ffb7104-...)
    NimBLEUUID passkeyUUID(GATT_UUID_PASSKEY); 
    NimBLERemoteCharacteristic* pPasskeyChar = nullptr;
    
    for (auto* service : services) {
        pPasskeyChar = service->getCharacteristic(passkeyUUID);
        if (pPasskeyChar) break;
    }

    if (!pPasskeyChar) {
        ESP_LOGE(TAG, "✗ Passkey characteristic not found!");
        ESP_LOGE(TAG, "  Note: Ensure device is in Pairing Mode (10s button hold).");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }

    // 2. Passkey schreiben (4 Bytes, Little Endian)
    ESP_LOGI(TAG, "→ Writing passkey: %06u", passkey);
    uint8_t passkeyBytes[4];
    passkeyBytes[0] = (passkey) & 0xFF;
    passkeyBytes[1] = (passkey >> 8) & 0xFF;
    passkeyBytes[2] = (passkey >> 16) & 0xFF;
    passkeyBytes[3] = (passkey >> 24) & 0xFF;

    if (!pPasskeyChar->writeValue(passkeyBytes, 4, true)) {
        ESP_LOGE(TAG, "✗ Failed to write passkey (Authentication failed)");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    ESP_LOGI(TAG, "✓ Passkey accepted");

    // 3. Encryption Key Characteristic finden (eb0fb41b-...)
    NimBLEUUID encKeyUUID(GATT_UUID_ENCRYPTION_KEY);
    NimBLERemoteCharacteristic* pEncKeyChar = nullptr;

    for (auto* service : services) {
        pEncKeyChar = service->getCharacteristic(encKeyUUID);
        if (pEncKeyChar) break;
    }

    if (!pEncKeyChar || !pEncKeyChar->canRead()) {
        ESP_LOGE(TAG, "✗ Encryption Key characteristic not found or not readable.");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }

    // 4. Key lesen (16 Bytes)
    ESP_LOGI(TAG, "→ Reading BindKey...");
    std::string value = pEncKeyChar->readValue();
    
    if (value.length() != 16) {
        ESP_LOGE(TAG, "✗ Invalid key length: %d bytes (expected 16)", value.length());
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }

    // Konvertiere zu Hex-String
    bindkey = "";
    for (size_t i = 0; i < value.length(); i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (uint8_t)value[i]);
        bindkey += hex;
    }
    
    ESP_LOGI(TAG, "✓ BindKey retrieved: %s", bindkey.c_str());
    
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    return true;
}

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
    
    // ✓ KORRIGIERT
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
    } else {
        if (continuousScan) {
            ESP_LOGW(TAG, "⚠ No devices found in this scan cycle");
        } else {
            ESP_LOGW(TAG, "⚠ No Shelly devices found");
            ESP_LOGI(TAG, "  Possible reasons:");
            ESP_LOGI(TAG, "  - Device out of range");
            ESP_LOGI(TAG, "  - Device powered off");
            ESP_LOGI(TAG, "  - Wrong device name (not SBDW-* or SBBT-*)");
        }
    }
    
    // ✓ WICHTIG: Auto-Restart NUR für Continuous Scan
    if (continuousScan) {
        // ✓ DOPPELTE SICHERHEIT: Prüfe nochmal ob noch gepairt
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
    
    // ✅ FILTER 1: Nur Shelly BLU Door/Window Sensoren
    if (!name.startsWith("SBDW-")) {
        // Komplett ignorieren - kein Log!
        return;
    }
    
    // ✅ AB HIER: Nur noch SBDW-* Geräte
    
    ESP_LOGI(TAG, "┌─────────────────────────────────");
    ESP_LOGI(TAG, "│ 🔍 Shelly BLU Door/Window found");
    ESP_LOGI(TAG, "├─────────────────────────────────");
    ESP_LOGI(TAG, "│ Name: %s", name.c_str());
    ESP_LOGI(TAG, "│ MAC:  %s", address.c_str());
    ESP_LOGI(TAG, "│ RSSI: %d dBm", advertisedDevice->getRSSI());
    
    int8_t rssi = advertisedDevice->getRSSI();
    bool isEncrypted = false;
    bool hasServiceData = false;
    
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
                
                // Optional: Raw data anzeigen (nur bei kleinen Paketen)
                if (serviceData.length() <= 20) {
                    String hexDump = "";
                    for (size_t i = 0; i < serviceData.length(); i++) {
                        char hex[4];
                        snprintf(hex, sizeof(hex), "%02X ", (uint8_t)serviceData[i]);
                        hexDump += hex;
                    }
                    ESP_LOGI(TAG, "│ Raw data: %s", hexDump.c_str());
                }
            }
        }
    }
    
    // ✅ FILTER 2: Add/Update in Discovery List (nur SBDW-*)
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
        
        // ✅ STOP ON FIRST MATCH
        if (stopOnFirstMatch && !continuousScan) {
            ESP_LOGI(TAG, "│");
            ESP_LOGI(TAG, "│ 🎯 FIRST MATCH FOUND - STOPPING SCAN");
            ESP_LOGI(TAG, "│");
            
            // Stop scan asynchron (nicht im Callback!)
            xTaskCreate([](void* param) {
                vTaskDelay(pdMS_TO_TICKS(100));  // Kurze Verzögerung
                
                ShellyBLEManager* mgr = (ShellyBLEManager*)param;
                if (mgr->isScanning()) {
                    mgr->stopScan();
                }
                
                vTaskDelete(NULL);
            }, "stop_scan_task", 2048, this, 1, NULL);
        }
    }
    
    ESP_LOGI(TAG, "└─────────────────────────────────");
    
    // Update Paired Device Data (wenn gepairt und Service Data vorhanden)
    if (isPaired() && pairedDevice.address == address && hasServiceData) {
        ESP_LOGI(TAG, "┌─────────────────────────────────");
        ESP_LOGI(TAG, "│ PAIRED DEVICE DATA UPDATE");
        ESP_LOGI(TAG, "├─────────────────────────────────");
        
        std::string serviceData = advertisedDevice->getServiceData(NimBLEUUID(BTHOME_SERVICE_UUID));
        
        ShellyBLESensorData newData;
        newData.rssi = rssi;
        
        if (parseBTHomePacket((uint8_t*)serviceData.data(), serviceData.length(),
                              pairedDevice.bindkey, address, newData)) {
            
            // Check for data changes
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
            }
            
            pairedDevice.sensorData = newData;
            pairedDevice.sensorData.lastUpdate = millis();
            pairedDevice.sensorData.dataValid = true;
            
            if (dataChanged && sensorDataCallback) {
                ESP_LOGI(TAG, "│ → Triggering callback...");
                sensorDataCallback(address, newData);
            }
            
            ESP_LOGI(TAG, "└─────────────────────────────────");
        } else {
            ESP_LOGW(TAG, "│ ✗ Failed to parse BTHome packet");
            ESP_LOGW(TAG, "└─────────────────────────────────");
        }
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

// ============================================================================
// BTHome v2 Parser (MIT ALLEN NEUEN FEATURES!)
// ============================================================================

bool ShellyBLEManager::parseBTHomePacket(const uint8_t* data, size_t length,
                                         const String& bindkey, 
                                         const String& macAddress,  // ✓ NEU
                                         ShellyBLESensorData& sensorData) {
    ESP_LOGI(TAG, "───────────────────────────────────");
    ESP_LOGI(TAG, "Parsing BTHome packet");
    ESP_LOGI(TAG, "───────────────────────────────────");
    
    if (length < 2) {
        ESP_LOGW(TAG, "✗ Packet too short: %d bytes (min: 2)", length);
        return false;
    }
    
    uint8_t deviceInfo = data[0];
    bool encrypted = (deviceInfo & 0x01) != 0;
    
    ESP_LOGI(TAG, "Packet length: %d bytes", length);
    ESP_LOGI(TAG, "Device Info byte: 0x%02X", deviceInfo);
    ESP_LOGI(TAG, "Encryption: %s", encrypted ? "YES" : "NO");
    
    uint8_t* payload = nullptr;
    size_t payloadLength = 0;
    uint8_t decryptedBuffer[256];
    
    if (encrypted) {
        if (bindkey.length() != 32) {
            ESP_LOGW(TAG, "✗ Encrypted packet but no valid bindkey");
            ESP_LOGW(TAG, "  Bindkey length: %d (expected: 32)", bindkey.length());
            return false;
        }
        
        ESP_LOGI(TAG, "→ Attempting decryption...");
        size_t decryptedLen = 0;
        
        // ✓ KORRIGIERT: MAC-Adresse übergeben
        if (!decryptBTHome(data, length, bindkey, macAddress, decryptedBuffer, decryptedLen)) {
            ESP_LOGW(TAG, "✗ Decryption failed!");
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Decryption successful (%d bytes)", decryptedLen);
        payload = decryptedBuffer + 1;
        payloadLength = decryptedLen - 1;
    } else {
        payload = (uint8_t*)data + 1;
        payloadLength = length - 1;
    }
    
    // Parse payload objects mit SICHEREM Length-Handling
    size_t offset = 0;
    bool hasData = false;
    sensorData.hasButtonEvent = false;  // Reset
    
    while (offset < payloadLength) {
        if (offset >= payloadLength) break;
        
        uint8_t objectId = payload[offset++];
        size_t objectLen = getBTHomeObjectLength(objectId);
        
        if (objectLen == 0) {
            ESP_LOGW(TAG, "  ⚠ Unknown Object ID: 0x%02X (skipping rest)", objectId);
            break;
        }
        
        if (offset + objectLen > payloadLength) {
            ESP_LOGW(TAG, "  ✗ Object 0x%02X: insufficient data (%d/%d bytes)", 
                     objectId, payloadLength - offset, objectLen);
            break;
        }
        
        switch (objectId) {
            case BTHOME_OBJ_PACKET_ID: {  // 0x00 - Packet ID (NEU!)
                sensorData.packetId = payload[offset];
                hasData = true;
                ESP_LOGI(TAG, "  Packet ID: %d", sensorData.packetId);
                break;
            }
            
            case BTHOME_OBJ_BATTERY: {  // 0x01
                sensorData.battery = payload[offset];
                hasData = true;
                ESP_LOGI(TAG, "  Battery: %d%%", sensorData.battery);
                break;
            }
            
            case BTHOME_OBJ_ILLUMINANCE: {  // 0x05
                uint32_t lux_raw = payload[offset] | 
                                  (payload[offset+1] << 8) | 
                                  (payload[offset+2] << 16);
                sensorData.illuminance = lux_raw / 100;  // 0.01 lux -> lux
                hasData = true;
                ESP_LOGI(TAG, "  Illuminance: %d lux", sensorData.illuminance);
                break;
            }
            
            case BTHOME_OBJ_WINDOW: {  // 0x2D
                sensorData.windowOpen = (payload[offset] != 0);
                hasData = true;
                ESP_LOGI(TAG, "  Window: %s", sensorData.windowOpen ? "OPEN 🔓" : "CLOSED 🔒");
                break;
            }
            
            case BTHOME_OBJ_BUTTON: {  // 0x3A - Button Event (NEU!)
                uint16_t btnValue = payload[offset] | (payload[offset+1] << 8);
                sensorData.buttonEvent = static_cast<ShellyButtonEvent>(btnValue & 0xFF);
                sensorData.hasButtonEvent = true;
                hasData = true;
                
                const char* eventName;
                switch (sensorData.buttonEvent) {
                    case BUTTON_SINGLE_PRESS: eventName = "SINGLE PRESS 👆"; break;
                    case BUTTON_HOLD: eventName = "HOLD ⏸️"; break;
                    default: eventName = "NONE";
                }
                
                ESP_LOGI(TAG, "  Button: %s (0x%04X)", eventName, btnValue);
                break;
            }
            
            case BTHOME_OBJ_ROTATION: {  // 0x3F
                int16_t rot_raw = payload[offset] | (payload[offset+1] << 8);
                sensorData.rotation = rot_raw / 10;  // 0.1 deg -> deg
                hasData = true;
                ESP_LOGI(TAG, "  Rotation: %d°", sensorData.rotation);
                break;
            }
            
            default:
                ESP_LOGW(TAG, "  ⚠ Unhandled Object ID: 0x%02X", objectId);
                break;
        }
        
        offset += objectLen;
    }
    
    ESP_LOGI(TAG, "───────────────────────────────────");
    
    return hasData;
}

// ============================================================================
// BTHome Object Length Table (für sicheres Parsing)
// ============================================================================

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
// AES-CCM Decryption (BTHome v2) - KORRIGIERT MIT MAC-ADRESSE!
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
    
    // ✅ BUILD NONCE (13 bytes) - KORRIGIERT!
    uint8_t nonce[13];
    
    // ✅ MAC-Adresse DIREKT verwenden (NICHT reversen!)
    memcpy(nonce, mac, 6);  // <-- HIER IST DIE KORREKTUR!
    
    // ✅ UUID in Little Endian (0xFCD2 → D2 FC)
    nonce[6] = 0xD2;
    nonce[7] = 0xFC;
    
    // Device Info byte
    nonce[8] = encryptedData[0];
    
    // Counter (4 bytes, already Little Endian)
    memcpy(nonce + 9, counter, 4);
    
    ESP_LOGI(TAG, "Nonce constructed (13 bytes):");
    ESP_LOGI(TAG, "  MAC (NORMAL): %02X:%02X:%02X:%02X:%02X:%02X", 
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

// ============================================================================
// Helper: MAC-Adresse parsen
// ============================================================================

bool ShellyBLEManager::parseMacAddress(const String& macStr, uint8_t* mac) {
    if (macStr.length() != 17) {  // Format: XX:XX:XX:XX:XX:XX
        ESP_LOGE(TAG, "Invalid MAC length: %d (expected 17)", macStr.length());
        return false;
    }
    
    for (int i = 0; i < 6; i++) {
        // Check colon separator (except after last byte)
        if (i < 5 && macStr[i*3 + 2] != ':') {
            ESP_LOGE(TAG, "Invalid MAC format at position %d (expected ':')", i*3 + 2);
            return false;
        }
        
        char hex[3] = {macStr[i*3], macStr[i*3+1], 0};
        
        // Validate hex characters
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

bool ShellyBLEManager::connectDevice(const String& address) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PHASE 1: CONNECT (BONDING)      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    if (isPaired()) {
        ESP_LOGE(TAG, "✗ Device already paired! Unpair first.");
        return false;
    }
    
    // ========================================================================
    // SCHRITT 1: Device Info aus Discovery holen
    // ========================================================================
    
    String deviceName = "Unknown";
    uint8_t addressType = BLE_ADDR_RANDOM;  // Default: RANDOM
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
    
    // ========================================================================
    // SCHRITT 2: Scan stoppen
    // ========================================================================
    
    bool wasScanning = scanning;
    if (wasScanning) {
        ESP_LOGI(TAG, "→ Stopping scan...");
        stopScan();
        vTaskDelay(pdMS_TO_TICKS(1500)); 
    }
    
    // ========================================================================
    // SCHRITT 3: Pairing Mode Anweisung
    // ========================================================================
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   ACTION REQUIRED                 ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "📋 Press and HOLD the button for 10+ seconds");
    ESP_LOGI(TAG, "⏳ Waiting 12 seconds...");
    ESP_LOGI(TAG, "");
    
    for (int i = 12; i > 0; i--) {
        if (i % 3 == 0 || i <= 3) {
            ESP_LOGI(TAG, "  ⏱️  %d seconds...", i);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // SCHRITT 4: GATT-Verbindung mit mehreren Versuchen
    // ========================================================================
    
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   ESTABLISHING CONNECTION         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    NimBLEClient* pClient = nullptr;
    bool connected = false;
    
    // ✅ STRATEGIE 1: Mit erkanntem Address Type verbinden
    ESP_LOGI(TAG, "→ Attempt 1: Connecting with detected address type (%s)...",
             addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    
    pClient = NimBLEDevice::createClient();
    if (!pClient) {
        ESP_LOGE(TAG, "✗ Failed to create BLE client");
        if (wasScanning) startScan(30);
        return false;
    }
    
    // Connection Timeout auf 25 Sekunden erhöhen!
    pClient->setConnectTimeout(25);
    
    std::string stdAddress = address.c_str();
    NimBLEAddress bleAddr(stdAddress, addressType);
    
    ESP_LOGI(TAG, "  Address: %s", bleAddr.toString().c_str());
    ESP_LOGI(TAG, "  Timeout: 25 seconds");
    
    connected = pClient->connect(bleAddr, false);  // false = no auto-reconnect
    
    if (connected) {
        ESP_LOGI(TAG, "✓ Connected successfully with %s address type!",
                 addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    } else {
        ESP_LOGW(TAG, "✗ Attempt 1 failed");
        
        // Cleanup
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // ✅ STRATEGIE 2: Mit alternativen Address Type versuchen
        uint8_t altAddressType = (addressType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "→ Attempt 2: Trying alternative address type (%s)...",
                 altAddressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            ESP_LOGE(TAG, "✗ Failed to create BLE client");
            if (wasScanning) startScan(30);
            return false;
        }
        
        pClient->setConnectTimeout(25);
        
        NimBLEAddress altBleAddr(stdAddress, altAddressType);
        ESP_LOGI(TAG, "  Address: %s", altBleAddr.toString().c_str());
        
        connected = pClient->connect(altBleAddr, false);
        
        if (connected) {
            ESP_LOGI(TAG, "✓ Connected successfully with %s address type!",
                     altAddressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
            
            // Korrekten Address Type speichern
            addressType = altAddressType;
        } else {
            ESP_LOGE(TAG, "✗ Attempt 2 failed");
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGE(TAG, "║  CONNECTION FAILED                ║");
            ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "Possible reasons:");
            ESP_LOGE(TAG, "  1. Device not in pairing mode");
            ESP_LOGE(TAG, "     → Press button for 10+ seconds");
            ESP_LOGE(TAG, "  2. Device too far away (RSSI too low)");
            ESP_LOGE(TAG, "     → Move device closer to ESP32");
            ESP_LOGE(TAG, "  3. Device already paired with another controller");
            ESP_LOGE(TAG, "     → Factory reset the device");
            ESP_LOGE(TAG, "  4. BLE interference");
            ESP_LOGE(TAG, "     → Move away from WiFi routers");
            ESP_LOGE(TAG, "");
            
            NimBLEDevice::deleteClient(pClient);
            if (wasScanning) startScan(30);
            return false;
        }
    }
    
    // ========================================================================
    // SCHRITT 5: Connection bestätigt
    // ========================================================================
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ BLE connection established");
    ESP_LOGI(TAG, "  Peer address: %s", pClient->getPeerAddress().toString().c_str());
    ESP_LOGI(TAG, "  MTU: %d bytes", pClient->getMTU());
    ESP_LOGI(TAG, "");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // ========================================================================
    // SCHRITT 6: Services discovern
    // ========================================================================
    
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
    
    // Debug: Services ausgeben
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Available services:");
    for (auto* pService : services) {
        ESP_LOGI(TAG, "  - %s", pService->getUUID().toString().c_str());
        
        // Characteristics auflisten
        auto chars = pService->getCharacteristics(true);
        for (auto* pChar : chars) {
            String props = "";
            if (pChar->canRead()) props += "R";
            if (pChar->canWrite()) props += "W";
            if (pChar->canNotify()) props += "N";
            
            ESP_LOGI(TAG, "    └─ %s [%s]", 
                     pChar->getUUID().toString().c_str(), props.c_str());
        }
    }
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // SCHRITT 7: Trennen (Bonding ist abgeschlossen)
    // ========================================================================
    
    ESP_LOGI(TAG, "→ Disconnecting...");
    pClient->disconnect();
    
    uint8_t retries = 0;
    while (pClient->isConnected() && retries < 20) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retries++;
    }
    
    NimBLEDevice::deleteClient(pClient);
    
    ESP_LOGI(TAG, "✓ Disconnected");
    
    // ========================================================================
    // SCHRITT 8: Gerät als "connected but not encrypted" speichern
    // ========================================================================
    
    pairedDevice.address = address;
    pairedDevice.name = deviceName;
    pairedDevice.bindkey = "";  // KEIN BINDKEY!
    pairedDevice.sensorData = ShellyBLESensorData();
    
    savePairedDevice();
    
    deviceState = STATE_CONNECTED_UNENCRYPTED;
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ✓ CONNECTION SUCCESSFUL          ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device: %s (%s)", deviceName.c_str(), address.c_str());
    ESP_LOGI(TAG, "Status: Connected (Unencrypted)");
    ESP_LOGI(TAG, "Address Type: %s", 
             addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Next step: Enable encryption via Encrypt button");
    ESP_LOGI(TAG, "");
    
    // Scan neu starten
    if (wasScanning) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        startScan(30);
    }
    
    return true;
}

bool ShellyBLEManager::enableEncryption(const String& address, uint32_t passkey) {
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PHASE 2: ENABLE ENCRYPTION      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    
    // 1. Scan stoppen
    if (scanning) {
        stopScan();
        vTaskDelay(pdMS_TO_TICKS(1500)); 
    }
    
    // 2. Client erstellen
    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (!pClient) return false;
    pClient->setConnectTimeout(20);

    // 3. Verbinden
    ESP_LOGI(TAG, "→ Connecting to %s...", address.c_str());
    
    // FIX 1: Adresse mit Typ erstellen. Wir probieren PUBLIC (da in Logs gesehen), sonst RANDOM.
    NimBLEAddress bleAddr(address.c_str(), BLE_ADDR_PUBLIC);
    bool connected = pClient->connect(bleAddr, false);
    
    if (!connected) {
        ESP_LOGW(TAG, "⚠ Connect with PUBLIC address failed, trying RANDOM...");
        NimBLEAddress bleAddrrnd(address.c_str(), BLE_ADDR_RANDOM);
        connected = pClient->connect(bleAddrrnd, false);
    }

    if (!connected) {
        ESP_LOGE(TAG, "✗ Connection failed");
        NimBLEDevice::deleteClient(pClient);
        if (continuousScan) startScan(30);
        return false;
    }
    
    // 4. Passkey Characteristic suchen
    NimBLEUUID passkeyUUID(GATT_UUID_PASSKEY);
    NimBLERemoteCharacteristic* pPasskeyChar = nullptr;
    
    auto services = pClient->getServices(true);
    for (auto* pService : services) {
        pPasskeyChar = pService->getCharacteristic(passkeyUUID);
        if (pPasskeyChar) break;
    }
    
    if (!pPasskeyChar) {
        ESP_LOGE(TAG, "✗ Passkey char not found");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    // 5. Passkey schreiben
    uint8_t passkeyBytes[4];
    passkeyBytes[0] = (passkey) & 0xFF;
    passkeyBytes[1] = (passkey >> 8) & 0xFF;
    passkeyBytes[2] = (passkey >> 16) & 0xFF;
    passkeyBytes[3] = (passkey >> 24) & 0xFF;
    
    ESP_LOGI(TAG, "→ Writing Passkey: %u", passkey);
    if (!pPasskeyChar->writeValue(passkeyBytes, 4, true)) {
        pPasskeyChar->writeValue(passkeyBytes, 4, false);
    }
    
    ESP_LOGI(TAG, "✓ Passkey written. Device is rebooting...");
    
    // 6. Device Reboot abwarten
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    pClient = nullptr;
    
    for(int i=0; i<5; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "  Waiting for reboot... %d/5", i+1);
    }
    
    // 7. Re-Discovery
    ESP_LOGI(TAG, "→ Looking for device (Re-Discovery)...");
    
    String newAddress = address;
    uint8_t newType = BLE_ADDR_RANDOM;
    bool found = false;
    
    pBLEScan->clearResults();
    if(pBLEScan->start(10, false)) {
         NimBLEScanResults results = pBLEScan->getResults();
         for(int i=0; i<results.getCount(); i++) {
             // FIX 2: NimBLEAdvertisedDevice ist ein Pointer
             const NimBLEAdvertisedDevice* dev = results.getDevice(i);
             String name = dev->getName().c_str();
             
             if (name == pairedDevice.name) {
                 newAddress = dev->getAddress().toString().c_str();
                 newType = dev->getAddress().getType();
                 found = true;
                 ESP_LOGI(TAG, "✓ Device found! Addr: %s", newAddress.c_str());
                 break;
             }
         }
    }
    pBLEScan->stop();
    
    if (!found) {
        ESP_LOGW(TAG, "⚠ Device not found by name after reboot. Trying old address...");
    }

    // 8. Reconnect & Read Key
    ESP_LOGI(TAG, "→ Connecting to read key...");
    pClient = NimBLEDevice::createClient();
    pClient->setConnectTimeout(20);
    
    // Hier verwenden wir die neu gefundene Adresse und Typ
    if (!pClient->connect(NimBLEAddress(newAddress.c_str(), newType), false)) {
        ESP_LOGE(TAG, "✗ Final connection failed");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    NimBLEUUID keyUUID(GATT_UUID_ENCRYPTION_KEY);
    NimBLERemoteCharacteristic* pKeyChar = nullptr;
    
    services = pClient->getServices(true);
    for (auto* pService : services) {
        pKeyChar = pService->getCharacteristic(keyUUID);
        if (pKeyChar) break;
    }
    
    String bindkey = "";
    if (pKeyChar && pKeyChar->canRead()) {
        std::string val = pKeyChar->readValue();
        if (val.length() == 16) {
            for (size_t i = 0; i < val.length(); i++) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", (uint8_t)val[i]);
                bindkey += hex;
            }
            ESP_LOGI(TAG, "✓ Bindkey read: %s", bindkey.c_str());
        }
    }
    
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    
    if (bindkey.length() == 32) {
        pairedDevice.address = newAddress;
        pairedDevice.bindkey = bindkey;
        savePairedDevice();
        
        if (continuousScan) startScan(30);
        return true;
    }
    
    ESP_LOGE(TAG, "✗ Failed to read valid key");
    return false;
}

bool ShellyBLEManager::pairDeviceAndEnableEncryption(const String& address, uint32_t passkey) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "  PAIR + ENABLE ENCRYPTION");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    if (passkey > 999999) {
        ESP_LOGE(TAG, "✗ Invalid passkey: %u (must be 0-999999)", passkey);
        return false;
    }
    
    if (isPaired()) {
        ESP_LOGE(TAG, "✗ Device already paired! Unpair first.");
        return false;
    }
    
    ESP_LOGI(TAG, "Target: %s", address.c_str());
    ESP_LOGI(TAG, "Passkey: %06u", passkey);
    
    String currentAddress = address;
    
    // ========================================================================
    // SCHRITT 1: Device Info
    // ========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PHASE 1: BONDING (NO ENCRYPTION)║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "We will first pair WITHOUT encryption,");
    ESP_LOGI(TAG, "then activate encryption in Phase 2.");
    ESP_LOGI(TAG, "");
    
    bool wasScanning = scanning;
    if (wasScanning) {
        ESP_LOGI(TAG, "→ Stopping scan...");
        stopScan();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    uint8_t addressType = BLE_ADDR_RANDOM;
    String deviceName = "Unknown";
    bool deviceFound = false;
    
    for (const auto& dev : discoveredDevices) {
        if (dev.address.equalsIgnoreCase(address)) {
            deviceName = dev.name;
            addressType = dev.addressType;
            deviceFound = true;
            ESP_LOGI(TAG, "✓ Device found: %s", deviceName.c_str());
            ESP_LOGI(TAG, "  Address: %s (%s)", 
                     address.c_str(),
                     addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
            break;
        }
    }
    
    if (!deviceFound) {
        ESP_LOGE(TAG, "✗ Device not found in recent scan");
        if (wasScanning) startScan(30);
        return false;
    }
    
    // ========================================================================
    // SCHRITT 2: Pairing Mode
    // ========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   STEP 1: ACTIVATE PAIRING MODE   ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "📋 ACTION REQUIRED:");
    ESP_LOGI(TAG, "   Press and HOLD the pairing button");
    ESP_LOGI(TAG, "   for 10+ seconds until LED flashes rapidly");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "⏳ Waiting 12 seconds...");
    ESP_LOGI(TAG, "");
    
    for (int i = 12; i > 0; i--) {
        if (i % 3 == 0 || i <= 3) {
            ESP_LOGI(TAG, "  ⏱️  %d seconds remaining...", i);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // SCHRITT 3: NORMAL verbinden (OHNE Security!)
    // ========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   STEP 2: CONNECTING (NO SECURITY)║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Creating client...");
    
    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (!pClient) {
        ESP_LOGE(TAG, "✗ Failed to create client");
        if (wasScanning) startScan(30);
        return false;
    }
    
    // ✅ Einfache Callbacks (lokal definiert)
    class SimpleCallbacks : public NimBLEClientCallbacks {
        void onConnect(NimBLEClient* pClient) override {
            ESP_LOGI(TAG, "  → Connected");
        }
        
        void onDisconnect(NimBLEClient* pClient, int reason) override {
            ESP_LOGI(TAG, "  → Disconnected (reason: %d)", reason);
        }
    };
    
    // ✅ Variable als Basistyp deklarieren
    NimBLEClientCallbacks* pCallbacks = new SimpleCallbacks();
    pClient->setClientCallbacks(pCallbacks, false);
    pClient->setConnectTimeout(20);
    
    std::string stdAddress = currentAddress.c_str();
    NimBLEAddress bleAddr(stdAddress, addressType);
    
    ESP_LOGI(TAG, "→ Connecting to: %s", currentAddress.c_str());
    ESP_LOGI(TAG, "  WITHOUT security/encryption (plain bonding)");
    ESP_LOGI(TAG, "");
    
    if (!pClient->connect(bleAddr, true)) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ Connection failed");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Possible reasons:");
        ESP_LOGE(TAG, "  1. Device not in pairing mode");
        ESP_LOGE(TAG, "  2. Pairing mode timeout");
        ESP_LOGE(TAG, "  3. Button not held long enough");
        ESP_LOGE(TAG, "");
        
        delete pCallbacks;
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Connected successfully!");
    ESP_LOGI(TAG, "");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // ========================================================================
    // SCHRITT 4: Services abrufen
    // ========================================================================
    ESP_LOGI(TAG, "→ Discovering services...");
    auto services = pClient->getServices(true);
    
    if (services.empty()) {
        ESP_LOGE(TAG, "✗ No services found");
        pClient->disconnect();
        delete pCallbacks;
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Found %d services", services.size());
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // SCHRITT 5: Passkey Characteristic finden
    // ========================================================================
    NimBLEUUID passkeyUUID(GATT_UUID_PASSKEY);
    NimBLERemoteCharacteristic* pPasskeyChar = nullptr;
    
    ESP_LOGI(TAG, "→ Looking for Passkey characteristic...");
    
    for (auto* pService : services) {
        pPasskeyChar = pService->getCharacteristic(passkeyUUID);
        if (pPasskeyChar) {
            ESP_LOGI(TAG, "✓ Found Passkey characteristic");
            ESP_LOGI(TAG, "  Service: %s", pService->getUUID().toString().c_str());
            break;
        }
    }
    
    if (!pPasskeyChar) {
        ESP_LOGE(TAG, "✗ Passkey characteristic not found!");
        
        // Debug: Alle Services/Characteristics ausgeben
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "Available services:");
        for (auto* pService : services) {
            ESP_LOGE(TAG, "  Service: %s", pService->getUUID().toString().c_str());
            auto chars = pService->getCharacteristics(true);
            for (auto* pChar : chars) {
                ESP_LOGE(TAG, "    - %s", pChar->getUUID().toString().c_str());
            }
        }
        ESP_LOGE(TAG, "");
        
        pClient->disconnect();
        delete pCallbacks;
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    // ========================================================================
    // ✅ PHASE 2: ENCRYPTION AKTIVIEREN
    // ========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   PHASE 2: ENABLE ENCRYPTION      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device is now bonded (paired).");
    ESP_LOGI(TAG, "We can now activate encryption by writing the passkey.");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // SCHRITT 6: Passkey schreiben
    // ========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   STEP 3: WRITING PASSKEY         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Writing passkey to activate encryption...");
    ESP_LOGI(TAG, "  Passkey: %06u", passkey);
    
    uint8_t passkeyBytes[4];
    passkeyBytes[0] = (passkey) & 0xFF;
    passkeyBytes[1] = (passkey >> 8) & 0xFF;
    passkeyBytes[2] = (passkey >> 16) & 0xFF;
    passkeyBytes[3] = (passkey >> 24) & 0xFF;
    
    ESP_LOGI(TAG, "  Bytes (LE): %02X %02X %02X %02X",
             passkeyBytes[0], passkeyBytes[1], passkeyBytes[2], passkeyBytes[3]);
    ESP_LOGI(TAG, "");
    
    bool writeSuccess = pPasskeyChar->writeValue(passkeyBytes, 4, true);
    
    if (!writeSuccess) {
        ESP_LOGW(TAG, "⚠ Write with response failed, trying without response...");
        writeSuccess = pPasskeyChar->writeValue(passkeyBytes, 4, false);
    }
    
    if (!writeSuccess) {
        ESP_LOGE(TAG, "✗ Failed to write passkey");
        pClient->disconnect();
        delete pCallbacks;
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Passkey written successfully!");
    ESP_LOGI(TAG, "  → Device will restart to activate encryption...");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // SCHRITT 7: Warten auf Device Restart
    // ========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   STEP 4: WAITING FOR RESTART     ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Monitoring connection...");
    
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        
        if (!pClient->isConnected()) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "✓ Device disconnected (restart detected)");
            ESP_LOGI(TAG, "");
            break;
        }
    }
    
    // ========================================================================
    // SCHRITT 8: Re-Discovery
    // ========================================================================
    if (!pClient->isConnected()) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   STEP 5: RE-DISCOVERY            ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "⚠ Device address may have changed (BLE Privacy)");
        ESP_LOGI(TAG, "  Original: %s (%s)", 
                 currentAddress.c_str(),
                 addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        ESP_LOGI(TAG, "");
        
        // Cleanup
        delete pCallbacks;
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
        
        ESP_LOGI(TAG, "→ Waiting 5 seconds for device to stabilize...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // Re-Discovery Scan
        ESP_LOGI(TAG, "→ Starting re-discovery scan (15 seconds)...");
        ESP_LOGI(TAG, "  Looking for: %s", deviceName.c_str());
        ESP_LOGI(TAG, "");
        
        std::vector<ShellyBLEDevice> tempDiscovered = discoveredDevices;
        discoveredDevices.clear();
        
        scanning = true;
        bool scanStarted = pBLEScan->start(15, false);
        
        if (!scanStarted) {
            ESP_LOGE(TAG, "✗ Failed to start scan");
            discoveredDevices = tempDiscovered;
            if (wasScanning) startScan(30);
            return false;
        }
        
                // Warten mit Progress
        int scanWait = 0;
        while (scanning && scanWait < 180) {
            vTaskDelay(pdMS_TO_TICKS(100));
            scanWait++;
            
            if (scanWait % 30 == 0) {
                ESP_LOGI(TAG, "  Scanning... %d/%d seconds (%d devices found)", 
                         scanWait / 10, 15, discoveredDevices.size());
            }
        }
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "✓ Scan complete: %d devices found", discoveredDevices.size());
        ESP_LOGI(TAG, "");
        
        // Alle gefundenen Geräte ausgeben
        if (discoveredDevices.size() > 0) {
            ESP_LOGI(TAG, "Discovered devices:");
            for (size_t i = 0; i < discoveredDevices.size(); i++) {
                const auto& dev = discoveredDevices[i];
                ESP_LOGI(TAG, "  [%d] %s", i + 1, dev.name.c_str());
                ESP_LOGI(TAG, "      Address: %s (%s)", 
                         dev.address.c_str(),
                         dev.addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
                ESP_LOGI(TAG, "      RSSI: %d dBm | Encrypted: %s", 
                         dev.rssi, dev.isEncrypted ? "YES" : "NO");
            }
            ESP_LOGI(TAG, "");
        }
        
        // Gerät suchen (nach Name)
        String newAddress = "";
        uint8_t newAddressType = BLE_ADDR_RANDOM;
        bool deviceRediscovered = false;
        
        ESP_LOGI(TAG, "→ Searching for target device...");
        
        // Name-Match
        for (const auto& dev : discoveredDevices) {
            if (dev.name == deviceName) {
                newAddress = dev.address;
                newAddressType = dev.addressType;
                deviceRediscovered = true;
                
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "✓ Device found by NAME!");
                ESP_LOGI(TAG, "  Name: %s", dev.name.c_str());
                
                if (newAddress != currentAddress) {
                    ESP_LOGI(TAG, "  ⚠ ADDRESS CHANGED:");
                    ESP_LOGI(TAG, "    Old: %s (%s)", 
                             currentAddress.c_str(),
                             addressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
                    ESP_LOGI(TAG, "    New: %s (%s)", 
                             newAddress.c_str(),
                             newAddressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
                } else {
                    ESP_LOGI(TAG, "  Address unchanged: %s", newAddress.c_str());
                }
                ESP_LOGI(TAG, "");
                break;
            }
        }
        
        // Fallback: Irgendein Shelly-Gerät
        if (!deviceRediscovered) {
            ESP_LOGW(TAG, "⚠ Name match failed, trying any Shelly device...");
            
            for (const auto& dev : discoveredDevices) {
                if (dev.name.startsWith("SBDW-") || dev.name.startsWith("SBBT-")) {
                    newAddress = dev.address;
                    newAddressType = dev.addressType;
                    deviceRediscovered = true;
                    
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "✓ Found Shelly device (assuming target):");
                    ESP_LOGI(TAG, "  Name: %s (expected: %s)", 
                             dev.name.c_str(), deviceName.c_str());
                    ESP_LOGI(TAG, "  Address: %s (%s)", 
                             newAddress.c_str(),
                             newAddressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
                    ESP_LOGW(TAG, "  ⚠ Name mismatch - using anyway!");
                    ESP_LOGI(TAG, "");
                    break;
                }
            }
        }
        
        // Discoveries wiederherstellen
        for (const auto& dev : tempDiscovered) {
            bool exists = false;
            for (const auto& newDev : discoveredDevices) {
                if (newDev.address == dev.address) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                discoveredDevices.push_back(dev);
            }
        }
        
        if (!deviceRediscovered) {
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGE(TAG, "║  DEVICE NOT FOUND                 ║");
            ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "Searched for: %s", deviceName.c_str());
            ESP_LOGE(TAG, "Found: %d devices (none matched)", discoveredDevices.size());
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "Possible reasons:");
            ESP_LOGE(TAG, "  1. Device needs more time to restart");
            ESP_LOGE(TAG, "  2. Device out of range");
            ESP_LOGE(TAG, "  3. Encryption activation failed");
            ESP_LOGE(TAG, "");
            
            if (wasScanning) startScan(30);
            return false;
        }
        
        // ========================================================================
        // SCHRITT 9: Reconnect mit neuer Adresse
        // ========================================================================
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║   STEP 6: RECONNECTING            ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        ESP_LOGI(TAG, "→ Creating new client...");
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            ESP_LOGE(TAG, "✗ Failed to create client");
            if (wasScanning) startScan(30);
            return false;
        }
        
        // ✅ Neue Callbacks für Reconnect (brauchen PairingCallbacks falls Bonding nötig)
        pCallbacks = new PairingCallbacks();
        pClient->setClientCallbacks(pCallbacks, false);
        pClient->setConnectTimeout(20);
        
        std::string stdNewAddress = newAddress.c_str();
        NimBLEAddress newBleAddr(stdNewAddress, newAddressType);
        
        ESP_LOGI(TAG, "→ Connecting to: %s", newAddress.c_str());
        ESP_LOGI(TAG, "  Type: %s", 
                 newAddressType == BLE_ADDR_PUBLIC ? "PUBLIC" : "RANDOM");
        ESP_LOGI(TAG, "");
        
        if (!pClient->connect(newBleAddr, true)) {
            ESP_LOGE(TAG, "✗ Reconnection failed");
            delete pCallbacks;
            NimBLEDevice::deleteClient(pClient);
            if (wasScanning) startScan(30);
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Reconnected successfully!");
        ESP_LOGI(TAG, "");
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Services neu abrufen
        ESP_LOGI(TAG, "→ Discovering services...");
        services = pClient->getServices(true);
        
        if (services.empty()) {
            ESP_LOGE(TAG, "✗ No services found");
            delete pCallbacks;
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
            if (wasScanning) startScan(30);
            return false;
        }
        
        ESP_LOGI(TAG, "✓ Found %d services", services.size());
        ESP_LOGI(TAG, "");
        
        // Adresse aktualisieren
        currentAddress = newAddress;
    }
    
    // ========================================================================
    // SCHRITT 10: Encryption Key auslesen
    // ========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   STEP 7: READING ENCRYPTION KEY  ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    NimBLEUUID encKeyUUID(GATT_UUID_ENCRYPTION_KEY);
    NimBLERemoteCharacteristic* pEncKeyChar = nullptr;
    
    ESP_LOGI(TAG, "→ Looking for Encryption Key characteristic...");
    ESP_LOGI(TAG, "  UUID: %s", GATT_UUID_ENCRYPTION_KEY);
    ESP_LOGI(TAG, "");
    
    for (auto* pService : services) {
        pEncKeyChar = pService->getCharacteristic(encKeyUUID);
        if (pEncKeyChar) {
            ESP_LOGI(TAG, "✓ Found Encryption Key characteristic");
            ESP_LOGI(TAG, "  Service: %s", pService->getUUID().toString().c_str());
            break;
        }
    }
    
    if (!pEncKeyChar) {
        ESP_LOGE(TAG, "✗ Encryption Key characteristic not found");
        ESP_LOGE(TAG, "");
        
        // Debug: Alle Characteristics ausgeben
        ESP_LOGE(TAG, "Available characteristics:");
        for (auto* pService : services) {
            ESP_LOGE(TAG, "Service: %s", pService->getUUID().toString().c_str());
            auto chars = pService->getCharacteristics(true);
            for (auto* pChar : chars) {
                String props = "";
                if (pChar->canRead()) props += "R";
                if (pChar->canWrite()) props += "W";
                if (pChar->canWriteNoResponse()) props += "w";
                if (pChar->canNotify()) props += "N";
                if (pChar->canIndicate()) props += "I";
                
                ESP_LOGE(TAG, "  - %s [%s]", 
                         pChar->getUUID().toString().c_str(),
                         props.c_str());
            }
        }
        ESP_LOGE(TAG, "");
        
        delete pCallbacks;
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    ESP_LOGI(TAG, "→ Reading encryption key (with retries)...");
    ESP_LOGI(TAG, "");
    
    std::string keyValue;
    int maxRetries = 5;
    bool keyFound = false;
    
    for (int attempt = 1; attempt <= maxRetries; attempt++) {
        ESP_LOGI(TAG, "  Attempt %d/%d...", attempt, maxRetries);
        
        keyValue = pEncKeyChar->readValue();
        
        if (keyValue.length() == 16) {
            ESP_LOGI(TAG, "  ✓ Key found! (16 bytes)");
            keyFound = true;
            break;
        } else {
            ESP_LOGW(TAG, "  ⚠ Key not ready (%d bytes)", keyValue.length());
            
            if (attempt < maxRetries) {
                ESP_LOGI(TAG, "  → Waiting 1 second...");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
    
    if (!keyFound || keyValue.length() != 16) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGE(TAG, "║  ENCRYPTION KEY READ FAILED       ║");
        ESP_LOGE(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ Failed to read encryption key after %d attempts", maxRetries);
        ESP_LOGE(TAG, "  Final key length: %d bytes (expected 16)", keyValue.length());
        ESP_LOGE(TAG, "");
        
        delete pCallbacks;
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        if (wasScanning) startScan(30);
        return false;
    }
    
    // Bindkey konvertieren
    String bindkey = "";
    for (size_t i = 0; i < keyValue.length(); i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (uint8_t)keyValue[i]);
        bindkey += hex;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ Encryption key retrieved successfully!");
    ESP_LOGI(TAG, "  Bindkey: %s", bindkey.c_str());
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // 11. Verbindung trennen
    // ========================================================================
    ESP_LOGI(TAG, "→ Disconnecting...");
    pClient->disconnect();
    
    uint8_t retries = 0;
    while (pClient->isConnected() && retries < 10) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retries++;
    }
    
    delete pCallbacks;
    NimBLEDevice::deleteClient(pClient);
    ESP_LOGI(TAG, "✓ Disconnected");
    
    // ========================================================================
    // 12. Gerät speichern
    // ========================================================================
    pairedDevice.address = currentAddress;
    pairedDevice.name = deviceName;
    pairedDevice.bindkey = bindkey;
    pairedDevice.sensorData = ShellyBLESensorData();
    
    savePairedDevice();
    
    // ========================================================================
    // 13. Success!
    // ========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ✓ ENCRYPTION ENABLED             ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Device: %s (%s)", deviceName.c_str(), currentAddress.c_str());
    ESP_LOGI(TAG, "Passkey: %06u", passkey);
    ESP_LOGI(TAG, "Bindkey: %s", bindkey.c_str());
        ESP_LOGI(TAG, "");
    
    if (currentAddress != address) {
        ESP_LOGW(TAG, "⚠️  IMPORTANT: Device address changed!");
        ESP_LOGW(TAG, "    Old: %s", address.c_str());
        ESP_LOGW(TAG, "    New: %s", currentAddress.c_str());
        ESP_LOGW(TAG, "    (This is normal - BLE Privacy feature)");
        ESP_LOGW(TAG, "");
    }
    
    ESP_LOGW(TAG, "⚠️  SAVE THESE CREDENTIALS:");
    ESP_LOGW(TAG, "    Passkey: %06u", passkey);
    ESP_LOGW(TAG, "    Bindkey: %s", bindkey.c_str());
    ESP_LOGW(TAG, "    Required for future factory resets!");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // 14. Scan neu starten
    // ========================================================================
    if (wasScanning) {
        ESP_LOGI(TAG, "→ Restarting scan in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        startScan(30);
    }
    
    return true;
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
// PairingCallbacks Implementation (h2zero/esp-nimble-cpp)
// ============================================================================

void ShellyBLEManager::PairingCallbacks::onConnect(NimBLEClient* pClient) {
    ESP_LOGI(TAG, "→ Client connected");
    ESP_LOGI(TAG, "  Peer address: %s", pClient->getPeerAddress().toString().c_str());
    
    // MTU aushandeln
    pClient->updateConnParams(120, 120, 0, 60);
}

void ShellyBLEManager::PairingCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    ESP_LOGI(TAG, "→ Client disconnected");
    ESP_LOGI(TAG, "  Reason code: %d", reason);
    
    // Reason codes (BLE GAP)
    // 0x08 = Connection timeout
    // 0x13 = Remote user terminated connection
    // 0x16 = Connection terminated by local host
    // 0x3E = Connection failed to be established
}

bool ShellyBLEManager::PairingCallbacks::onConnParamsUpdateRequest(
    NimBLEClient* pClient, const ble_gap_upd_params* params) {
    
    ESP_LOGI(TAG, "→ Connection parameters update request");
    ESP_LOGI(TAG, "  Min interval: %d", params->itvl_min);
    ESP_LOGI(TAG, "  Max interval: %d", params->itvl_max);
    ESP_LOGI(TAG, "  Latency: %d", params->latency);
    ESP_LOGI(TAG, "  Timeout: %d", params->supervision_timeout);
    
    return true;  // Accept the parameter update
}

void ShellyBLEManager::PairingCallbacks::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  AUTHENTICATION COMPLETE          ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // Connection Security Status prüfen
    bool encrypted = connInfo.isEncrypted();
    bool bonded = connInfo.isBonded();
    bool authenticated = connInfo.isAuthenticated();
    
    if (encrypted) {
        ESP_LOGI(TAG, "  ✓ Connection is ENCRYPTED");
        pairingSuccess = true;
    } else {
        ESP_LOGW(TAG, "  ✗ Connection is NOT encrypted");
        pairingSuccess = false;
    }
    
    if (bonded) {
        ESP_LOGI(TAG, "  ✓ Connection is BONDED");
    } else {
        ESP_LOGW(TAG, "  ✗ Connection is NOT bonded");
    }
    
    if (authenticated) {
        ESP_LOGI(TAG, "  ✓ Connection is AUTHENTICATED");
    } else {
        ESP_LOGW(TAG, "  ✗ Connection is NOT authenticated");
    }
    
    // Security Level ausgeben
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Security Details:");
    ESP_LOGI(TAG, "  Encryption Key Size: %d bytes", connInfo.getSecKeySize());
    ESP_LOGI(TAG, "");
    
    // Pairing als abgeschlossen markieren
    pairingComplete = true;
}

// Verwendet NimBLEConnInfo statt separater Parameter
void ShellyBLEManager::PairingCallbacks::onPassKeyEntry(NimBLEConnInfo& connInfo) {
    ESP_LOGI(TAG, "→ Passkey entry requested by device");
    
    // Passkey aus globaler Security-Config holen und injizieren
    uint32_t key = NimBLEDevice::getSecurityPasskey();
    
    ESP_LOGI(TAG, "  Injecting passkey: %06u", key);
    
    // ✅ Passkey mit NimBLEDevice::injectPassKey injizieren
    NimBLEDevice::injectPassKey(connInfo, key);
}

// Verwendet NimBLEConnInfo + pass_key Parameter
void ShellyBLEManager::PairingCallbacks::onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pass_key) {
    ESP_LOGI(TAG, "→ Confirm passkey request");
    ESP_LOGI(TAG, "  Passkey to confirm: %06u", pass_key);
    
    // Bei "Numeric Comparison" Pairing
    uint32_t expectedKey = NimBLEDevice::getSecurityPasskey();
    
    if (pass_key == expectedKey) {
        ESP_LOGI(TAG, "  ✓ Passkey matches - confirming");
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    } else {
        ESP_LOGW(TAG, "  ✗ Passkey mismatch (expected: %06u) - rejecting", expectedKey);
        NimBLEDevice::injectConfirmPasskey(connInfo, false);
    }
}
