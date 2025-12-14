#include "shelly_ble_manager.h"
#include <Preferences.h>
#include <esp_log.h>
#include <mbedtls/ccm.h>

static const char* TAG = "ShellyBLE";

#define BTHOME_OBJ_WINDOW      0x2D
#define BTHOME_OBJ_BATTERY     0x01

// ============================================================================
// Constructor / Destructor
// ============================================================================

ShellyBLEManager::ShellyBLEManager() 
    : initialized(false), scanning(false), pBLEScan(nullptr), scanCallback(nullptr) {
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
    pBLEScan->setInterval(1000);
    pBLEScan->setWindow(30);
    
    loadPairedDevices();
    
    initialized = true;
    ESP_LOGI(TAG, "Initialized successfully (%d paired devices)", pairedDevices.size());
    
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
// Discovery
// ============================================================================

void ShellyBLEManager::startScan(uint16_t durationSeconds) {
    if (!initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }
    
    if (scanning) {
        ESP_LOGW(TAG, "Already scanning");
        return;
    }
    
    ESP_LOGI(TAG, "Starting BLE scan for %d seconds...", durationSeconds);
    
    discoveredDevices.clear();
    scanning = true;
    
    pBLEScan->start(durationSeconds, false);
}

void ShellyBLEManager::stopScan() {
    if (!scanning) return;
    
    pBLEScan->stop();
    scanning = false;
    
    ESP_LOGI(TAG, "Scan stopped. Discovered %d Shelly devices", discoveredDevices.size());
}

// ============================================================================
// Scan Callback
// ============================================================================

void ShellyBLEManager::onAdvertisedDevice(NimBLEAdvertisedDevice* advertisedDevice) {
    String name = advertisedDevice->getName().c_str();
    if (!name.startsWith("SBDW-") && !name.startsWith("SBBT-")) {
        return;
    }
    
    if (!advertisedDevice->haveServiceUUID() || 
        !advertisedDevice->isAdvertisingService(NimBLEUUID(BTHOME_SERVICE_UUID))) {
        return;
    }
    
    String address = advertisedDevice->getAddress().toString().c_str();
    int8_t rssi = advertisedDevice->getRSSI();
    
    std::string serviceData = advertisedDevice->getServiceData(NimBLEUUID(BTHOME_SERVICE_UUID));
    bool isEncrypted = false;
    
    if (serviceData.length() > 0) {
        uint8_t firstByte = serviceData[0];
        isEncrypted = (firstByte & 0x01) != 0;
    }
    
    bool found = false;
    for (auto& dev : discoveredDevices) {
        if (dev.address == address) {
            dev.rssi = rssi;
            dev.lastSeen = millis();
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
        
        discoveredDevices.push_back(device);
        
        ESP_LOGI(TAG, "Discovered: %s (%s) RSSI: %d, Encrypted: %s", 
                 name.c_str(), address.c_str(), rssi, isEncrypted ? "Yes" : "No");
    }
    
    // Paired Device State Update
    PairedShellyDevice* paired = findPairedDevice(address);
    if (paired && serviceData.length() > 0) {
        bool windowOpen = false;
        if (parseBTHomePacket((uint8_t*)serviceData.data(), serviceData.length(),
                              paired->bindkey, windowOpen)) {
            if (paired->windowOpen != windowOpen) {
                paired->windowOpen = windowOpen;
                paired->lastUpdate = millis();
                
                ESP_LOGI(TAG, "Window state changed: %s -> %s", 
                         address.c_str(), windowOpen ? "OPEN" : "CLOSED");
                
                if (windowStateCallback) {
                    windowStateCallback(address, windowOpen);
                }
            }
        }
    }
}

// ============================================================================
// BTHome Parser
// ============================================================================

bool ShellyBLEManager::parseBTHomePacket(const uint8_t* data, size_t length,
                                         const String& bindkey, bool& windowOpen) {
    if (length < 2) return false;
    
    uint8_t deviceInfo = data[0];
    bool encrypted = (deviceInfo & 0x01) != 0;
    
    if (encrypted && bindkey.length() != 32) {
        ESP_LOGW(TAG, "Encrypted packet but no bindkey");
        return false;
    }
    
    // reduced version: only parse unencrypted packets
    if (!encrypted) {
        uint8_t* payload = (uint8_t*)&data[1];
        size_t payloadLength = length - 1;
        
        size_t offset = 0;
        while (offset < payloadLength) {
            if (offset + 1 >= payloadLength) break;
            
            uint8_t objectId = payload[offset++];
            
            if (objectId == BTHOME_OBJ_WINDOW) {
                if (offset >= payloadLength) return false;
                windowOpen = (payload[offset++] != 0);
                ESP_LOGD(TAG, "Window state: %s", windowOpen ? "OPEN" : "CLOSED");
                return true;
            } else if (objectId == BTHOME_OBJ_BATTERY) {
                if (offset >= payloadLength) return false;
                offset++;  // Skip battery value
            } else {
                ESP_LOGW(TAG, "Unknown Object ID: 0x%02X", objectId);
                return false;
            }
        }
    }
    
    return false;
}

bool ShellyBLEManager::decryptBTHome(const uint8_t* encryptedData, size_t length,
                                     const String& bindkey, uint8_t* decrypted) {
    // TODO: Implement AES-CCM decryption
    ESP_LOGW(TAG, "Decryption not yet implemented");
    return false;
}

// ============================================================================
// Pairing & Persistence
// ============================================================================

bool ShellyBLEManager::pairDevice(const String& address, const String& bindkey) {
    if (findPairedDevice(address)) {
        ESP_LOGW(TAG, "Device already paired: %s", address.c_str());
        return false;
    }
    
    String name = "Unknown";
    for (const auto& dev : discoveredDevices) {
        if (dev.address == address) {
            name = dev.name;
            break;
        }
    }
    
    if (bindkey.length() > 0 && bindkey.length() != 32) {
        ESP_LOGE(TAG, "Invalid bindkey length");
        return false;
    }
    
    PairedShellyDevice paired;
    paired.address = address;
    paired.name = name;
    paired.bindkey = bindkey;
    paired.windowOpen = false;
    paired.lastUpdate = 0;
    
    pairedDevices.push_back(paired);
    savePairedDevices();
    
    ESP_LOGI(TAG, "Paired device: %s (%s)", name.c_str(), address.c_str());
    
    return true;
}

bool ShellyBLEManager::unpairDevice(const String& address) {
    for (auto it = pairedDevices.begin(); it != pairedDevices.end(); ++it) {
        if (it->address == address) {
            ESP_LOGI(TAG, "Unpaired device: %s", address.c_str());
            pairedDevices.erase(it);
            savePairedDevices();
            return true;
        }
    }
    return false;
}

PairedShellyDevice* ShellyBLEManager::findPairedDevice(const String& address) {
    for (auto& dev : pairedDevices) {
        if (dev.address == address) {
            return &dev;
        }
    }
    return nullptr;
}

bool ShellyBLEManager::getWindowState(const String& address, bool& isOpen) const {
    for (const auto& dev : pairedDevices) {
        if (dev.address == address) {
            isOpen = dev.windowOpen;
            return true;
        }
    }
    return false;
}

void ShellyBLEManager::loadPairedDevices() {
    Preferences prefs;
    if (!prefs.begin("ShellyBLE", true)) {  // Read-only
        ESP_LOGW(TAG, "ShellyBLE namespace not found, creating...");
        prefs.begin("ShellyBLE", false);
        prefs.putUChar("count", 0);
        prefs.end();
        
        if (!prefs.begin("ShellyBLE", true)) {
            ESP_LOGE(TAG, "Failed to create ShellyBLE namespace");
            return;
        }
    }
    
    uint8_t count = prefs.getUChar("count", 0);
    pairedDevices.clear();
    
    for (uint8_t i = 0; i < count; i++) {
        char key[16];
        
        snprintf(key, sizeof(key), "addr_%d", i);
        String address = prefs.getString(key, "");
        if (address.length() == 0) continue;
        
        snprintf(key, sizeof(key), "name_%d", i);
        String name = prefs.getString(key, "Unknown");
        
        snprintf(key, sizeof(key), "bind_%d", i);
        String bindkey = prefs.getString(key, "");
        
        PairedShellyDevice device;
        device.address = address;
        device.name = name;
        device.bindkey = bindkey;
        device.windowOpen = false;
        device.lastUpdate = 0;
        
        pairedDevices.push_back(device);
    }
    
    prefs.end();
    
    ESP_LOGI(TAG, "Loaded %d paired devices from Preferences", pairedDevices.size());
}

void ShellyBLEManager::savePairedDevices() {
    Preferences prefs;
    prefs.begin("ShellyBLE", false);
    
    prefs.clear();
    prefs.putUChar("count", pairedDevices.size());
    
    for (size_t i = 0; i < pairedDevices.size(); i++) {
        char key[16];
        
        snprintf(key, sizeof(key), "addr_%d", i);
        prefs.putString(key, pairedDevices[i].address);
        
        snprintf(key, sizeof(key), "name_%d", i);
        prefs.putString(key, pairedDevices[i].name);
        
        snprintf(key, sizeof(key), "bind_%d", i);
        prefs.putString(key, pairedDevices[i].bindkey);
    }
    
    prefs.end();
    
    ESP_LOGI(TAG, "Saved %d paired devices to Preferences", pairedDevices.size());
}

void ShellyBLEManager::loop() {
    if (!initialized) return;
    
    cleanupOldDiscoveries();
    
    // Check if scan has finished
    if (scanning && !pBLEScan->isScanning()) {
        scanning = false;
        ESP_LOGI(TAG, "Scan complete. Found %d devices", discoveredDevices.size());
    }
}

void ShellyBLEManager::cleanupOldDiscoveries() {
    uint32_t now = millis();
    const uint32_t timeout = 30000;
    
    for (auto it = discoveredDevices.begin(); it != discoveredDevices.end(); ) {
        if (now - it->lastSeen > timeout) {
            ESP_LOGD(TAG, "Removing stale discovery: %s", it->address.c_str());
            it = discoveredDevices.erase(it);
        } else {
            ++it;
        }
    }
}
