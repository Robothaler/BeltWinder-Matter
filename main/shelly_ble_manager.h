#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEClient.h> 
#include <vector>
#include <map>
#include <functional>

// ============================================================================
// BTHome v2 Konstanten
// ============================================================================

#define BTHOME_SERVICE_UUID "0000fcd2-0000-1000-8000-00805f9b34fb"

// BTHome Object IDs
#define BTHOME_OBJ_PACKET_ID    0x00
#define BTHOME_OBJ_BATTERY      0x01
#define BTHOME_OBJ_ILLUMINANCE  0x05
#define BTHOME_OBJ_WINDOW       0x2D
#define BTHOME_OBJ_BUTTON       0x3A
#define BTHOME_OBJ_ROTATION     0x3F

// ============================================================================
// GATT Characteristic UUIDs
// ============================================================================

#define GATT_UUID_BEACON_MODE           "cb9e957e-952d-4761-a7e1-4416494a5bfa"
#define GATT_UUID_ANGLE_THRESHOLD       "86e7cc43-19f4-4f38-b5ad-1ae586237e2a"
#define GATT_UUID_FACTORY_RESET         "b0a7e40f-2b87-49db-801c-eb3686a24bdb"
#define GATT_UUID_PASSKEY               "0ffb7104-860c-49ae-8989-1f946d5f6c03"
#define GATT_UUID_ENCRYPTION_KEY        "eb0fb41b-af4b-4724-a6f9-974f55aba81a"

// ============================================================================
// Enums & Structs
// ============================================================================

/**
 * @brief Button Event Types (BTHome Object 0x3A)
 */
enum ShellyButtonEvent {
    BUTTON_NONE = 0x00,
    BUTTON_SINGLE_PRESS = 0x01,
    BUTTON_HOLD = 0x80
};

/**
 * @brief Sensor-Daten vom Shelly BLU Door/Window Sensor
 */
struct ShellyBLESensorData {
    uint8_t battery;
    uint32_t illuminance;
    bool windowOpen;
    int16_t rotation;
    int8_t rssi;
    uint32_t lastUpdate;
    bool dataValid;
    
    uint8_t packetId;
    ShellyButtonEvent buttonEvent;
    bool hasButtonEvent;
    
    ShellyBLESensorData() 
        : battery(0), illuminance(0), windowOpen(false), rotation(0),
          rssi(0), lastUpdate(0), dataValid(false),
          packetId(0), buttonEvent(BUTTON_NONE), hasButtonEvent(false) {}
};

/**
 * @brief Entdecktes BLE-Gerät (während Scan)
 */
struct ShellyBLEDevice {
    String address;
    String name;
    int8_t rssi;
    bool isEncrypted;
    uint32_t lastSeen;
    uint8_t addressType;
    
    ShellyBLEDevice() 
        : rssi(0), isEncrypted(false), lastSeen(0), addressType(BLE_ADDR_RANDOM) {}
};

/**
 * @brief Gepairtes Gerät (gespeichert in Preferences)
 */
struct PairedShellyDevice {
    String address;
    String name;
    String bindkey;
    ShellyBLESensorData sensorData;
    
    PairedShellyDevice() {}
};

/**
 * @brief Device-Konfiguration (via GATT ausgelesen)
 */
struct DeviceConfig {
    bool beaconModeEnabled;
    uint8_t angleThreshold;
    bool valid;
    
    DeviceConfig() 
        : beaconModeEnabled(false), angleThreshold(0), valid(false) {}
};

/**
 * @brief Callback für Sensor-Daten-Updates
 */
typedef std::function<void(const String& address, const ShellyBLESensorData& data)> SensorDataCallback;

// ============================================================================
// ShellyBLEManager Hauptklasse
// ============================================================================

class ShellyBLEManager {
public:
    // ========================================================================
    // Constructor / Destructor
    // ========================================================================
    ShellyBLEManager();
    ~ShellyBLEManager();
    
    // ========================================================================
    // Initialization
    // ========================================================================
    bool begin();
    void end();
    
    // ========================================================================
    // Discovery / Scanning
    // ========================================================================
    void startScan(uint16_t durationSeconds = 10, bool stopOnFirstMatch = false);
    void stopScan();
    void startContinuousScan();
    bool isScanning() const { return scanning; }
    bool isContinuousScanActive() const;
    String getScanStatus() const;
    
    std::vector<ShellyBLEDevice> getDiscoveredDevices() const { 
        return discoveredDevices; 
    }
    
    void savePasskey(uint32_t passkey);
    uint32_t getPasskey();

    // ========================================================================
    // State Management
    // ========================================================================
    enum DeviceState {
        STATE_NOT_PAIRED = 0,
        STATE_CONNECTED_UNENCRYPTED = 1,
        STATE_CONNECTED_ENCRYPTED = 2
    };
    
    DeviceState getDeviceState() const;
    
    // ========================================================================
    // Pairing (Single Device!) - OPTIMIERT
    // ========================================================================
    
