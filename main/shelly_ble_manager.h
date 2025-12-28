// shelly_ble_manager.h

#ifndef SHELLY_BLE_MANAGER_H
#define SHELLY_BLE_MANAGER_H

#pragma once

#include <Arduino.h>
#include <vector>
#include <functional>
#include <map>
#include <memory>  // für unique_ptr
#include <Preferences.h>

// Simple BLE Scanner (C++20 kompatibel)
#include "esp32_ble_simple.h"

// NimBLE nur für GATT Connections (Pairing, Encryption)
#include <NimBLEDevice.h>
#include <NimBLEClient.h>

// Task Stack Sizes
#define BLE_AUTOSTART_TASK_STACK_SIZE  (8192)   // 8KB (war 2KB)
#define BLE_RESTART_TASK_STACK_SIZE    (8192)   // 8KB

// BTHome Constants
#define BTHOME_SERVICE_UUID "fcd2"
#define BTHOME_UUID_UINT16  0xFCD2

// GATT UUIDs
#define GATT_UUID_FACTORY_RESET         "b0a7e40f-2b87-49db-801c-eb3686a24bdb"
#define GATT_UUID_PASSKEY               "0ffb7104-860c-49ae-8989-1f946d5f6c03"
#define GATT_UUID_ENCRYPTION_KEY        "eb0fb41b-af4b-4724-a6f9-974f55aba81a"
#define GATT_UUID_BEACON_MODE           "cb9e957e-952d-4761-a7e1-4416494a5bfa"
#define GATT_UUID_ANGLE_THRESHOLD       "86e7cc43-19f4-4f38-b5ad-1ae586237e2a"
#define GATT_UUID_SAMPLE_BTHOME_DATA    "d52246df-98ac-4d21-be1b-70d5f66a5ddb"

// BTHome Object IDs
#define BTHOME_OBJ_PACKET_ID    0x00
#define BTHOME_OBJ_BATTERY      0x01
#define BTHOME_OBJ_ILLUMINANCE  0x05
#define BTHOME_OBJ_WINDOW       0x2D
#define BTHOME_OBJ_BUTTON       0x3A
#define BTHOME_OBJ_ROTATION     0x3F

// ═══════════════════════════════════════════════════════════════════════
// RAII-KLASSEN FÜR NIMBLE CLIENT MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════

namespace ShellyBLE {

/**
 * @brief RAII-Wrapper für NimBLE Client
 * 
 * Garantiert automatisches Cleanup bei Scope-Exit, auch bei Exceptions.
 */
class NimBLEClientGuard {
public:
    explicit NimBLEClientGuard(NimBLEClient* client = nullptr) 
        : client_(client), should_delete_(true) {}
    
    ~NimBLEClientGuard() {
        if (client_ && should_delete_) {
            ESP_LOGI("BLEGuard", "→ Auto-cleanup NimBLE Client");
            if (client_->isConnected()) {
                client_->disconnect();
            }
            NimBLEDevice::deleteClient(client_);
        }
    }
    
    // Non-copyable
    NimBLEClientGuard(const NimBLEClientGuard&) = delete;
    NimBLEClientGuard& operator=(const NimBLEClientGuard&) = delete;
    
    // Movable
    NimBLEClientGuard(NimBLEClientGuard&& other) noexcept 
        : client_(other.client_), should_delete_(other.should_delete_) {
        other.client_ = nullptr;
    }
    
    NimBLEClient* get() const { return client_; }
    NimBLEClient* operator->() const { return client_; }
    
    // Release ownership (when storing client elsewhere)
    NimBLEClient* release() {
        should_delete_ = false;
        return client_;
    }
    
    void reset(NimBLEClient* new_client = nullptr) {
        if (client_ && should_delete_) {
            if (client_->isConnected()) {
                client_->disconnect();
            }
            NimBLEDevice::deleteClient(client_);
        }
        client_ = new_client;
        should_delete_ = true;
    }
    
private:
    NimBLEClient* client_;
    bool should_delete_;
};

/**
 * @brief RAII-Wrapper für Pairing Callbacks
 */
class PairingCallbacksGuard {
public:
    explicit PairingCallbacksGuard(NimBLEClientCallbacks* callbacks = nullptr)
        : callbacks_(callbacks) {}
    
