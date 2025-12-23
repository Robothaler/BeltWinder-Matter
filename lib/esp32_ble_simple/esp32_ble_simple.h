#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <functional>
#include <string>
#include <map>

// NimBLE Includes
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

namespace esp32_ble_simple {

// ═══════════════════════════════════════════════════════════════════════
// Simple UUID Class
// ═══════════════════════════════════════════════════════════════════════

class SimpleBLEUUID {
public:
    SimpleBLEUUID() : is_16bit_(true), uuid16_(0) {}
    explicit SimpleBLEUUID(uint16_t uuid16) : is_16bit_(true), uuid16_(uuid16) {}
    
    bool operator==(const SimpleBLEUUID &other) const {
        if (is_16bit_ != other.is_16bit_) return false;
        return is_16bit_ ? (uuid16_ == other.uuid16_) : false;
    }
    
    bool operator!=(const SimpleBLEUUID &other) const {
        return !(*this == other);
    }
    
    uint16_t get_uuid16() const { return uuid16_; }
    bool is_16bit() const { return is_16bit_; }
    
private:
    bool is_16bit_;
    uint16_t uuid16_;
};

// ═══════════════════════════════════════════════════════════════════════
// Service Data Structure
// ═══════════════════════════════════════════════════════════════════════

struct SimpleBLEServiceData {
    SimpleBLEUUID uuid;
    std::vector<uint8_t> data;
    
    SimpleBLEServiceData() {}
    SimpleBLEServiceData(const SimpleBLEServiceData& other) 
        : uuid(other.uuid), data(other.data) {}
    SimpleBLEServiceData& operator=(const SimpleBLEServiceData& other) {
        if (this != &other) {
            uuid = other.uuid;
            data = other.data;
        }
        return *this;
    }
};

// ═══════════════════════════════════════════════════════════════════════
// SimpleBLEDevice
// ═══════════════════════════════════════════════════════════════════════

class SimpleBLEDevice {
public:
    SimpleBLEDevice() : address_(0), rssi_(0), address_type_(0) {}
    
    void parse_advertisement(const ble_gap_disc_desc *disc) {
        // Store basic info
        address_ = 0;
        for (int i = 0; i < 6; i++) {
            address_ = (address_ << 8) | disc->addr.val[5 - i];
        }
        
        rssi_ = disc->rssi;
        address_type_ = disc->addr.type;
        
        // Clear old data
        service_datas_.clear();
        name_.clear();
        
        // Parse Advertisement Data
        parse_adv_data(disc->data, disc->length_data);
    }
    
    // ═══════════════════════════════════════════════════════════════════
    // Getters
    // ═══════════════════════════════════════════════════════════════════
    
    uint64_t get_address_uint64() const { return address_; }
    
    std::string get_address_str() const {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                (uint8_t)(address_ >> 40), (uint8_t)(address_ >> 32),
                (uint8_t)(address_ >> 24), (uint8_t)(address_ >> 16),
                (uint8_t)(address_ >> 8), (uint8_t)(address_));
        return std::string(buf);
    }
    
    int8_t get_rssi() const { return rssi_; }
    std::string get_name() const { return name_; }
    uint8_t get_address_type() const { return address_type_; }
    
    const std::vector<SimpleBLEServiceData>& get_service_datas() const {
        return service_datas_;
    }
    
    // ═══════════════════════════════════════════════════════════════════
    // ✅ NEUE SETTER-METHODEN für Merging
    // ═══════════════════════════════════════════════════════════════════
    
    void set_name(const std::string& name) {
        name_ = name;
    }
    
    void set_rssi(int8_t rssi) {
        rssi_ = rssi;
    }
    
    void add_service_data(const SimpleBLEServiceData& sd) {
        // Prüfe, ob UUID bereits vorhanden
        for (auto& existing : service_datas_) {
            if (existing.uuid == sd.uuid) {
                // UUID existiert bereits → Daten aktualisieren
                existing.data = sd.data;
                return;
            }
        }
        // Neue UUID → hinzufügen
        service_datas_.push_back(sd);
    }
    
private:
    void parse_adv_data(const uint8_t *data, uint8_t len) {
        size_t pos = 0;
        
        while (pos < len) {
            if (pos >= len) break;
            
            uint8_t field_len = data[pos++];
            if (field_len == 0 || pos + field_len > len) break;
            
            uint8_t field_type = data[pos++];
            uint8_t data_len = field_len - 1;
            const uint8_t *field_data = &data[pos];
            
            switch (field_type) {
                case BLE_HS_ADV_TYPE_COMP_NAME:
                case BLE_HS_ADV_TYPE_INCOMP_NAME:
                    name_.assign((char*)field_data, data_len);
                    break;
                    
                case BLE_HS_ADV_TYPE_SVC_DATA_UUID16: {
                    if (data_len >= 2) {
                        SimpleBLEServiceData sd;
                        uint16_t uuid16 = field_data[0] | (field_data[1] << 8);
                        sd.uuid = SimpleBLEUUID(uuid16);
                        sd.data.assign(field_data + 2, field_data + data_len);
                        service_datas_.push_back(sd);
                    }
                    break;
                }
            }
            
            pos += data_len;
        }
    }
    
