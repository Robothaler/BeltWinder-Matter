#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>
#include <functional>

#define BTHOME_SERVICE_UUID  "0000fcd2-0000-1000-8000-00805f9b34fb"

// ============================================================================
// Structures
// ============================================================================

struct ShellyBLEDevice {
    String address;
    String name;
    int8_t rssi;
    bool isEncrypted;
    uint32_t lastSeen;
};

struct PairedShellyDevice {
    String address;
    String name;
    String bindkey;
    bool windowOpen;
    uint32_t lastUpdate;
};

typedef std::function<void(const String& address, bool isOpen)> WindowStateCallback;

// ============================================================================
// Shelly BLE Manager Class
// ============================================================================

class ShellyBLEManager {
public:
    ShellyBLEManager();
    ~ShellyBLEManager();
    
    bool begin();
    void end();
    
    void startScan(uint16_t durationSeconds = 10);
    void stopScan();
    bool isScanning() const { return scanning; }
    std::vector<ShellyBLEDevice> getDiscoveredDevices() const { return discoveredDevices; }
    
    bool pairDevice(const String& address, const String& bindkey = "");
    bool unpairDevice(const String& address);
    std::vector<PairedShellyDevice> getPairedDevices() const { return pairedDevices; }
    
    bool getWindowState(const String& address, bool& isOpen) const;
    void setWindowStateCallback(WindowStateCallback callback) { windowStateCallback = callback; }
    
    void loadPairedDevices();
    void savePairedDevices();
    
    void loop();
    
    void onAdvertisedDevice(NimBLEAdvertisedDevice* advertisedDevice);

private:
    class ScanCallback : public NimBLEScanCallbacks {
    public:
        ScanCallback(ShellyBLEManager* mgr) : manager(mgr) {}
        
        void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
            manager->onAdvertisedDevice(advertisedDevice);
        }
        
    private:
        ShellyBLEManager* manager;
    };
    
    bool parseBTHomePacket(const uint8_t* data, size_t length, 
                          const String& bindkey, bool& windowOpen);
    bool decryptBTHome(const uint8_t* encryptedData, size_t length,
                      const String& bindkey, uint8_t* decrypted);
    
    bool initialized;
    bool scanning;
    NimBLEScan* pBLEScan;
    ScanCallback* scanCallback;
    
    std::vector<ShellyBLEDevice> discoveredDevices;
    std::vector<PairedShellyDevice> pairedDevices;
    
    WindowStateCallback windowStateCallback;
    
    void cleanupOldDiscoveries();
    PairedShellyDevice* findPairedDevice(const String& address);
};