    ~PairingCallbacksGuard() {
        if (callbacks_) {
            ESP_LOGI("BLEGuard", "→ Auto-cleanup Pairing Callbacks");
            delete callbacks_;
        }
    }
    
    // Non-copyable, non-movable
    PairingCallbacksGuard(const PairingCallbacksGuard&) = delete;
    PairingCallbacksGuard& operator=(const PairingCallbacksGuard&) = delete;
    
    NimBLEClientCallbacks* get() const { return callbacks_; }
    
    NimBLEClientCallbacks* release() {
        auto* tmp = callbacks_;
        callbacks_ = nullptr;
        return tmp;
    }
    
private:
    NimBLEClientCallbacks* callbacks_;
};

} // namespace ShellyBLE

// ═══════════════════════════════════════════════════════════════════════
// Data Structures
// ═══════════════════════════════════════════════════════════════════════

enum ShellyButtonEvent {
    BUTTON_NONE = 0,
    BUTTON_SINGLE_PRESS = 0x01,
    BUTTON_DOUBLE_PRESS = 0x02,
    BUTTON_TRIPLE_PRESS = 0x03,
    BUTTON_LONG_PRESS = 0x04,
    BUTTON_LONG_DOUBLE_PRESS = 0x05,
    BUTTON_LONG_TRIPLE_PRESS = 0x06,
    BUTTON_HOLD = 0x8001
};

struct ShellyBLESensorData {
    uint8_t packetId;
    uint8_t battery;
    uint32_t illuminance;
    bool windowOpen;
    int16_t rotation;
    int8_t rssi;
    
    bool hasButtonEvent;
    ShellyButtonEvent buttonEvent;
    
    uint32_t lastUpdate;
    bool dataValid;
    bool wasEncrypted; 
    
    ShellyBLESensorData() : 
        packetId(0), battery(0), illuminance(0), windowOpen(false),
        rotation(0), rssi(0), hasButtonEvent(false),
        buttonEvent(BUTTON_NONE), lastUpdate(0), dataValid(false), wasEncrypted(false) {}
};

struct ShellyBLEDevice {
    String address;
    String name;
    int8_t rssi;
    bool isEncrypted;
    uint32_t lastSeen;
    uint8_t addressType;
    ShellyBLESensorData sensorData;
};

struct PairedShellyDevice {
    String address;
    String name;
    String bindkey;
    uint8_t addressType;
    ShellyBLESensorData sensorData;
    bool isCurrentlyEncrypted;
    
    PairedShellyDevice() : addressType(BLE_ADDR_RANDOM), isCurrentlyEncrypted(false) {}
};

struct DeviceConfig {
    bool beaconModeEnabled;
    uint8_t angleThreshold;
    bool valid;
    
    DeviceConfig() : beaconModeEnabled(false), angleThreshold(0), valid(false) {}
};

// ═══════════════════════════════════════════════════════════════════════
// Main Manager Class
// ═══════════════════════════════════════════════════════════════════════

class ShellyBLEManager : public esp32_ble_simple::SimpleBLEDeviceListener {
public:
    enum DeviceState {
        STATE_NOT_PAIRED,
        STATE_CONNECTED_UNENCRYPTED,
        STATE_CONNECTED_ENCRYPTED
    };
    
    // Callbacks
    using SensorDataCallback = std::function<void(const String&, const ShellyBLESensorData&)>;
    using StateChangeCallback = std::function<void(DeviceState, DeviceState)>;
    
    ShellyBLEManager();
    ~ShellyBLEManager();
    
    // Lifecycle
    bool begin();
    void end();
    void loop();
    
    // Simple BLE Scanner Interface
    bool on_device_found(const esp32_ble_simple::SimpleBLEDevice &device) override;
    
    // Public API
    void startScan(uint16_t durationSeconds = 30, bool stopOnFirst = false);
    void stopScan(bool manualStop = false);
    void startContinuousScan();
    bool isScanActive() const { return scanning; }
    bool isContinuousScanActive() const { return continuousScan && scanning; }
    bool isBLEStarted() const { return bleScanner != nullptr; }
    static bool hasAnyPairedDevice();
    bool ensureBLEStarted();
    String getScanStatus() const;
    void updateDeviceState(DeviceState newState);
    
    const std::vector<ShellyBLEDevice>& getDiscoveredDevices() const { 
        return discoveredDevices; 
    }
    
