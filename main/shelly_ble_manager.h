#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>
#include <functional>

// ============================================================================
// BTHome v2 Konstanten
// ============================================================================

#define BTHOME_SERVICE_UUID     "181C"

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
    BUTTON_HOLD = 0x80  // auch 0xFE bei Firmware < 1.0.20
};

/**
 * @brief Sensor-Daten vom Shelly BLU Door/Window Sensor
 */
struct ShellyBLESensorData {
    uint8_t battery;              // Battery level (0-100%)
    uint32_t illuminance;         // Illuminance in lux
    bool windowOpen;              // true = open, false = closed
    int16_t rotation;             // Rotation in degrees from closed position
    int8_t rssi;                  // Signal strength
    uint32_t lastUpdate;          // millis() timestamp
    bool dataValid;               // true = data was successfully parsed
    
    // Erweiterte Felder
    uint8_t packetId;             // Revolving counter (0x00)
    ShellyButtonEvent buttonEvent; // Button event (0x3A)
    bool hasButtonEvent;          // true = button event in diesem Paket
    
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
    uint32_t lastSeen;  // millis()
    
    ShellyBLEDevice() 
        : rssi(0), isEncrypted(false), lastSeen(0) {}
};

/**
 * @brief Gepairtes Gerät (gespeichert in Preferences)
 */
struct PairedShellyDevice {
    String address;
    String name;
    String bindkey;  // 32 hex chars (16 bytes AES key)
    ShellyBLESensorData sensorData;
    
    PairedShellyDevice() {}
};

/**
 * @brief Device-Konfiguration (via GATT ausgelesen)
 */
struct DeviceConfig {
    bool beaconModeEnabled;
    uint8_t angleThreshold;  // in degrees
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
    // Lifecycle
    // ========================================================================
    
    ShellyBLEManager();
    ~ShellyBLEManager();
    
    /**
     * @brief Initialisiert BLE und lädt gespeichertes Pairing
     * @return true bei Erfolg
     */
    bool begin();
    
    /**
     * @brief Beendet BLE-Manager und gibt Ressourcen frei
     */
    void end();
    
    /**
     * @brief Muss regelmäßig aufgerufen werden (Discovery-Cleanup)
     */
    void loop();
    
    // ========================================================================
    // Scanning
    // ========================================================================
    
    /**
     * @brief Startet BLE-Scan nach Shelly-Geräten (SBDW-*, SBBT-*)
     * @param durationSeconds Scan-Dauer in Sekunden
     */
    void startScan(uint16_t durationSeconds = 10);
    
    /**
     * @brief Stoppt laufenden Scan
     */
    void stopScan();
    
    /**
     * @brief Startet kontinuierlichen Scan (Auto-Restart nach Ende)
     */
    void startContinuousScan();
    
    /**
     * @brief Prüft, ob gerade gescannt wird
     */
    bool isScanning() const { return scanning; }
    
    /**
     * @brief Gibt Liste aller entdeckten Geräte zurück
     */
    std::vector<ShellyBLEDevice> getDiscoveredDevices() const { return discoveredDevices; }
    
    // ========================================================================
    // Pairing (nur 1 Gerät!)
    // ========================================================================
    
    /**
     * @brief Pairt mit unverschlüsseltem Gerät oder mit bekanntem Bindkey
     * @param address MAC-Adresse (Format: "AA:BB:CC:DD:EE:FF")
     * @param bindkey 32 hex chars (leer = unverschlüsselt)
     * @return true bei Erfolg
     */
    bool pairDevice(const String& address, const String& bindkey = "");
    
    /**
     * @brief Pairt mit verschlüsseltem Gerät und liest Bindkey automatisch aus
     * @param address MAC-Adresse
     * @param passkey 6-stelliger Code (0-999999)
     * @param timeout Timeout in Sekunden
     * @return true bei Erfolg
     */
    bool pairEncryptedDevice(const String& address, uint32_t passkey, uint16_t timeout = 30);
    
    /**
     * @brief Entfernt Pairing
     * @return true bei Erfolg
     */
    bool unpairDevice();
    
    /**
     * @brief Prüft, ob ein Gerät gepairt ist
     */
    bool isPaired() const { return pairedDevice.address.length() > 0; }
    
    /**
     * @brief Gibt gepairtes Gerät zurück
     */
    PairedShellyDevice getPairedDevice() const { return pairedDevice; }
    
    // ========================================================================
    // Sensor Data
    // ========================================================================
    
    /**
     * @brief Liest aktuelle Sensor-Daten vom gepairten Gerät
     * @param data Output-Parameter für Sensor-Daten
     * @return true wenn gültige Daten vorhanden
     */
    bool getSensorData(ShellyBLESensorData& data) const;
    
    /**
     * @brief Registriert Callback für Sensor-Daten-Updates
     */
    void setSensorDataCallback(SensorDataCallback cb) { sensorDataCallback = cb; }
    
    // ========================================================================
    // GATT Configuration (erfordert Bonding!)
    // ========================================================================
    
    /**
     * @brief Aktiviert/deaktiviert Beacon Mode (periodisches Broadcasting)
     * @param address MAC-Adresse
     * @param enabled true = aktiviert
     * @return true bei Erfolg
     */
    bool setBeaconMode(const String& address, bool enabled);
    
