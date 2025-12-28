// device_naming.cpp

#include "device_naming.h"
#include "config.h"
#include <esp_log.h>
#include <ESPmDNS.h>

#include <esp_matter.h>
#include <esp_matter_attribute.h>
#include <app/server/Server.h>

static const char* TAG = "DeviceNaming";

using namespace esp_matter;
using namespace chip::app::Clusters;

extern uint16_t window_covering_endpoint_id;

// Predefined room list (German)
const char* DeviceNaming::ROOM_PRESETS[] = {
    "Wohnzimmer",
    "Schlafzimmer",
    "Kueche",
    "Bad",
    "Flur",
    "Buero",
    "Kinderzimmer",
    "Gaestezimmer",
    "Esszimmer",
    "Keller",
    "Garage",
    "Terrasse",
    "Balkon"
};

const int DeviceNaming::ROOM_PRESET_COUNT = 
    sizeof(ROOM_PRESETS) / sizeof(ROOM_PRESETS[0]);

DeviceNaming::DeviceNaming() {
    // Defaults
    _current.room = "Unbenannt";
    _current.type = "Fenster";
    _current.position = "";
}

bool DeviceNaming::load() {
    _prefs.begin("device_name", true);
    
    _current.room = _prefs.getString("room", "Unbenannt");
    _current.type = _prefs.getString("type", "Fenster");
    _current.position = _prefs.getString("position", "");
    
    _prefs.end();
    
    generateNames();
    
    ESP_LOGI(TAG, "Loaded device name from NVS:");
    ESP_LOGI(TAG, "  Room: %s", _current.room.c_str());
    ESP_LOGI(TAG, "  Type: %s", _current.type.c_str());
    ESP_LOGI(TAG, "  Position: %s", _current.position.c_str());
    ESP_LOGI(TAG, "  Hostname: %s", _current.hostname.c_str());
    ESP_LOGI(TAG, "  Matter Name: %s", _current.matterName.c_str());
    
    return true;
}

bool DeviceNaming::save(const String& room, const String& type, const String& position) {
    // Validate inputs
    if (!isValidRoom(room) || !isValidType(type)) {
        ESP_LOGE(TAG, "Invalid input: room='%s', type='%s'", 
                 room.c_str(), type.c_str());
        return false;
    }
    
    if (position.length() > 0 && !isValidPosition(position)) {
        ESP_LOGE(TAG, "Invalid position: '%s'", position.c_str());
        return false;
    }
    
    // Save to NVS
    _prefs.begin("device_name", false);
    _prefs.putString("room", room);
    _prefs.putString("type", type);
    _prefs.putString("position", position);
    _prefs.end();
    
    // Update current
    _current.room = room;
    _current.type = type;
    _current.position = position;
    
    generateNames();
    
    ESP_LOGI(TAG, "✓ Device name saved to NVS");
    ESP_LOGI(TAG, "  New Hostname: %s", _current.hostname.c_str());
    ESP_LOGI(TAG, "  New Matter Name: %s", _current.matterName.c_str());
    
    return true;
}

DeviceNaming::DeviceName DeviceNaming::getNames() {
    return _current;
}

void DeviceNaming::generateNames() {
    // ========================================================================
    // Hostname: BW-[Raum]-[Typ]-[Position]
    // ========================================================================
    
    String hostnameBase = "BW-" + sanitizeForHostname(_current.room) + 
                         "-" + sanitizeForHostname(_current.type);
    
    if (_current.position.length() > 0) {
        hostnameBase += "-" + sanitizeForHostname(_current.position);
    }
    
    _current.hostname = hostnameBase;
    
    // ========================================================================
    // Matter Name: [Raum] [Typ] [Position]
    // ========================================================================
    
    String matterBase = _current.room + " " + _current.type;
    
    if (_current.position.length() > 0) {
        matterBase += " " + _current.position;
    }
    
    // Truncate to 32 chars (Matter limit)
    if (matterBase.length() > 32) {
        matterBase = matterBase.substring(0, 32);
        ESP_LOGW(TAG, "Matter name truncated to 32 chars: %s", matterBase.c_str());
    }
    
    _current.matterName = matterBase;
    
    // ========================================================================
    // Display Name (für Web-UI)
    // ========================================================================
    
    _current.displayName = _current.hostname;
}