    // Pairing
    bool pairDevice(const String& address, const String& bindkey = "");
    bool unpairDevice();
    bool isPaired() const { return pairedDevice.address.length() > 0; }
    const PairedShellyDevice& getPairedDevice() const { return pairedDevice; }
    void loadPairedDevice();
    
    // Encryption Setup
    bool connectDevice(const String& address);
    bool smartConnectDevice(const String& address, uint32_t passkey = 0);
    bool enableEncryption(const String& address, uint32_t passkey);
    
    // GATT Configuration
    bool setBeaconMode(const String& address, bool enabled);
    bool setAngleThreshold(const String& address, uint8_t degrees);
    bool factoryResetDevice(const String& address);
    bool readDeviceConfig(const String& address, DeviceConfig& config);
    bool readSampleBTHomeData(const String& address, ShellyBLESensorData& data);
    
    // Sensor Data
    bool getSensorData(ShellyBLESensorData& data) const;
    DeviceState getDeviceState() const;
    
    // Callbacks
    void setSensorDataCallback(SensorDataCallback cb) { sensorDataCallback = cb; }
    void setStateChangeCallback(StateChangeCallback cb) { stateChangeCallback = cb; }
    
    // Passkey Management
    void savePasskey(uint32_t passkey);
    uint32_t getPasskey();
    
    // Memory Monitoring
    void logMemoryStats(const char* location);
    
private:
    // BLE Components
    esp32_ble_simple::SimpleBLEScanner* bleScanner;
    
    NimBLEClient* activeClient;
    uint32_t activeClientTimestamp;
    bool ble_manually_disabled;
    
    // Pairing Callbacks (vollständige Klassen-Definition)
    class PairingCallbacks : public NimBLEClientCallbacks {
    public:
        PairingCallbacks(ShellyBLEManager* mgr) : manager(mgr), 
            pairingComplete(false), pairingSuccess(false) {}
        
        void onConnect(NimBLEClient* pClient) override;
        void onDisconnect(NimBLEClient* pClient, int reason) override;
        bool onConnParamsUpdateRequest(NimBLEClient* pClient, 
            const ble_gap_upd_params* params) override;
        void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
        void onPassKeyEntry(NimBLEConnInfo& connInfo) override;
        void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pass_key) override;
        
        ShellyBLEManager* manager;
        bool pairingComplete;
        bool pairingSuccess;
    };
    
    PairingCallbacks* activeClientCallbacks;
    
    // State
    bool initialized;
    bool scanning;
    bool continuousScan;
    bool stopOnFirstMatch;
    DeviceState deviceState;
    
    // Data
    std::vector<ShellyBLEDevice> discoveredDevices;
    PairedShellyDevice pairedDevice;
    std::map<String, uint32_t> recentConnections;
    
    // Callbacks
    SensorDataCallback sensorDataCallback;
    StateChangeCallback stateChangeCallback;
    
    // Helper Methods
    void updateDiscoveredDevice(
        const String& address, 
        const String& name, 
        int8_t rssi,
        bool isEncrypted,
        uint64_t addressUint64,
        uint8_t addressType
    );
    
    void cleanupOldDiscoveries();
    const char* stateToString(DeviceState state) const;
    
    // Persistence
    void savePairedDevice();
    void clearPairedDevice();
    
    // Connection Management
    void closeActiveConnection();
    
    // GATT Helpers
    bool writeGattCharacteristic(const String& address, const String& uuid, uint8_t value);
    bool readGattCharacteristic(const String& address, const String& uuid, uint8_t& value);
    NimBLERemoteCharacteristic* findCharacteristic(
        std::vector<NimBLERemoteService*>& services,
        const String& uuid
    );
    
    // BTHome Parsing
    bool parseBTHomePacket(
        const uint8_t* data, 
        size_t length,
        const String& bindkey, 
        const String& macAddress,
        ShellyBLESensorData& sensorData
    );
    
    size_t getBTHomeObjectLength(uint8_t objectId);
    
    // Encryption
    bool decryptBTHome(
        const uint8_t* encryptedData, 
        size_t length,
        const String& bindkey, 
        const String& macAddress,
        uint8_t* decrypted,
        size_t& decryptedLen
    );
    
    bool parseMacAddress(const String& macStr, uint8_t* mac);
    
    friend class PairingCallbacks;
};

#endif // SHELLY_BLE_MANAGER_H
