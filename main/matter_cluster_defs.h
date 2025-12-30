// matter_cluster_defs.h
// Clean, minimal, conflict-free definitions for custom cluster
#pragma once

#include <cstdint>

// === CUSTOM CLUSTER ID ===
// Must be in private range: 0xFFF1–0xFFFE (simple) or 0xFFF10000+ (with Vendor ID)
// We use simple ID for minimal flash + no vendor registration
#define CLUSTER_ID_ROLLERSHUTTER_CONFIG 0xFFF2

// === ATTRIBUTE IDs ===
#define ATTR_ID_DEVICE_IP          0x0001  // String – readable (IP address)

// === COMMAND IDs ===
static const uint32_t CMD_ID_START_CALIBRATION = 0x00;

// String-Längen für Attribute
#define DEVICE_IP_MAX_LENGTH       16      // "255.255.255.255" = 15 chars + \0

// Optional: Vendor ID (not used in code – keep for future)
// constexpr chip::VendorId VENDOR_ID = static_cast<chip::VendorId>(0xFFF1);

// ============================================================================
// Scene Cluster Configuration (Minimal Implementation)
// ============================================================================

// Scene Cluster ID (Matter Spec)
#define CLUSTER_ID_SCENES                       0x0005

// Scene Commands
#define CMD_ID_RECALL_SCENE                     0x05
#define CMD_ID_GET_SCENE_MEMBERSHIP             0x06

// Scene Attributes
#define ATTR_ID_SCENE_COUNT                     0x0000
#define ATTR_ID_CURRENT_SCENE                   0x0001
#define ATTR_ID_CURRENT_GROUP                   0x0002
#define ATTR_ID_SCENE_VALID                     0x0003
#define ATTR_ID_NAME_SUPPORT                    0x0004
#define ATTR_ID_LAST_CONFIGURED_BY              0x0005

// Scene ID Mapping für Rolladen
struct SceneMapping {
    uint8_t sceneId;
    uint8_t shutterPosition;  // 0-100%
    const char* description;
};

// Vordefinierte Scenes
static const SceneMapping SCENE_MAPPINGS[] = {
    {1, 0,   "Guten Morgen"},    // Scene 1 = Komplett offen
    {2, 25,  "Morning Light"},   // Scene 2 = 25% geschlossen
    {3, 50,  "Half Closed"},     // Scene 3 = Halb geschlossen
    {4, 75,  "Privacy"},         // Scene 4 = 75% geschlossen
    {5, 100, "Gute Nacht"},      // Scene 5 = Komplett geschlossen
    {6, 10,  "Ventilation"},     // Scene 6 = Leicht geöffnet
};

#define SCENE_MAPPING_COUNT (sizeof(SCENE_MAPPINGS) / sizeof(SceneMapping))