bool DeviceNaming::apply() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  APPLYING DEVICE NAME             ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    // ========================================================================
    // 1. Update mDNS Hostname
    // ========================================================================
    
    if (MDNS.begin(_current.hostname.c_str())) {
        ESP_LOGI(TAG, "✓ mDNS hostname set: %s.local", _current.hostname.c_str());
        
        // Registriere mDNS Service für Auto-Discovery
        MDNS.addService("beltwinder", "tcp", 80);
        MDNS.addServiceTxt("beltwinder", "tcp", "version", APP_VERSION);
        MDNS.addServiceTxt("beltwinder", "tcp", "room", _current.room.c_str());
        MDNS.addServiceTxt("beltwinder", "tcp", "type", _current.type.c_str());
        
    } else {
        ESP_LOGW(TAG, "⚠ mDNS failed to set hostname");
    }
    
    // ========================================================================
    // 2. Update Matter Device Name (Basic Information Cluster)
    // ========================================================================
    
    extern uint16_t window_covering_endpoint_id;
    
    if (window_covering_endpoint_id != 0) {
        ESP_LOGD(TAG, "→ Updating Matter device name...");
        
        // KORREKT: esp_matter_char_str für Strings
        esp_matter_attr_val_t name_val = 
            esp_matter_char_str(const_cast<char*>(_current.matterName.c_str()), 
                               _current.matterName.length() + 1);
        
        esp_err_t ret = attribute::update(
            window_covering_endpoint_id,
            BasicInformation::Id,
            BasicInformation::Attributes::NodeLabel::Id,
            &name_val
        );
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✓ Matter device name updated: %s", 
                     _current.matterName.c_str());
        } else {
            ESP_LOGW(TAG, "⚠ Failed to update Matter name: %s", 
                     esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "⚠ Window covering endpoint not yet initialized");
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ Device name applied");
    ESP_LOGI(TAG, "");
    
    return true;
}

// ============================================================================
// Validation Functions
// ============================================================================

bool DeviceNaming::isValidRoom(const String& room) {
    if (room.length() == 0 || room.length() > 20) {
        return false;
    }
    
    // Prüfe Zeichen - RICHTIG für UTF-8 Umlaute
    for (size_t i = 0; i < room.length(); i++) {
        unsigned char c = room[i];
        
        // Alphanumerisch OK
        if (isalnum(c)) continue;
        
        // Space OK
        if (c == ' ') continue;
        
        // UTF-8 Multi-Byte Zeichen (Umlaute) OK
        // Umlaute beginnen mit 0xC0 oder höher
        if (c >= 0xC0) {
            // Das ist ein UTF-8 Multi-Byte Zeichen
            // Überspringe die Folgebytes
            if ((c & 0xE0) == 0xC0) {
                // 2-Byte Zeichen (ä, ö, ü, etc.)
                i++; // Skip nächstes Byte
            } else if ((c & 0xF0) == 0xE0) {
                // 3-Byte Zeichen
                i += 2; // Skip nächste 2 Bytes
            } else if ((c & 0xF8) == 0xF0) {
                // 4-Byte Zeichen
                i += 3; // Skip nächste 3 Bytes
            }
            continue;
        }
        
        // Alles andere ist ungültig
        return false;
    }
    
    return true;
}

bool DeviceNaming::isValidType(const String& type) {
    return (type == "Fenster" || type == "Tuer");
}

bool DeviceNaming::isValidPosition(const String& position) {
    if (position.length() == 0) {
        return true;  // Optional field
    }
    
    return (position == "Links" || position == "Rechts" || 
            position == "Mitte" || position == "Oben" || position == "Unten");
}

// ============================================================================
// Sanitize for Hostname (mDNS-compatible)
// ============================================================================

String DeviceNaming::sanitizeForHostname(const String& input) {
    String output = input;
    
    // Replace German umlauts
    output.replace("ä", "ae");
    output.replace("ö", "oe");
    output.replace("ü", "ue");
    output.replace("Ä", "Ae");
    output.replace("Ö", "Oe");
    output.replace("Ü", "Ue");
    output.replace("ß", "ss");
    
    // Replace spaces with hyphens
    output.replace(" ", "-");
    
    // Remove any other special characters
    String sanitized = "";
    for (size_t i = 0; i < output.length(); i++) {
        char c = output[i];
        if (isalnum(c) || c == '-' || c == '_') {
            sanitized += c;
        }
    }
    
    return sanitized;
}
