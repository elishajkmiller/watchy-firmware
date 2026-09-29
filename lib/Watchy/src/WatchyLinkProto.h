// Message format for the T-Deck <-> Watchy link (ESP-NOW). Used by both firmwares; keep both copies identical.
#pragma once
#include <stdint.h>

#define WL_CHANNEL 1          // Wi-Fi channel both devices use for the link
#define WL_MAX_TEXT 96        // text bytes per message, including the terminating 0
#define WL_MAX_FROM 8         // sender name bytes, including the terminating 0
#define WL_FORWARD_CHANNEL 1  // Meshtastic channel index whose messages are forwarded (the "Private" channel)

static const uint8_t WL_WATCHY_MAC[6] = {0x48, 0xCA, 0x43, 0x54, 0x20, 0xE4};
static const uint8_t WL_TDECK_MAC[6] = {0x28, 0x37, 0x2F, 0x91, 0x20, 0x98};

enum { WL_TYPE_CHECKIN = 1, WL_TYPE_REPLY = 2 };

#pragma pack(push, 1)
struct WlHeader {
    uint8_t magic0, magic1; // 'W','L'
    uint8_t type;
    uint8_t ver; // 1
};

// watch -> T-Deck, once a minute: "anything for me?"
struct WlCheckin {
    WlHeader h;
    uint16_t session;   // the T-Deck session the watch last saw (0 if none)
    uint16_t ackId;     // newest message id the watch already has, in that session
    uint8_t batteryPct; // watch battery, 0-100
    uint8_t flags;
};

struct WlMsg {
    uint16_t id;
    char from[WL_MAX_FROM];
    char text[WL_MAX_TEXT];
};

// T-Deck -> watch: up to two waiting messages
struct WlReply {
    WlHeader h;
    uint16_t session; // random per T-Deck boot, so ids from an old boot are never confused with new ones
    uint8_t count;
    WlMsg msgs[2];
};
#pragma pack(pop)
