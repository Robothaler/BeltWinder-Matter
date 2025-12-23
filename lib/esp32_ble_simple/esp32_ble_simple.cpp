#include "esp32_ble_simple.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
    #include "host/ble_hs.h"
    #include "host/ble_gap.h"
}

static const char *TAG = "BLESimple";

namespace esp32_ble_simple {

SimpleBLEScanner *SimpleBLEScanner::instance_ = nullptr;

// ═══════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════════

SimpleBLEScanner::SimpleBLEScanner()
    : scanning_(false),
      scan_active_(true),
      scan_continuous_(false),
      scan_duration_(30),
      scan_interval_(300),
      scan_window_(100),
      scan_start_time_(0),
      listener_(nullptr),
      cache_timeout_ms_(5000),
      whitelist_active_(false) {
    instance_ = this;
}


SimpleBLEScanner::~SimpleBLEScanner() {
    stop_scan();
    device_cache_.clear();
    instance_ = nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════════════════════════════

bool SimpleBLEScanner::setup() {
    ESP_LOGI(TAG, "Setting up Simple BLE Scanner (NimBLE backend)...");
    ESP_LOGI(TAG, "Simple BLE Scanner setup complete");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// Loop
// ═══════════════════════════════════════════════════════════════════════

void SimpleBLEScanner::loop() {
    if (!scanning_) return;
    
    if (!scan_continuous_ && scan_duration_ > 0) {
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - scan_start_time_;
        
        if (elapsed >= (scan_duration_ * 1000)) {
            ESP_LOGI(TAG, "Scan duration elapsed, stopping scan");
            stop_scan();
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Scanning Control
// ═══════════════════════════════════════════════════════════════════════

bool SimpleBLEScanner::start_scan(uint32_t duration_sec) {
    if (scanning_) {
        ESP_LOGW(TAG, "Scan already in progress");
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // ✅ PRE-CHECK: Ist NimBLE Stack bereit?
    // ════════════════════════════════════════════════════════════════════
    
    if (!ble_hs_is_enabled()) {
        ESP_LOGE(TAG, "✗ NimBLE Host is not enabled!");
        ESP_LOGE(TAG, "  Call NimBLEDevice::init() first");
        return false;
    }
    
    if (ble_hs_synced() == 0) {
        ESP_LOGW(TAG, "⚠ NimBLE Host not yet synced with controller");
        ESP_LOGW(TAG, "  Waiting for sync...");
        
        // Warte bis zu 5 Sekunden auf Sync
        for (int i = 0; i < 50; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (ble_hs_synced()) {
                ESP_LOGI(TAG, "✓ NimBLE Host synced after %d ms", i * 100);
                break;
            }
        }
        
        if (ble_hs_synced() == 0) {
            ESP_LOGE(TAG, "✗ NimBLE Host sync timeout!");
            return false;
        }
    }
    
    ESP_LOGI(TAG, "✓ NimBLE Stack Status:");
    ESP_LOGI(TAG, "  Host enabled: YES");
    ESP_LOGI(TAG, "  Host synced: YES");
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════════
    // Rest of start_scan() code...
    // ════════════════════════════════════════════════════════════════════
    
    scan_duration_ = duration_sec;
    scan_start_time_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Starting BLE scan");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Duration: %u seconds", duration_sec);
    ESP_LOGI(TAG, "Mode: %s", scan_active_ ? "ACTIVE" : "PASSIVE");
    ESP_LOGI(TAG, "Interval: %u ms", scan_interval_);
    ESP_LOGI(TAG, "Window: %u ms", scan_window_);
    ESP_LOGI(TAG, "Continuous: %s", scan_continuous_ ? "YES" : "NO");
    
    // Whitelist Status
    if (whitelist_active_) {
        ESP_LOGI(TAG, "Whitelist: ACTIVE (%d device(s))", whitelist_addrs_.size());
        for (size_t i = 0; i < whitelist_addrs_.size(); i++) {
            const auto& addr = whitelist_addrs_[i];
            ESP_LOGI(TAG, "  [%d] %02X:%02X:%02X:%02X:%02X:%02X",
                     i + 1,
                     addr.val[5], addr.val[4], addr.val[3],
                     addr.val[2], addr.val[1], addr.val[0]);
        }
    } else {
        ESP_LOGI(TAG, "Whitelist: DISABLED (scanning all devices)");
    }
    
    ESP_LOGI(TAG, "");
    
    struct ble_gap_disc_params disc_params = {};
    disc_params.filter_duplicates = 0;
    disc_params.passive = scan_active_ ? 0 : 1;
    disc_params.itvl = scan_interval_ * 1000 / 625;
    disc_params.window = scan_window_ * 1000 / 625;
    
    if (whitelist_active_) {
        disc_params.filter_policy = BLE_HCI_SCAN_FILT_USE_WL;  // Use Whitelist!
    } else {
        disc_params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;   // Scan all
    }
    
    disc_params.limited = 0;
    
    ESP_LOGI(TAG, "BLE GAP Parameters:");
    ESP_LOGI(TAG, "  passive: %d", disc_params.passive);
    ESP_LOGI(TAG, "  itvl: %d (BLE units)", disc_params.itvl);
    ESP_LOGI(TAG, "  window: %d (BLE units)", disc_params.window);
    ESP_LOGI(TAG, "  filter_duplicates: %d", disc_params.filter_duplicates);
    ESP_LOGI(TAG, "  filter_policy: %d (%s)",
             disc_params.filter_policy,
             whitelist_active_ ? "WHITELIST" : "NO FILTER");
    ESP_LOGI(TAG, "");
    
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 
                         scan_continuous_ ? BLE_HS_FOREVER : (duration_sec * 1000),
                         &disc_params,
                         gap_event_handler_static,
                         nullptr);
    
    if (rc == 0) {
        scanning_ = true;
        ESP_LOGI(TAG, "✓ ble_gap_disc() returned 0 (success)");
        ESP_LOGI(TAG, "✓ BLE scan started successfully");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        return true;
    } else {
        ESP_LOGE(TAG, "✗ ble_gap_disc() failed with error: %d", rc);
        ESP_LOGE(TAG, "");
        
        // Error code meanings:
        switch (rc) {
            case BLE_HS_EALREADY:
                ESP_LOGE(TAG, "  Error: EALREADY - Scan already in progress");
                break;
            case BLE_HS_EINVAL:
                ESP_LOGE(TAG, "  Error: EINVAL - Invalid parameters");
                break;
            case BLE_HS_EBUSY:
                ESP_LOGE(TAG, "  Error: EBUSY - BLE stack busy");
                break;
            default:
                ESP_LOGE(TAG, "  Error: Unknown (%d)", rc);
                break;
        }
        
        ESP_LOGE(TAG, "═══════════════════════════════════");
        return false;
    }
}



void SimpleBLEScanner::stop_scan() {
    if (!scanning_) return;
    
    int rc = ble_gap_disc_cancel();
    
    if (rc == 0) {
        scanning_ = false;
        ESP_LOGI(TAG, "✓ BLE scan stopped");
    } else {
        ESP_LOGE(TAG, "✗ Failed to stop scan: %d", rc);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// GAP Event Handler
// ═══════════════════════════════════════════════════════════════════════

int SimpleBLEScanner::gap_event_handler_static(
    struct ble_gap_event *event,
    void *arg) {
    
    if (instance_) {
        return instance_->gap_event_handler(event);
    }
    return 0;
}

int SimpleBLEScanner::gap_event_handler(struct ble_gap_event *event) {
    
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            
            uint8_t event_type = event->disc.event_type;
            
            // MAC Address
            char addr_str[18];
            snprintf(addr_str, sizeof(addr_str), 
                    "%02x:%02x:%02x:%02x:%02x:%02x",
                    event->disc.addr.val[5],
                    event->disc.addr.val[4],
                    event->disc.addr.val[3],
                    event->disc.addr.val[2],
                    event->disc.addr.val[1],
                    event->disc.addr.val[0]);
            
            uint64_t mac_uint64 = 0;
            for (int i = 0; i < 6; i++) {
                mac_uint64 |= ((uint64_t)event->disc.addr.val[i]) << (i * 8);
            }
            
            // Parse device
            SimpleBLEDevice device;
            device.parse_advertisement(&event->disc);
            
            uint8_t base_event_type = event_type & 0x0F;
            bool is_adv = (base_event_type == 0x00 || base_event_type == 0x03);
            bool is_scan_rsp = (base_event_type == 0x04);
            
            std::string name = device.get_name();
            
            // ════════════════════════════════════════════════════════════
            // ✅ SMART LOGGING: Nur interessante Devices detailliert loggen
            // ════════════════════════════════════════════════════════════
            
            bool is_interesting = false;
            
            // Check if Shelly BLU
            if (name.length() >= 4 && 
                (name.substr(0, 4) == "SBDW" || 
                 name.substr(0, 4) == "SBW-" ||
                 name.substr(0, 7) == "Shelly ")) {
                is_interesting = true;
            }
            
            // Check if has Service Data (might be relevant)
            if (!is_interesting && device.get_service_datas().size() > 0) {
                // Nur loggen wenn BTHome UUID
                for (const auto& sd : device.get_service_datas()) {
                    if (sd.uuid.is_16bit() && sd.uuid.get_uuid16() == 0xFCD2) {
                        is_interesting = true;
                        break;
                    }
                }
            }
            
            // ════════════════════════════════════════════════════════════
            // CONDITIONAL LOGGING
            // ════════════════════════════════════════════════════════════
            
            if (is_interesting) {
                // ✅ FULL LOGGING für interessante Devices
                ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
                ESP_LOGI(TAG, "║  BLE DEVICE (INTERESTING)         ║");
                ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
                ESP_LOGI(TAG, "Address: %s", addr_str);
                ESP_LOGI(TAG, "Event Type: 0x%02X (%s)", event_type, 
                         is_scan_rsp ? "SCAN_RSP" : "ADVERTISEMENT");
                ESP_LOGI(TAG, "RSSI: %d dBm", event->disc.rssi);
                ESP_LOGI(TAG, "Name: %s", name.empty() ? "(empty)" : name.c_str());
                ESP_LOGI(TAG, "Service Datas: %d", device.get_service_datas().size());
                ESP_LOGI(TAG, "");
            } else {
                // ✅ MINIMAL LOGGING für Noise
                ESP_LOGV(TAG, "BLE: %s | %s | RSSI %d | Name: %s",
                         addr_str,
                         is_scan_rsp ? "SCAN_RSP" : "ADV",
                         event->disc.rssi,
                         name.empty() ? "(none)" : name.c_str());
            }
            
            // ════════════════════════════════════════════════════════════
            // ✅ HOME ASSISTANT STYLE: Callback NUR bei ADV mit Service Data!
            // ════════════════════════════════════════════════════════════
            
            if (scan_active_) {
                auto it = device_cache_.find(mac_uint64);
                
                if (it == device_cache_.end()) {
                    // ════════════════════════════════════════════════════
                    // NEW DEVICE
                    // ════════════════════════════════════════════════════
                    
                    CachedDevice cached;
                    cached.device = device;
                    cached.last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    cached.has_adv = is_adv;
                    cached.has_scan_rsp = is_scan_rsp;
                    
                    device_cache_[mac_uint64] = cached;
                    
                    if (is_interesting) {
                        ESP_LOGI(TAG, "→ Cached new device");
                    }
                    
                    // ✅ CALLBACK NUR bei ADV mit Service Data!
                    if (is_adv && device.get_service_datas().size() > 0) {
                        if (is_interesting) {
                            ESP_LOGI(TAG, "→ ADV has Service Data - processing immediately");
                        }
                        
                        if (listener_) {
                            bool should_continue = listener_->on_device_found(device);
                            
                            if (!should_continue && !scan_continuous_) {
                                ESP_LOGI(TAG, "→ Stopping scan as requested");
                                stop_scan();
                            }
                        }
                    }
                    
                } else {
                    // ════════════════════════════════════════════════════
                    // DEVICE IN CACHE
                    // ════════════════════════════════════════════════════
                    
                    CachedDevice& cached = it->second;
                    
                    if (is_scan_rsp) {
                        // ✅ SCAN_RSP: Nur Name mergen, KEIN Callback!
                        
                        if (!name.empty() && cached.device.get_name().empty()) {
                            if (is_interesting) {
                                ESP_LOGI(TAG, "→ SCAN_RSP: Updating name: '%s'", name.c_str());
                            }
                            cached.device.set_name(name);
                        }
                        
                        cached.has_scan_rsp = true;
                        cached.last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
                        
                        // ❌ KEIN CALLBACK bei SCAN_RSP!
                        
                    } else if (is_adv && device.get_service_datas().size() > 0) {
                        // ✅ ADV mit Service Data: SOFORT Callback!
                        
                        if (is_interesting) {
                            ESP_LOGI(TAG, "→ ADV with Service Data - processing immediately");
                        }
                        
                        // Name aus Cache übernehmen falls vorhanden
                        if (!cached.device.get_name().empty()) {
                            device.set_name(cached.device.get_name());
                        }
                        
                        // Cache aktualisieren
                        cached.device = device;  // ← ERSETZEN, nicht mergen!
                        cached.has_adv = true;
                        cached.last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
                        
                        // ✅ CALLBACK AUSLÖSEN!
                        if (listener_) {
                            bool should_continue = listener_->on_device_found(device);
                            
                            if (!should_continue && !scan_continuous_) {
                                ESP_LOGI(TAG, "→ Stopping scan");
                                stop_scan();
                            }
                        }
                        
                    } else {
                        // ✅ ADV ohne Service Data: Nur Cache aktualisieren
                        
                        if (is_interesting) {
                            ESP_LOGV(TAG, "→ ADV without Service Data - updating cache only");
                        }
                        
                        cached.has_adv = is_adv;
                        cached.last_seen = xTaskGetTickCount() * portTICK_PERIOD_MS;
                        
                        // Update RSSI wenn stärker
                        if (device.get_rssi() > cached.device.get_rssi()) {
                            cached.device.set_rssi(device.get_rssi());
                        }
                    }
                }
                
                // Cleanup (nur alle 1000 Events)
                static uint32_t cleanup_counter = 0;
                if (++cleanup_counter % 1000 == 0) {
                    cleanup_device_cache();
                }
                
            } else {
                // ════════════════════════════════════════════════════════
                // PASSIVE SCAN
                // ════════════════════════════════════════════════════════
                
                if (listener_) {
                    bool should_continue = listener_->on_device_found(device);
                    
                    if (!should_continue && !scan_continuous_) {
                        stop_scan();
                    }
                }
            }
            
            break;
        }
        
        case BLE_GAP_EVENT_DISC_COMPLETE: {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║  SCAN COMPLETE                    ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "Reason: %d", event->disc_complete.reason);
            
            scanning_ = false;
            
            // Process remaining cache
            if (!device_cache_.empty()) {
                ESP_LOGW(TAG, "⚠ %d devices in cache (incomplete)", 
                         device_cache_.size());
                
                // ✅ OPTIONAL: Verarbeite auch incomplete devices
                if (listener_) {
                    for (auto& pair : device_cache_) {
                        // Nur wenn Service Data vorhanden
                        if (pair.second.device.get_service_datas().size() > 0) {
                            listener_->on_device_found(pair.second.device);
                        }
                    }
                }
                
                device_cache_.clear();
            }
            
            if (scan_continuous_) {
                ESP_LOGI(TAG, "→ Continuous: Restarting...");
                vTaskDelay(pdMS_TO_TICKS(100));
                start_scan(scan_duration_);
            }
            
            ESP_LOGI(TAG, "");
            break;
        }
        
        default:
            ESP_LOGV(TAG, "GAP event: %d", event->type);
            break;
    }
    
    return 0;
}



// ════════════════════════════════════════════════════════════════════════
// Helper: Merge Device Data
// ════════════════════════════════════════════════════════════════════════

void SimpleBLEScanner::merge_device_data(
    SimpleBLEDevice& target, 
    const SimpleBLEDevice& source) {
    
    // ════════════════════════════════════════════════════════════════════
    // 1. Merge Name (prefer non-empty)
    // ════════════════════════════════════════════════════════════════════
    
    if (target.get_name().empty() && !source.get_name().empty()) {
        ESP_LOGI(TAG, "  → Merging name: '%s'", source.get_name().c_str());
        target.set_name(source.get_name());
    }
    
    // ════════════════════════════════════════════════════════════════════
    // 2. Service Data: DO NOT MERGE!
    // ════════════════════════════════════════════════════════════════════
    
    // ✅ CRITICAL DESIGN DECISION:
    //    BTHome (and similar protocols) send the CURRENT state in EVERY advertisement.
    //    We should NOT cache/merge Service Data because:
    //    
    //    1. Each ADV contains the LATEST state (e.g., Window OPEN/CLOSED)
    //    2. Merging would accumulate old + new data → Parser confusion
    //    3. The callback on_device_found() is called for EACH event
    //    4. The device object in the callback contains THIS event's data
    //    
    //    Therefore: Skip Service Data merging entirely!
    //    The current Service Data in 'target' represents the LATEST event.
    
    const auto& source_sd = source.get_service_datas();
    if (!source_sd.empty()) {
        ESP_LOGV(TAG, "  → Service Data present in source (%d items)", source_sd.size());
        ESP_LOGV(TAG, "     Strategy: NOT merging (use current event data only)");
        
        // ✅ NO MERGE - Service Data already in target from current event!
    }
    
    // ════════════════════════════════════════════════════════════════════
    // 3. Update RSSI (prefer stronger signal)
    // ════════════════════════════════════════════════════════════════════
    
    if (source.get_rssi() > target.get_rssi()) {
        ESP_LOGI(TAG, "  → Updating RSSI: %d → %d dBm",
                 target.get_rssi(), source.get_rssi());
        target.set_rssi(source.get_rssi());
    }
}



// ════════════════════════════════════════════════════════════════════════
// Helper: Cleanup Device Cache
// ════════════════════════════════════════════════════════════════════════

void SimpleBLEScanner::cleanup_device_cache()
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    for (auto it = device_cache_.begin(); it != device_cache_.end();)
    {
        if (now - it->second.last_seen > cache_timeout_ms_)
        {
            ESP_LOGW(TAG, "  → Cache timeout for device - processing anyway");

            // Process incomplete device
            if (listener_)
            {
                listener_->on_device_found(it->second.device);
            }

            it = device_cache_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════
// ✅ WHITELIST MANAGEMENT
// ════════════════════════════════════════════════════════════════════════

bool SimpleBLEScanner::set_scan_whitelist(const std::vector<WhitelistEntry>& entries) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  CONFIGURE SCAN WHITELIST         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    if (scanning_) {
        ESP_LOGE(TAG, "✗ Cannot configure whitelist while scanning!");
        ESP_LOGE(TAG, "  Stop scan first");
        return false;
    }
    
    whitelist_addrs_.clear();
    
    if (entries.empty()) {
        ESP_LOGI(TAG, "→ Empty whitelist - disabling filter");
        whitelist_active_ = false;
        return true;
    }
    
    ESP_LOGI(TAG, "Adding %d address(es) to whitelist:", entries.size());
    
    for (const auto& entry : entries) {
        ble_addr_t addr;
        
        // ════════════════════════════════════════════════════════════════
        // Parse MAC address string
        // ════════════════════════════════════════════════════════════════
        
        const std::string& mac_str = entry.mac_address;
        
        if (mac_str.length() != 17) {
            ESP_LOGE(TAG, "✗ Invalid MAC format: %s", mac_str.c_str());
            continue;
        }
        
        bool parse_ok = true;
        for (int i = 0; i < 6; i++) {
            char hex[3] = {mac_str[i*3], mac_str[i*3+1], 0};
            char* endptr;
            long val = strtol(hex, &endptr, 16);
            
            if (*endptr != 0 || val < 0 || val > 255) {
                parse_ok = false;
                break;
            }
            
            // ✅ NimBLE erwartet Little-Endian (umgekehrte Reihenfolge)
            addr.val[5 - i] = (uint8_t)val;
        }
        
        if (!parse_ok) {
            ESP_LOGE(TAG, "✗ Failed to parse: %s", mac_str.c_str());
            continue;
        }
        
        // ════════════════════════════════════════════════════════════════
        // ✅ VERWENDE GESPEICHERTEN ADDRESS TYPE!
        // ════════════════════════════════════════════════════════════════
        
        addr.type = entry.address_type;
        
        whitelist_addrs_.push_back(addr);
        
        const char* type_str;
        switch (entry.address_type) {
            case BLE_ADDR_PUBLIC:  type_str = "PUBLIC"; break;
            case BLE_ADDR_RANDOM:  type_str = "RANDOM"; break;
            case BLE_ADDR_PUBLIC_ID: type_str = "PUBLIC_ID"; break;
            case BLE_ADDR_RANDOM_ID: type_str = "RANDOM_ID"; break;
            default: type_str = "UNKNOWN";
        }
        
        ESP_LOGI(TAG, "  ✓ %s (type: %s [%d])", 
                 mac_str.c_str(), type_str, entry.address_type);
    }
    
    if (whitelist_addrs_.empty()) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "✗ No valid addresses in whitelist!");
        whitelist_active_ = false;
        return false;
    }
    
    // ════════════════════════════════════════════════════════════════════
    // Apply whitelist to NimBLE
    // ════════════════════════════════════════════════════════════════════
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "→ Applying whitelist to NimBLE controller...");
    
    // Clear existing whitelist
    int rc = ble_gap_wl_set(nullptr, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "✗ Failed to clear whitelist: %d", rc);
        return false;
    }
    
    // Add addresses to whitelist
    rc = ble_gap_wl_set(whitelist_addrs_.data(), whitelist_addrs_.size());
    if (rc != 0) {
        ESP_LOGE(TAG, "✗ Failed to set whitelist: %d", rc);
        ESP_LOGE(TAG, "  Error code: %d", rc);
        
        // ✅ ERROR DETAIL LOGGING
        switch (rc) {
            case BLE_HS_EINVAL:
                ESP_LOGE(TAG, "  Reason: Invalid parameters");
                break;
            case BLE_HS_ENOMEM:
                ESP_LOGE(TAG, "  Reason: Out of memory");
                break;
            case BLE_HS_EBUSY:
                ESP_LOGE(TAG, "  Reason: BLE stack busy");
                break;
            default:
                ESP_LOGE(TAG, "  Reason: Unknown (%d)", rc);
        }
        
        return false;
    }
    
    whitelist_active_ = true;
    
    ESP_LOGI(TAG, "✓ Whitelist configured successfully");
    ESP_LOGI(TAG, "  %d address(es) active", whitelist_addrs_.size());
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "⚠ Next scan will ONLY see whitelisted devices!");
    ESP_LOGI(TAG, "");
    
    return true;
}


bool SimpleBLEScanner::clear_scan_whitelist() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  CLEAR SCAN WHITELIST             ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    if (scanning_) {
        ESP_LOGE(TAG, "✗ Cannot clear whitelist while scanning!");
        return false;
    }
    
    whitelist_addrs_.clear();
    whitelist_active_ = false;
    
    // Clear NimBLE whitelist
    int rc = ble_gap_wl_set(nullptr, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "✗ Failed to clear NimBLE whitelist: %d", rc);
        return false;
    }
    
    ESP_LOGI(TAG, "✓ Whitelist cleared");
    ESP_LOGI(TAG, "  Next scan will see ALL devices");
    ESP_LOGI(TAG, "");
    
    return true;
}

}  // namespace esp32_ble_simple
