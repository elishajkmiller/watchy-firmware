// Watchy side of the T-Deck <-> Watchy link (ESP-NOW). Once a minute the watch asks the T-Deck
// "anything for me?" and keeps the last few texts it gets back.
#pragma once

#include "WatchyLinkProto.h"
#include <Arduino.h>

#define WL_STORE 5 // texts kept on the watch

void wlInit();                     // start from a clean state (call after any reset)
int wlCheckin(uint8_t batteryPct, bool force = false); // ask the T-Deck; returns how many NEW texts arrived (force = ignore the back-off)
bool wlLastAnswered();             // did the T-Deck answer the last check-in
int wlCount();                     // texts stored (0..WL_STORE)
int wlUnread();                    // texts not yet read on the watch
const WlMsg *wlGet(int i);         // 0 = newest, up to wlCount()-1
void wlClearUnread();
