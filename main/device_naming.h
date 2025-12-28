// device_naming.h

#ifndef DEVICE_NAMING_H
#define DEVICE_NAMING_H

#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_log.h>
#include <ESPmDNS.h>

class DeviceNaming {
public:
    struct DeviceName {
        String room;           // z.B. "Wohnzimmer"
        String type;           // "Fenster" oder "Tuer"
        String position;       // "Links", "Rechts", "Mitte" oder ""
        
        // Computed names
        String hostname;       // "BW-Wohnzimmer-Fenster-Links"
        String matterName;     // "Wohnzimmer Fenster Links"
        String displayName;    // Für Web-UI
    };
    
    DeviceNaming();
    
    // Load from NVS
    bool load();
    
    // Save to NVS
    bool save(const String& room, const String& type, const String& position);
    
    // Get current names
    DeviceName getNames();
    
    // Apply to system (mDNS + Matter)
    bool apply();
    
    // Validate inputs
    static bool isValidRoom(const String& room);
    static bool isValidType(const String& type);
    static bool isValidPosition(const String& position);
    
    // Sanitize for hostname (remove umlauts, spaces, etc.)
    static String sanitizeForHostname(const String& input);
    
    // Predefined room list (can be extended)
    static const char* ROOM_PRESETS[];
    static const int ROOM_PRESET_COUNT;
    
private:
    Preferences _prefs;
    DeviceName _current;
    
    void generateNames();
};

#endif // DEVICE_NAMING_H
