#include "WatchyLink.h"
#include "WatchyLinkSecret.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

// Kept in RTC memory so it survives the watch's deep sleep. A magic number guards against
// stale bytes from an older firmware after a flash.
struct WlState {
    uint32_t magic;
    uint16_t session; // T-Deck session the ids belong to
    uint16_t ackId;   // newest id stored from that session
    uint8_t count;    // texts stored
    uint8_t unread;
    uint8_t misses;   // check-ins in a row with no reply
    uint8_t skipLeft; // minutes to skip before the next check-in (back-off when the T-Deck is away)
    WlMsg msgs[WL_STORE]; // oldest first, newest last
};

static const uint32_t WL_MAGIC = 0x574C0001;
RTC_DATA_ATTR static WlState wl;

static bool lastAnswered = false;
static volatile bool gotReply = false;
static WlReply replyBuf;
static volatile int replyLen = 0;

static void ensureState()
{
    if (wl.magic != WL_MAGIC || wl.count > WL_STORE)
        wlInit();
}

void wlInit()
{
    memset(&wl, 0, sizeof(wl));
    wl.magic = WL_MAGIC;
}

int wlCount()
{
    ensureState();
    return wl.count;
}

int wlUnread()
{
    ensureState();
    return wl.unread;
}

const WlMsg *wlGet(int i)
{
    ensureState();
    if (i < 0 || i >= wl.count)
        return nullptr;
    return &wl.msgs[wl.count - 1 - i];
}

void wlClearUnread()
{
    ensureState();
    wl.unread = 0;
}

static void onRecv(const uint8_t *mac, const uint8_t *data, int len)
{
    if (memcmp(mac, WL_TDECK_MAC, 6) != 0 || len < 8 || len > (int)sizeof(WlReply))
        return;
    memcpy(&replyBuf, data, len);
    replyLen = len;
    gotReply = true;
}

static void store(const WlMsg &m)
{
    if (wl.count == WL_STORE) { // full: drop the oldest
        memmove(&wl.msgs[0], &wl.msgs[1], (WL_STORE - 1) * sizeof(WlMsg));
        wl.count--;
    }
    wl.msgs[wl.count++] = m;
    if (wl.unread < 99)
        wl.unread++;
}

// One check-in / reply exchange. Returns the number of new texts, or -1 if no reply came.
static int exchange(uint8_t batteryPct)
{
    WlCheckin ci;
    memset(&ci, 0, sizeof(ci));
    ci.h.magic0 = 'W';
    ci.h.magic1 = 'L';
    ci.h.type = WL_TYPE_CHECKIN;
    ci.h.ver = 1;
    ci.session = wl.session;
    ci.ackId = wl.ackId;
    ci.batteryPct = batteryPct;

    gotReply = false;
    esp_err_t sr = esp_now_send(WL_TDECK_MAC, (const uint8_t *)&ci, sizeof(ci));
    Serial.printf("link: send rc=%d\n", (int)sr);
    if (sr != ESP_OK)
        return -1;
    int waited = 0;
    for (; waited < 400 && !gotReply; waited += 5)
        delay(5);
    Serial.printf("link: %s after %d ms\n", gotReply ? "reply" : "no reply", waited);
    if (!gotReply)
        return -1;

    const WlReply &r = replyBuf;
    if (r.h.magic0 != 'W' || r.h.magic1 != 'L' || r.h.type != WL_TYPE_REPLY || r.h.ver != 1 || r.count > 2)
        return 0;
    if (r.session != wl.session) { // the T-Deck restarted: its ids start over
        wl.session = r.session;
        wl.ackId = 0;
    }
    int fresh = 0;
    for (int i = 0; i < r.count; i++) {
        if (r.msgs[i].id > wl.ackId) {
            WlMsg m = r.msgs[i];
            m.from[WL_MAX_FROM - 1] = 0;
            m.text[WL_MAX_TEXT - 1] = 0;
            store(m);
            wl.ackId = m.id;
            fresh++;
        }
    }
    return fresh;
}

bool wlLastAnswered()
{
    return lastAnswered;
}

int wlCheckin(uint8_t batteryPct, bool force)
{
    ensureState();
    Serial.printf("link: checkin start (skipLeft=%d misses=%d)\n", wl.skipLeft, wl.misses);
    if (!force && wl.skipLeft > 0) { // the T-Deck was not answering: ask less often to save battery
        wl.skipLeft--;
        return 0;
    }
    WiFi.mode(WIFI_STA);
    Serial.println("link: wifi STA on");
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(WL_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    Serial.println("link: channel set");
    if (esp_now_init() != ESP_OK) {
        Serial.println("link: esp_now_init FAILED");
        WiFi.mode(WIFI_OFF);
        return 0;
    }
    Serial.println("link: esp_now ready");
    esp_now_set_pmk(WL_PMK);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, WL_TDECK_MAC, 6);
    peer.channel = WL_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    memcpy(peer.lmk, WL_LMK, 16);
    esp_err_t pr = esp_now_add_peer(&peer);
    Serial.printf("link: add_peer rc=%d\n", (int)pr);
    esp_now_register_recv_cb(onRecv);

    int total = 0;
    bool answered = false;
    for (int round = 0; round < 3; round++) { // each reply carries at most two texts; the next check-in confirms them
        int n = exchange(batteryPct);
        if (n < 0)
            break; // no reply: the T-Deck is not listening right now
        answered = true;
        total += n;
        if (n == 0)
            break; // nothing more waiting
    }
    lastAnswered = answered;
    if (answered) {
        wl.misses = 0;
        wl.skipLeft = 0;
    } else if (!force) {
        if (wl.misses < 250)
            wl.misses++;
        // every minute at first, then every 2 minutes, then every 4 minutes at most
        wl.skipLeft = wl.misses < 2 ? 0 : (wl.misses < 4 ? 1 : 3);
    }

    Serial.printf("link: done total=%d answered=%d\n", total, (int)answered);
    esp_now_deinit();
    Serial.println("link: esp_now deinit");
    WiFi.mode(WIFI_OFF);
    Serial.println("link: wifi off");
    return total;
}