    uint64_t address_;
    int8_t rssi_;
    uint8_t address_type_;
    std::string name_;
    std::vector<SimpleBLEServiceData> service_datas_;
};

// ═══════════════════════════════════════════════════════════════════════
// Device Listener Interface
// ═══════════════════════════════════════════════════════════════════════

class SimpleBLEDeviceListener {
public:
    virtual ~SimpleBLEDeviceListener() {}
    virtual bool on_device_found(const SimpleBLEDevice &device) = 0;
};

// ═══════════════════════════════════════════════════════════════════════
// Simple BLE Scanner
// ═══════════════════════════════════════════════════════════════════════

class SimpleBLEScanner {
public:
    SimpleBLEScanner();
    ~SimpleBLEScanner();
    
    // Setup
    bool setup();
    void loop();
    
    // Scanning
    bool start_scan(uint32_t duration_sec = 30);
    void stop_scan();
    bool is_scanning() const { return scanning_; }
    
    // Configuration
    void set_scan_active(bool active) { scan_active_ = active; }
    void set_scan_continuous(bool cont) { scan_continuous_ = cont; }
    void set_scan_interval_ms(uint32_t interval) { scan_interval_ = interval; }
    void set_scan_window_ms(uint32_t window) { scan_window_ = window; }
    
    // Listener
    void register_listener(SimpleBLEDeviceListener *listener) {
        listener_ = listener;
    }

    struct WhitelistEntry {
        std::string mac_address;
        uint8_t address_type;  // BLE_ADDR_PUBLIC (0) oder BLE_ADDR_RANDOM (1)
        
        // Constructor für einfache Erstellung
        WhitelistEntry(const std::string& mac, uint8_t type) 
            : mac_address(mac), address_type(type) {}
        
        WhitelistEntry() : address_type(BLE_ADDR_RANDOM) {}  // Default
    };

    // Whitelist Management
    bool set_scan_whitelist(const std::vector<WhitelistEntry>& entries);
    
    // ✅ OPTIONAL: Helper für einfache Nutzung (nur MAC, Type = RANDOM)
    bool set_scan_whitelist_simple(const std::vector<std::string>& mac_addresses) {
        std::vector<WhitelistEntry> entries;
        for (const auto& mac : mac_addresses) {
            entries.push_back(WhitelistEntry(mac, BLE_ADDR_RANDOM));
        }
        return set_scan_whitelist(entries);
    }
    bool clear_scan_whitelist();
    bool is_whitelist_active() const { return whitelist_active_; }
    
private:
    static int gap_event_handler_static(
        struct ble_gap_event *event,
        void *arg);
    
    int gap_event_handler(struct ble_gap_event *event);
    
    bool scanning_;
    bool scan_active_;
    bool scan_continuous_;
    bool whitelist_active_;
    uint32_t scan_duration_;
    uint32_t scan_interval_;
    uint32_t scan_window_;
    uint32_t scan_start_time_;
    
    SimpleBLEDeviceListener *listener_;
    
    static SimpleBLEScanner *instance_;

    struct CachedDevice {
        SimpleBLEDevice device;
        uint32_t last_seen;
        bool has_adv;      // Hat ADVERTISEMENT gesehen
        bool has_scan_rsp; // Hat SCAN_RSP gesehen
        
        CachedDevice() : last_seen(0), has_adv(false), has_scan_rsp(false) {}
    };

    std::vector<ble_addr_t> whitelist_addrs_;
    
    std::map<uint64_t, CachedDevice> device_cache_;  // Key: MAC as uint64
    uint32_t cache_timeout_ms_ = 5000;  // 5 Sekunden
    
    // Helper: Merge two devices
    void merge_device_data(SimpleBLEDevice& target, const SimpleBLEDevice& source);
    
    // Helper: Cleanup old cache entries
    void cleanup_device_cache();
};

}  // namespace esp32_ble_simple