    // Einfaches Pairing (für unverschlüsselte Geräte)
    bool pairDevice(const String& address, const String& bindkey = "");
    
    bool connectDevice(const String& address);                           // Phase 1: Connect
    bool enableEncryption(const String& address, uint32_t passkey);      // Phase 2: Encrypt
    
    // Unpair
    bool unpairDevice();
    
    bool isPaired() const { 
        return pairedDevice.address.length() > 0; 
    }
    
    PairedShellyDevice getPairedDevice() const { 
        return pairedDevice; 
    }
    
    // ========================================================================
    // Sensor Data Access
    // ========================================================================
    bool getSensorData(ShellyBLESensorData& data) const;
    
    // ========================================================================
    // GATT Configuration
    // ========================================================================
    bool setBeaconMode(const String& address, bool enabled);
    bool setAngleThreshold(const String& address, uint8_t degrees);
    bool factoryResetDevice(const String& address);
    bool readDeviceConfig(const String& address, DeviceConfig& config);
    
    // ========================================================================
    // Callback Registration
    // ========================================================================
    void setSensorDataCallback(SensorDataCallback callback) {
        sensorDataCallback = callback;
    }

    // ========================================================================
    // State Callback
    // ========================================================================
    typedef std::function<void(DeviceState oldState, DeviceState newState)> StateChangeCallback;

    void setStateChangeCallback(StateChangeCallback callback) {
        stateChangeCallback = callback;
    }
    
    // ========================================================================
    // Loop & Cleanup
    // ========================================================================
    void loop();
    
private:
    // ========================================================================
    // Scan Callback
    // ========================================================================
    class ScanCallback : public NimBLEScanCallbacks {
    private:
        ShellyBLEManager* mgr;
    public:
        ScanCallback(ShellyBLEManager* manager) : mgr(manager) {}
        void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
        void onScanEnd(const NimBLEScanResults& results, int reason) override;
    };
    
    // ========================================================================
    // Client Callbacks (Connection Events)
    // ========================================================================
    class PairingCallbacks : public NimBLEClientCallbacks {
    private:
        ShellyBLEManager* mgr;
    public:
        bool pairingComplete = false;
        bool pairingSuccess = false;
        
        PairingCallbacks(ShellyBLEManager* manager) : mgr(manager) {}
        
        void onConnect(NimBLEClient* pClient) override;
        void onDisconnect(NimBLEClient* pClient, int reason) override;
        bool onConnParamsUpdateRequest(NimBLEClient* pClient, 
                                    const ble_gap_upd_params* params) override;
        void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
        void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pass_key) override;
        void onPassKeyEntry(NimBLEConnInfo& connInfo) override;
    };

    // ========================================================================
    // Private Helper Functions
    // ========================================================================
    void onAdvertisedDevice(NimBLEAdvertisedDevice* advertisedDevice);
    void onScanComplete(int reason);
    void cleanupOldDiscoveries();
    
    bool quickEncryptionActivation(const String& address, uint32_t passkey);
    bool fullEncryptionSetup(const String& address, uint32_t passkey);
    
    // ✅ NEU: State Management Helper
    void updateDeviceState(DeviceState newState);
    const char* stateToString(DeviceState state) const;
    
    // Helper für Characteristic-Suche
    NimBLERemoteCharacteristic* findCharacteristic(
        std::vector<NimBLERemoteService*>& services,
        const String& uuid
    );
    
    // GATT Operations
    bool writeGattCharacteristic(const String& address, const String& uuid, uint8_t value);
    bool readGattCharacteristic(const String& address, const String& uuid, uint8_t& value);
    
    // BTHome Parsing
    bool parseBTHomePacket(const uint8_t* data, size_t length,
                          const String& bindkey, 
                          const String& macAddress,
                          ShellyBLESensorData& sensorData);
    
    size_t getBTHomeObjectLength(uint8_t objectId);
    
    // Encryption
    bool decryptBTHome(const uint8_t* encryptedData, size_t length,
                      const String& bindkey, 
                      const String& macAddress,
                      uint8_t* decrypted,
                      size_t& decryptedLen);
    
    bool parseMacAddress(const String& macStr, uint8_t* mac);
    
    // Persistence
    void loadPairedDevice();
    void savePairedDevice();
    void clearPairedDevice();
    void closeActiveConnection();
    
    // ========================================================================
    // Private Member Variables
    // ========================================================================
    bool initialized;
    bool scanning;
    bool continuousScan;
    bool stopOnFirstMatch;
    uint32_t activeClientTimestamp;
    
    NimBLEScan* pBLEScan;
    ScanCallback* scanCallback;
    SensorDataCallback sensorDataCallback;
    StateChangeCallback stateChangeCallback;

    NimBLEClient* activeClient = nullptr;
    PairingCallbacks* activeClientCallbacks = nullptr;
    
    std::vector<ShellyBLEDevice> discoveredDevices;
    PairedShellyDevice pairedDevice;
    DeviceState deviceState;
    
    std::map<String, uint32_t> recentConnections;  // address -> millis() timestamp
};
