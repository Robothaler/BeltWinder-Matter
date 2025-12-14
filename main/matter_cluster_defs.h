// matter_cluster_defs.h
// Clean, minimal, conflict-free definitions for custom cluster
#pragma once

#include <cstdint>

// === CUSTOM CLUSTER ID ===
// Must be in private range: 0xFFF1–0xFFFE (simple) or 0xFFF10000+ (with Vendor ID)
// We use simple ID for minimal flash + no vendor registration
#define CLUSTER_ID_ROLLERSHUTTER_CONFIG 0xFFF2

// === ATTRIBUTE IDs ===
#define ATTR_ID_DIRECTION_INVERTED 0x0000  // Boolean – writable
#define ATTR_ID_DEVICE_IP          0x0001  // String – readable (IP address)

// === COMMAND IDs ===
#define CMD_ID_START_CALIBRATION   0x0001  // No payload – trigger calibration

// String-Längen für Attribute
#define DEVICE_IP_MAX_LENGTH       16      // "255.255.255.255" = 15 chars + \0

// Optional: Vendor ID (not used in code – keep for future)
// constexpr chip::VendorId VENDOR_ID = static_cast<chip::VendorId>(0xFFF1);