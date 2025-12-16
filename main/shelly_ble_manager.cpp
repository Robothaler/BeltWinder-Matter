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
      pBLEScan(nullptr), scanCallback(nullptr), sensorDataCallback(nullptr) {
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

void ShellyBLEManager::startScan(uint16_t durationSeconds) {
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
    ESP_LOGI(TAG, "    STARTING BLE SCAN");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Duration: %d seconds", durationSeconds);
    ESP_LOGI(TAG, "Target devices: Shelly SBDW-*, SBBT-*");
    ESP_LOGI(TAG, "Service UUID: %s", BTHOME_SERVICE_UUID);
    
    if (isPaired()) {
        ESP_LOGI(TAG, "Paired device: %s (%s)", 
                 pairedDevice.name.c_str(), pairedDevice.address.c_str());
    } else {
        ESP_LOGI(TAG, "No device paired yet");
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    // Once-off scan: clear previous discoveries
    if (!continuousScan) {
        discoveredDevices.clear();
    }
    
    scanning = true;
    
    uint32_t durationMs = durationSeconds * 1000;
    bool started = pBLEScan->start(durationMs, false);
    
    if (!started) {
        ESP_LOGE(TAG, "✗ pBLEScan->start() failed!");
        scanning = false;
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
    pBLEScan->stop();
    scanning = false;
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "BLE SCAN STOPPED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Total devices found: %d", discoveredDevices.size());
    
    if (discoveredDevices.size() > 0) {
        ESP_LOGI(TAG, "Discovered devices:");
        for (size_t i = 0; i < discoveredDevices.size(); i++) {
            const auto& dev = discoveredDevices[i];
            ESP_LOGI(TAG, "  [%d] %s", i+1, dev.name.c_str());
            ESP_LOGI(TAG, "      MAC: %s | RSSI: %d dBm | Encrypted: %s",
                     dev.address.c_str(), dev.rssi, dev.isEncrypted ? "Yes" : "No");
        }
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
}

void ShellyBLEManager::startContinuousScan() {
    if (!initialized) {
        ESP_LOGE(TAG, "✗ Cannot start scan: Manager not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  STARTING CONTINUOUS BLE SCAN     ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    continuousScan = true;
    startScan(30);  // 30 Sekunden pro Scan-Zyklus
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
    
    ESP_LOGI(TAG, "Unpairing device: %s", pairedDevice.address.c_str());
    
    clearPairedDevice();
    
    return true;
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
    ESP_LOGI(TAG, "→ Connecting to device...");
    
    // Security Setup
    NimBLEDevice::setSecurityPasskey(passkey);
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    
    // GATT-Client erstellen
    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (!pClient) {
        ESP_LOGE(TAG, "✗ Failed to create BLE client");
        return false;
    }
    
    // ✓ KORRIGIERT: Mit BLE_ADDR_PUBLIC
    ESP_LOGI(TAG, "  Attempting connection...");
    NimBLEAddress bleAddr(address.c_str(), BLE_ADDR_PUBLIC);
    if (!pClient->connect(bleAddr, false)) {
        ESP_LOGE(TAG, "✗ Connection failed");
        ESP_LOGE(TAG, "  Possible reasons:");
        ESP_LOGE(TAG, "  - Device out of range");
        ESP_LOGE(TAG, "  - Wrong passkey");
        ESP_LOGE(TAG, "  - Device already connected");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Connected");
    
    // Warte auf Bonding
    if (!pClient->isConnected()) {
        ESP_LOGE(TAG, "✗ Connection lost during bonding");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Bonding successful");
    
    // ✓ KORRIGIERT: const auto*
    NimBLEUUID encKeyUUID(GATT_UUID_ENCRYPTION_KEY);
    NimBLERemoteCharacteristic* pChar = nullptr;
    
    ESP_LOGI(TAG, "→ Searching for Encryption Key characteristic...");

    const auto& services = pClient->getServices(true);
    ESP_LOGI(TAG, "  Found %d services", services.size());

    for (auto service : services) {
        pChar = service->getCharacteristic(encKeyUUID);
        if (pChar) {
            ESP_LOGI(TAG, "✓ Found Encryption Key in service %s", 
                    service->getUUID().toString().c_str());
            break;
        }
    }
    
    if (!pChar) {
        ESP_LOGE(TAG, "✗ Encryption Key characteristic not found");
        ESP_LOGE(TAG, "  UUID: %s", GATT_UUID_ENCRYPTION_KEY);
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    if (!pChar->canRead()) {
        ESP_LOGE(TAG, "✗ Encryption Key characteristic not readable");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    ESP_LOGI(TAG, "→ Reading encryption key...");
    
    std::string value = pChar->readValue();
    
    if (value.length() != 16) {
        ESP_LOGE(TAG, "✗ Invalid key length: %d bytes (expected 16)", value.length());
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    // Konvertiere zu Hex-String (32 Zeichen)
    bindkey = "";
    for (size_t i = 0; i < value.length(); i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (uint8_t)value[i]);
        bindkey += hex;
    }
    
    ESP_LOGI(TAG, "✓ Encryption key read successfully");
    
    // Verbindung sauber trennen
    pClient->disconnect();
    
    // Warte bis Disconnected
    uint8_t retries = 0;
    while (pClient->isConnected() && retries < 10) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retries++;
    }
    
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
    
    // ✓ KORRIGIERT
    NimBLEAddress bleAddr(address.c_str(), BLE_ADDR_PUBLIC);
    if (!pClient->connect(bleAddr, false)) {
        ESP_LOGE(TAG, "✗ Connection failed");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Connected");
    
    // ✓ KORRIGIERT: const auto*
    NimBLERemoteCharacteristic* pChar = nullptr;
    NimBLEUUID targetUUID(uuid.c_str());

    const auto& services = pClient->getServices(true);
    for (auto service : services) {
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
    
    // ✓ KORRIGIERT
    NimBLEAddress bleAddr(address.c_str(), BLE_ADDR_PUBLIC);
    if (!pClient->connect(bleAddr, false)) {
        ESP_LOGE(TAG, "✗ Connection failed");
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    // ✓ KORRIGIERT: const auto*
    NimBLERemoteCharacteristic* pChar = nullptr;
    NimBLEUUID targetUUID(uuid.c_str());

    const auto& services = pClient->getServices(true);
    for (auto service : services) {
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
    
    ESP_LOGI(TAG, "Scan complete. Found %d devices", discoveredDevices.size());
    
    if (discoveredDevices.size() > 0) {
        ESP_LOGI(TAG, "Discovered devices:");
        for (size_t i = 0; i < discoveredDevices.size(); i++) {
            const auto& dev = discoveredDevices[i];
            ESP_LOGI(TAG, "  [%d] %s", i+1, dev.name.c_str());
            ESP_LOGI(TAG, "      MAC: %s | RSSI: %d dBm | Encrypted: %s",
                     dev.address.c_str(), dev.rssi, dev.isEncrypted ? "Yes" : "No");
        }
    } else {
        ESP_LOGW(TAG, "⚠ No Shelly devices found");
        ESP_LOGI(TAG, "  Possible reasons:");
        ESP_LOGI(TAG, "  - Device out of range");
        ESP_LOGI(TAG, "  - Device powered off");
        ESP_LOGI(TAG, "  - Wrong device name (not SBDW-* or SBBT-*)");
    }
    
    // Auto-Restart für kontinuierlichen Scan
    if (continuousScan) {
        ESP_LOGI(TAG, "→ Restarting continuous scan in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        startScan(30);
    }
}

void ShellyBLEManager::onAdvertisedDevice(NimBLEAdvertisedDevice* advertisedDevice) {
    String name = advertisedDevice->getName().c_str();
    String address = advertisedDevice->getAddress().toString().c_str();
    
    ESP_LOGI(TAG, "┌─────────────────────────────────");
    ESP_LOGI(TAG, "│ BLE Advertisement received");
    ESP_LOGI(TAG, "├─────────────────────────────────");
    ESP_LOGI(TAG, "│ Name: %s", name.c_str());
    ESP_LOGI(TAG, "│ MAC:  %s", address.c_str());
    ESP_LOGI(TAG, "│ RSSI: %d dBm", advertisedDevice->getRSSI());
    
    // Check for Shelly device
    if (!name.startsWith("SBDW-") && !name.startsWith("SBBT-")) {
        ESP_LOGI(TAG, "└─ Not a Shelly device → Ignored");
        return;
    }
    
    ESP_LOGI(TAG, "│ ✓ Shelly device detected!");
    
    int8_t rssi = advertisedDevice->getRSSI();
    bool isEncrypted = false;
    bool hasServiceData = false;
    
    // Check for BTHome Service Data
    if (advertisedDevice->haveServiceUUID()) {
        ESP_LOGI(TAG, "│ ℹ Has Service UUID");
        
        if (advertisedDevice->isAdvertisingService(NimBLEUUID(BTHOME_SERVICE_UUID))) {
            ESP_LOGI(TAG, "│ ✓ BTHome service found");
            
            std::string serviceData = advertisedDevice->getServiceData(NimBLEUUID(BTHOME_SERVICE_UUID));
            if (serviceData.length() > 0) {
                hasServiceData = true;
                uint8_t firstByte = serviceData[0];
                isEncrypted = (firstByte & 0x01) != 0;
                
                ESP_LOGI(TAG, "│ Service data: %d bytes", serviceData.length());
                ESP_LOGI(TAG, "│ Device Info: 0x%02X", firstByte);
                ESP_LOGI(TAG, "│ Encrypted: %s", isEncrypted ? "YES 🔒" : "NO");
                
                // Log raw data (für Debugging)
                if (serviceData.length() <= 20) {
                    String hexDump = "";
                    for (size_t i = 0; i < serviceData.length(); i++) {
                        char hex[4];
                        snprintf(hex, sizeof(hex), "%02X ", (uint8_t)serviceData[i]);
                        hexDump += hex;
                    }
                    ESP_LOGI(TAG, "│ Raw data: %s", hexDump.c_str());
                }
            } else {
                ESP_LOGW(TAG, "│ ⚠ BTHome service, but no data");
            }
        } else {
            ESP_LOGW(TAG, "│ ⚠ Service UUID present, but not BTHome");
        }
    } else {
        ESP_LOGI(TAG, "│ ℹ No Service UUID in this advertisement (normal)");
    }
    
    // Add/Update in Discovery List
    bool found = false;
    for (auto& dev : discoveredDevices) {
        if (dev.address == address) {
            dev.rssi = rssi;
            dev.lastSeen = millis();
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
        
        discoveredDevices.push_back(device);
        
        ESP_LOGI(TAG, "│ ✓ Added to discovered devices");
        ESP_LOGI(TAG, "│   Total discovered: %d", discoveredDevices.size());
    }
    
    ESP_LOGI(TAG, "└─────────────────────────────────");
    
    // Update Paired Device Data (nur wenn Service Data vorhanden)
    if (isPaired() && pairedDevice.address == address && hasServiceData) {
        ESP_LOGI(TAG, "┌─────────────────────────────────");
        ESP_LOGI(TAG, "│ PAIRED DEVICE DATA UPDATE");
        ESP_LOGI(TAG, "├─────────────────────────────────");
        
        std::string serviceData = advertisedDevice->getServiceData(NimBLEUUID(BTHOME_SERVICE_UUID));
        
        ShellyBLESensorData newData;
        newData.rssi = rssi;
        
        // ✓ KORRIGIERT: MAC-Adresse übergeben
        if (parseBTHomePacket((uint8_t*)serviceData.data(), serviceData.length(),
                              pairedDevice.bindkey, address, newData)) {
            
            // Check for data changes
            bool dataChanged = (newData.windowOpen != pairedDevice.sensorData.windowOpen) ||
                              (newData.battery != pairedDevice.sensorData.battery) ||
                              (abs((int)newData.illuminance - (int)pairedDevice.sensorData.illuminance) > 10) ||
                              (newData.rotation != pairedDevice.sensorData.rotation) ||
                              (newData.hasButtonEvent);  // NEU: Button-Events immer melden
            
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
                
                // NEU: Button-Event Logging
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
                ESP_LOGI(TAG, "│ ℹ No significant changes");
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
                                     const String& macAddress,  // ✓ NEU
                                     uint8_t* decrypted,
                                     size_t& decryptedLen) {
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG, "AES-CCM Decryption");
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    if (length < 13) {
        ESP_LOGW(TAG, "✗ Encrypted packet too short: %d bytes (min: 13)", length);
        ESP_LOGI(TAG, "  Format: [DevInfo:1][Counter:4][Payload:N][Tag:4]");
        return false;
    }
    
    ESP_LOGI(TAG, "Input length: %d bytes", length);
    ESP_LOGI(TAG, "MAC Address: %s", macAddress.c_str());
    
    const uint8_t* counter = encryptedData + 1;
    const uint8_t* payload = encryptedData + 5;
    size_t payloadLen = length - 9;
    const uint8_t* tag = encryptedData + length - 4;
    
    ESP_LOGI(TAG, "Counter: %02X %02X %02X %02X", 
             counter[0], counter[1], counter[2], counter[3]);
    ESP_LOGI(TAG, "Payload length: %d bytes", payloadLen);
    ESP_LOGI(TAG, "Tag: %02X %02X %02X %02X", 
             tag[0], tag[1], tag[2], tag[3]);
    
    // Convert hex bindkey
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
    
    // ✓ KORRIGIERT: Nonce mit echter MAC-Adresse aufbauen
    uint8_t nonce[13];
    
    // MAC-Adresse parsen (Format: "AA:BB:CC:DD:EE:FF")
    uint8_t mac[6];
    if (!parseMacAddress(macAddress, mac)) {
        ESP_LOGE(TAG, "✗ Invalid MAC address format: %s", macAddress.c_str());
        return false;
    }
    
    // BTHome v2 Nonce: MAC(6) + UUID(2) + DevInfo(1) + Counter(4)
    memcpy(nonce, mac, 6);
    nonce[6] = 0x1C;  // BTHome Service UUID LSB (0x181C)
    nonce[7] = 0x18;  // BTHome Service UUID MSB
    nonce[8] = encryptedData[0];  // Device Info
    memcpy(nonce + 9, counter, 4);
    
    ESP_LOGI(TAG, "Nonce (13 bytes):");
    ESP_LOGI(TAG, "  MAC:     %02X:%02X:%02X:%02X:%02X:%02X", 
             nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5]);
    ESP_LOGI(TAG, "  UUID:    %02X %02X", nonce[6], nonce[7]);
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
    ESP_LOGI(TAG, "✓ CCM key set");
    
    ret = mbedtls_ccm_auth_decrypt(&ctx, payloadLen, nonce, 13,  // ✓ KORRIGIERT: 13 statt 11
                                    nullptr, 0,
                                    payload, decrypted + 1,
                                    tag, 4);
    
    mbedtls_ccm_free(&ctx);
    
    if (ret != 0) {
        ESP_LOGW(TAG, "✗ CCM decrypt/verify failed: -0x%04X", -ret);
        ESP_LOGW(TAG, "  Possible causes:");
        ESP_LOGW(TAG, "  - Wrong bindkey");
        ESP_LOGW(TAG, "  - Wrong MAC address");
        ESP_LOGW(TAG, "  - Corrupted packet");
        return false;
    }
    
    decrypted[0] = encryptedData[0] & 0xFE;  // Clear encryption bit
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