    /**
     * @brief Setzt minimalen Winkel für Rotation-Reports
     * @param address MAC-Adresse
     * @param degrees Schwellwert in Grad (0-180)
     * @return true bei Erfolg
     */
    bool setAngleThreshold(const String& address, uint8_t degrees);
    
    /**
     * @brief Factory Reset (deaktiviert Verschlüsselung!)
     * @param address MAC-Adresse
     * @return true bei Erfolg
     */
    bool factoryResetDevice(const String& address);
    
    /**
     * @brief Liest Device-Konfiguration aus
     * @param address MAC-Adresse
     * @param config Output-Parameter
     * @return true bei Erfolg
     */
    bool readDeviceConfig(const String& address, DeviceConfig& config);
    
private:
    // ========================================================================
    // Scan Callback (Nested Class)
    // ========================================================================
    
    class ScanCallback : public NimBLEScanCallbacks {
    public:
        ScanCallback(ShellyBLEManager* manager) : mgr(manager) {}
        
        void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
        void onScanEnd(const NimBLEScanResults& results, int reason) override;
        
    private:
        ShellyBLEManager* mgr;
    };
    
    // ========================================================================
    // Private Members
    // ========================================================================
    
    bool initialized;
    bool scanning;
    bool continuousScan;
    
    NimBLEScan* pBLEScan;
    ScanCallback* scanCallback;
    
    PairedShellyDevice pairedDevice;
    std::vector<ShellyBLEDevice> discoveredDevices;
    
    SensorDataCallback sensorDataCallback;
    
    // ========================================================================
    // Private Methods - Persistence
    // ========================================================================
    
    void loadPairedDevice();
    void savePairedDevice();
    void clearPairedDevice();
    
    // ========================================================================
    // Private Methods - BTHome Parsing
    // ========================================================================
    
    /**
     * @brief Parst BTHome v2 Paket
     * @param data Raw packet data
     * @param length Packet length
     * @param bindkey Encryption key (32 hex chars, leer = unverschlüsselt)
     * @param macAddress MAC-Adresse für Nonce (Format: "AA:BB:CC:DD:EE:FF")
     * @param sensorData Output-Parameter
     * @return true bei Erfolg
     */
    bool parseBTHomePacket(const uint8_t* data, size_t length, 
                          const String& bindkey, 
                          const String& macAddress,
                          ShellyBLESensorData& sensorData);
    
    /**
     * @brief Entschlüsselt BTHome v2 Paket (AES-CCM)
     * @param encryptedData Encrypted packet
     * @param length Packet length
     * @param bindkey Encryption key (32 hex chars)
     * @param macAddress MAC-Adresse für Nonce
     * @param decrypted Output-Buffer
     * @param decryptedLen Output-Länge
     * @return true bei Erfolg
     */
    bool decryptBTHome(const uint8_t* encryptedData, size_t length,
                      const String& bindkey, 
                      const String& macAddress,
                      uint8_t* decrypted, 
                      size_t& decryptedLen);
    
    /**
     * @brief Gibt BTHome Object Length zurück (für sicheres Parsing)
     * @param objectId BTHome Object ID
     * @return Länge in Bytes (0 = unbekannt)
     */
    size_t getBTHomeObjectLength(uint8_t objectId);
    
    // ========================================================================
    // Private Methods - GATT Client
    // ========================================================================
    
    /**
     * @brief Verbindet, bonded und liest Encryption Key aus
     * @param address MAC-Adresse
     * @param passkey 6-stelliger Code
     * @param bindkey Output: 32 hex chars
     * @return true bei Erfolg
     */
    bool connectAndReadEncryptionKey(const String& address, uint32_t passkey, String& bindkey);
    
    /**
     * @brief Schreibt GATT Characteristic
     * @param address MAC-Adresse
     * @param uuid Characteristic UUID
     * @param value Wert (1 byte)
     * @return true bei Erfolg
     */
    bool writeGattCharacteristic(const String& address, const String& uuid, uint8_t value);
    
    /**
     * @brief Liest GATT Characteristic
     * @param address MAC-Adresse
     * @param uuid Characteristic UUID
     * @param value Output-Parameter
     * @return true bei Erfolg
     */
    bool readGattCharacteristic(const String& address, const String& uuid, uint8_t& value);
    
    // ========================================================================
    // Private Methods - Helpers
    // ========================================================================
    
    /**
     * @brief Parst MAC-Adresse von String zu Byte-Array
     * @param macStr Format: "AA:BB:CC:DD:EE:FF"
     * @param mac Output: 6 bytes
     * @return true bei gültigem Format
     */
    bool parseMacAddress(const String& macStr, uint8_t* mac);
    
    /**
     * @brief Scan Callback Handler
     */
    void onAdvertisedDevice(NimBLEAdvertisedDevice* advertisedDevice);
    
    /**
     * @brief Scan-Ende Callback Handler
     */
    void onScanComplete(int reason);
    
    /**
     * @brief Entfernt alte Discoveries (>5 Minuten)
     */
    void cleanupOldDiscoveries();
};
