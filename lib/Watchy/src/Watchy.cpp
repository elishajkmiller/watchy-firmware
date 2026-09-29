#include "Watchy.h"
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Preferences.h>
#include "esp_private/esp_clk.h"

#define DBG(...) Serial.printf(__VA_ARGS__) // boot log over USB; never prints the Wi-Fi password

// Our own copy of the Wi-Fi login, kept in flash so it survives every reset. RTC variables
// (like WIFI_CONFIGURED) are cleared on any reset except deep sleep, so they can't hold it.
static void saveWifiCreds(const String &ssid, const String &pass) {
  Preferences prefs;
  prefs.begin("watchy", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

#ifdef ARDUINO_ESP32S3_DEV
  Watchy32KRTC Watchy::RTC;
  #define ACTIVE_LOW 0
#else
  WatchyRTC Watchy::RTC;
  #define ACTIVE_LOW 1
#endif
GxEPD2_BW<WatchyDisplay, WatchyDisplay::HEIGHT> Watchy::display(
    WatchyDisplay{});

RTC_DATA_ATTR int guiState;
RTC_DATA_ATTR int menuIndex;
RTC_DATA_ATTR BMA423 sensor;
RTC_DATA_ATTR bool WIFI_CONFIGURED;
RTC_DATA_ATTR bool BLE_CONFIGURED;
RTC_DATA_ATTR weatherData currentWeather;
RTC_DATA_ATTR int weatherIntervalCounter = -1;
RTC_DATA_ATTR uint8_t weatherQuickRetries = 0; // one fast retry after a failed fetch
RTC_DATA_ATTR long gmtOffset = 0;
RTC_DATA_ATTR bool alreadyInMenu         = true;
RTC_DATA_ATTR bool USB_PLUGGED_IN = false;
RTC_DATA_ATTR tmElements_t bootTime;
RTC_DATA_ATTR uint32_t lastIPAddress;
RTC_DATA_ATTR char lastSSID[30];

// The 3-day forecast (Down button on the clock): fetched when opened if the copy is old, kept in RTC memory.
struct FcDay {
  int16_t hi, lo;
  int16_t code;  // OpenWeatherMap condition id around midday
  uint8_t pop;   // chance of rain/snow, percent
  uint8_t wday;  // 1 = Sunday
  uint8_t day;   // day of month
};
RTC_DATA_ATTR FcDay fcDays[3];
RTC_DATA_ATTR uint8_t fcCount    = 0; // 0 = nothing cached
RTC_DATA_ATTR uint32_t fcFetchedAt = 0; // local time (makeTime of the watch clock) of the last good fetch
RTC_DATA_ATTR uint8_t fcFetchDay = 0;

// The last known clock and the sunrise/sunset times live in flash, so a reset (which clears
// RTC memory) does not lose them. The clock is restored from the RTC timer, which keeps
// counting through a reset, so the time that passed is added back.
RTC_DATA_ATTR bool clockTrusted = false; // set once the clock was synced or restored

// Alarm: the settings (mode/time/offset/enabled) live in flash (see save/restoreState below) so they survive a
// reset; the rest is runtime state that only needs to survive deep sleep.
RTC_DATA_ATTR bool alarmEnabled       = false;
RTC_DATA_ATTR uint8_t alarmMode       = 0; // 0 fixed time, 1 relative to sunrise
RTC_DATA_ATTR uint8_t alarmHour       = 7;
RTC_DATA_ATTR uint8_t alarmMinute     = 0;
RTC_DATA_ATTR int16_t alarmSunOffset  = 0;  // minutes from sunrise: 0 = at sunrise, -15 = 15 min before
RTC_DATA_ATTR bool alarmFiredToday    = false;
RTC_DATA_ATTR int8_t alarmFiredDay    = -1; // day of month the alarm last went off (or was snoozed through)
RTC_DATA_ATTR bool alarmRinging       = false; // true only while _startAlarmRinging()'s loop is running
RTC_DATA_ATTR bool alarmSnoozeActive  = false;
RTC_DATA_ATTR int16_t alarmSnoozeTargetMin = -1; // minutes since midnight to ring again, or -1

// Local minutes-of-day for a UTC sunrise/sunset reading (mirrors the watch face's own copy of this: kept
// separate so this file does not depend on the face implementation).
static int sunLocalMinutes(tmElements_t utc, long gmtOff) {
  tmElements_t local;
  breakTime(makeTime(utc) + gmtOff, local);
  return local.Hour * 60 + local.Minute;
}
static bool sunDataValid(tmElements_t rise, tmElements_t set) {
  return makeTime(rise) > 86400UL * 365 && makeTime(set) > 86400UL * 365; // 0 means never fetched
}
// The alarm's target minute-of-day, or -1 when it cannot be worked out yet (sunrise mode before the first
// weather fetch of the day).
static int alarmTargetMinutes(uint8_t mode, uint8_t hour, uint8_t minute, int16_t sunOffset,
                               const weatherData &weather, long gmtOff) {
  if (mode == 0) {
    return hour * 60 + minute;
  }
  if (!sunDataValid(weather.sunrise, weather.sunset)) {
    return -1;
  }
  int t = sunLocalMinutes(weather.sunrise, gmtOff) + sunOffset;
  return ((t % 1440) + 1440) % 1440;
}

static void saveState() {
  Preferences prefs;
  prefs.begin("watchy", false);
  prefs.putLong("gmt", gmtOffset);
  tmElements_t rise = currentWeather.sunrise;
  tmElements_t set  = currentWeather.sunset;
  if (makeTime(rise) > 86400UL * 365 && makeTime(set) > 86400UL * 365) {
    prefs.putULong("rise", makeTime(rise));
    prefs.putULong("set", makeTime(set));
  }
  if (clockTrusted) {
    prefs.putULong("wall", (uint32_t)time(nullptr)); // wall-clock time, as the watch keeps it
    prefs.putULong64("rtcus", esp_clk_rtc_time());
  }
  prefs.putBool("almOn", alarmEnabled);
  prefs.putUChar("almMode", alarmMode);
  prefs.putUChar("almHr", alarmHour);
  prefs.putUChar("almMin", alarmMinute);
  prefs.putShort("almOfs", alarmSunOffset);
  prefs.end();
}

static void restoreState() {
  Preferences prefs;
  prefs.begin("watchy", true);
  if (prefs.isKey("gmt")) {
    gmtOffset = prefs.getLong("gmt", gmtOffset);
  }
  if (prefs.isKey("rise") && prefs.isKey("set")) {
    breakTime((time_t)prefs.getULong("rise", 0), currentWeather.sunrise);
    breakTime((time_t)prefs.getULong("set", 0), currentWeather.sunset);
  }
  if (prefs.isKey("wall") && prefs.isKey("rtcus")) {
    uint32_t wall    = prefs.getULong("wall", 0);
    uint64_t savedUs = prefs.getULong64("rtcus", 0);
    uint64_t nowUs   = esp_clk_rtc_time();
    DBG("restore: saved wall=%lu savedUs=%llu nowUs=%llu sysclock=%ld\n", (unsigned long)wall, savedUs, nowUs, (long)time(nullptr));
    if (nowUs >= savedUs) {
      wall += (nowUs - savedUs) / 1000000ULL; // RTC kept counting: add the time that passed
    }
    // config() starts the clock from an empty date, which is garbage (it reads 6:28 in 2036), so
    // never compare against it. The clock can never be earlier than the moment this firmware was
    // built, so a saved time before that (like a stray 2021 date) is garbage too: ignore it.
    struct tm buildTm;
    memset(&buildTm, 0, sizeof(buildTm));
    strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &buildTm);
    const uint32_t buildEpoch = (uint32_t)mktime(&buildTm);
    if (wall >= buildEpoch) {
      struct timeval tv;
      tv.tv_sec  = wall;
      tv.tv_usec = 0;
      settimeofday(&tv, NULL);
    }
    clockTrusted = true;
    DBG("restore: clock now %ld\n", (long)time(nullptr));
  } else {
    DBG("restore: no saved clock in flash\n");
  }
  alarmEnabled   = prefs.getBool("almOn", false);
  alarmMode      = prefs.getUChar("almMode", 0);
  alarmHour      = prefs.getUChar("almHr", 7);
  alarmMinute    = prefs.getUChar("almMin", 0);
  alarmSunOffset = prefs.getShort("almOfs", 0);
  prefs.end();
}

void Watchy::init(String datetime) {
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause(); // get wake up reason
  #ifdef ARDUINO_ESP32S3_DEV
    Wire.begin(WATCHY_V3_SDA, WATCHY_V3_SCL);     // init i2c
  #else
    Wire.begin(SDA, SCL);                         // init i2c
  #endif
  RTC.init();
  Serial.begin(115200);
  if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER && wakeup_reason != ESP_SLEEP_WAKEUP_EXT1 &&
      wakeup_reason != ESP_SLEEP_WAKEUP_EXT0) {
    delay(2500); // reset only: lets the USB port come up for the boot log and the radio settle
  }
  DBG("\n== init: wake=%d reset=%d ==\n", (int)wakeup_reason, (int)esp_reset_reason());
  // Init the display since is almost sure we will use it
  display.epd2.initWatchy();

  switch (wakeup_reason) {
  #ifdef ARDUINO_ESP32S3_DEV
  case ESP_SLEEP_WAKEUP_TIMER: // RTC Alarm
  #else
  case ESP_SLEEP_WAKEUP_EXT0: // RTC Alarm
  #endif
    RTC.read(currentTime);
    switch (guiState) {
    case WATCHFACE_STATE:
      // _evaluateAlarm() runs the whole ring (buzz, screen, snooze/dismiss) right here and does not
      // return until it is over, so the normal tick below is skipped for the minute it rang in.
      if (!_evaluateAlarm()) {
        showWatchFace(true); // partial updates on tick
        if (settings.vibrateOClock) {
          if (currentTime.Minute == 0) {
            // The RTC wakes us up once per minute
            vibMotor(75, 4);
          }
        }
      }
      break;
    case MAIN_MENU_STATE:
      // Return to watchface if in menu for more than one tick
      if (alreadyInMenu) {
        guiState = WATCHFACE_STATE;
        showWatchFace(false);
      } else {
        alreadyInMenu = true;
      }
      break;
    }
    break;
  case ESP_SLEEP_WAKEUP_EXT1: // button Press
    handleButtonPress();
    break;
  #ifdef ARDUINO_ESP32S3_DEV
  case ESP_SLEEP_WAKEUP_EXT0: // USB plug in
    pinMode(USB_DET_PIN, INPUT);
    USB_PLUGGED_IN = (digitalRead(USB_DET_PIN) == 1);
    if(guiState == WATCHFACE_STATE){
      RTC.read(currentTime);
      showWatchFace(true);
    }
    break;
  #endif
  default: // reset
    // After a flash, RTC memory can still hold bytes from the previous firmware, and this
    // build's variables may sit at different addresses. Never trust it: start from a clean state.
    guiState              = WATCHFACE_STATE;
    menuIndex             = 0;
    WIFI_CONFIGURED       = false;
    BLE_CONFIGURED        = false;
    new (&currentWeather) weatherData(); // value-initialised: zeroed, with a valid String
    weatherIntervalCounter = -1;
    weatherQuickRetries   = 0;
    fcCount               = 0; // RTC memory may hold stale bytes: forget any cached forecast
    gmtOffset             = 0;
    alreadyInMenu         = true;
    USB_PLUGGED_IN        = false;
    memset(&bootTime, 0, sizeof(bootTime));
    lastIPAddress         = 0;
    lastSSID[0]           = 0;
    clockTrusted          = false;
    // Alarm settings (enabled/mode/hour/minute/offset) come from flash below, in restoreState(); only the
    // runtime state needs resetting here.
    alarmFiredToday        = false;
    alarmFiredDay          = -1;
    alarmRinging           = false;
    alarmSnoozeActive      = false;
    alarmSnoozeTargetMin   = -1;
    wlInit();
    RTC.config(datetime);
    _bmaConfig();
    #ifdef ARDUINO_ESP32S3_DEV
    pinMode(USB_DET_PIN, INPUT);
    USB_PLUGGED_IN = (digitalRead(USB_DET_PIN) == 1);
    #endif    
    gmtOffset = settings.gmtOffset;
    restoreState(); // saved timezone, sun times and clock, if any
    RTC.read(currentTime);
    RTC.read(bootTime);
    DBG("boot: clock after restore %02d:%02d, gmtOffset=%ld\n", currentTime.Hour, currentTime.Minute, gmtOffset);
    weatherIntervalCounter = -1; // refetch weather now; this also re-syncs the clock over WiFi
    showWatchFace(false); // full update on reset
    vibMotor(75, 4);
    // For some reason, seems to be enabled on first boot
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    break;
  }
  deepSleep();
}
void Watchy::deepSleep() {
  display.hibernate();
  RTC.clearAlarm();        // resets the alarm flag in the RTC
  #ifdef ARDUINO_ESP32S3_DEV
  esp_sleep_enable_ext0_wakeup((gpio_num_t)USB_DET_PIN, USB_PLUGGED_IN ? LOW : HIGH); //// enable deep sleep wake on USB plug in/out
  rtc_gpio_set_direction((gpio_num_t)USB_DET_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)USB_DET_PIN);

  esp_sleep_enable_ext1_wakeup(
      BTN_PIN_MASK,
      ESP_EXT1_WAKEUP_ANY_LOW); // enable deep sleep wake on button press
  rtc_gpio_set_direction((gpio_num_t)UP_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)UP_BTN_PIN);

  rtc_clk_32k_enable(true);
  //rtc_clk_slow_freq_set(RTC_SLOW_FREQ_32K_XTAL);
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  int secToNextMin = 60 - timeinfo.tm_sec;
  esp_sleep_enable_timer_wakeup(secToNextMin * uS_TO_S_FACTOR);
  #else
  // Set GPIOs 0-39 to input to avoid power leaking out
  const uint64_t ignore = 0b11110001000000110000100111000010; // Ignore some GPIOs due to resets
  for (int i = 0; i < GPIO_NUM_MAX; i++) {
    if ((ignore >> i) & 0b1)
      continue;
    pinMode(i, INPUT);
  }
  esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_INT_PIN,
                               0); // enable deep sleep wake on RTC interrupt
  esp_sleep_enable_ext1_wakeup(
      BTN_PIN_MASK,
      ESP_EXT1_WAKEUP_ANY_HIGH); // enable deep sleep wake on button press
  #endif
  esp_deep_sleep_start();
}

void Watchy::handleButtonPress() {
  uint64_t wakeupBit = esp_sleep_get_ext1_wakeup_status();
  // Note: a ringing alarm is not handled here. _startAlarmRinging() runs its own loop and reads the
  // buttons directly, so it never leaves the device in deep sleep waiting for a button-wake while ringing.

  // Menu Button
  if (wakeupBit & MENU_BTN_MASK) {
    if (guiState ==
        WATCHFACE_STATE) { // enter menu state if coming from watch face
      showMenu(menuIndex, false);
    } else if (guiState ==
               MAIN_MENU_STATE) { // if already in menu, then select menu item
      switch (menuIndex) {
      case 0:
        showAbout();
        break;
      case 1:
        showAccelerometer();
        break;
      case 2:
        setTime();
        break;
      case 3:
        showSync();
        break;
      case 4:
        setAlarm();
        break;
      case 5:
        showMessages();
        break;
      case 6:
        showLinkTest();
        break;
      case 7:
        showBuzzer();
        break;
      case 8:
        showUsbMode();
        break;
      default:
        break;
      }
    } /*else if (guiState == FW_UPDATE_STATE) {
      updateFWBegin();
    }*/
  }
  // Back Button
  else if (wakeupBit & BACK_BTN_MASK) {
    if (guiState == MAIN_MENU_STATE) { // exit to watch face if already in menu
      RTC.read(currentTime);
      showWatchFace(false);
    } else if (guiState == APP_STATE) {
      showMenu(menuIndex, false); // exit to menu if already in app
    } else if (guiState == FW_UPDATE_STATE) {
      showMenu(menuIndex, false); // exit to menu if already in app
    } else if (guiState == WATCHFACE_STATE) {
      return;
    }
  }
  // Up Button
  else if (wakeupBit & UP_BTN_MASK) {
    if (guiState == MAIN_MENU_STATE) { // increment menu index
      menuIndex--;
      if (menuIndex < 0) {
        menuIndex = MENU_LENGTH - 1;
      }
      showMenu(menuIndex, true);
    } else if (guiState == WATCHFACE_STATE) {
      return;
    }
  }
  // Down Button
  else if (wakeupBit & DOWN_BTN_MASK) {
    if (guiState == MAIN_MENU_STATE) { // decrement menu index
      menuIndex++;
      if (menuIndex > MENU_LENGTH - 1) {
        menuIndex = 0;
      }
      showMenu(menuIndex, true);
    } else if (guiState == WATCHFACE_STATE) {
      showForecast(); // second face: Down again (or Back) returns to the clock
      return;
    }
  }

  /***************** fast menu *****************/
  bool timeout     = false;
  long lastTimeout = millis();
  pinMode(MENU_BTN_PIN, INPUT);
  pinMode(BACK_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(DOWN_BTN_PIN, INPUT);
  while (!timeout) {
    if (millis() - lastTimeout > 5000) {
      timeout = true;
    } else {
      if (digitalRead(MENU_BTN_PIN) == ACTIVE_LOW) {
        lastTimeout = millis();
        if (guiState ==
            MAIN_MENU_STATE) { // if already in menu, then select menu item
          switch (menuIndex) {
          case 0:
            showAbout();
            break;
          case 1:
            showAccelerometer();
            break;
          case 2:
            setTime();
            break;
          case 3:
            showSync();
            break;
          case 4:
            setAlarm();
            break;
          case 5:
            showMessages();
            break;
          case 6:
            showLinkTest();
            break;
          case 7:
            showBuzzer();
            break;
          case 8:
            showUsbMode();
            break;
          default:
            break;
          }
        }/* else if (guiState == FW_UPDATE_STATE) {
          updateFWBegin();
        }*/
      } else if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
        lastTimeout = millis();
        if (guiState ==
            MAIN_MENU_STATE) { // exit to watch face if already in menu
          RTC.read(currentTime);
          showWatchFace(false);
          break; // leave loop
        } else if (guiState == APP_STATE) {
          showMenu(menuIndex, false); // exit to menu if already in app
        } else if (guiState == FW_UPDATE_STATE) {
          showMenu(menuIndex, false); // exit to menu if already in app
        }
      } else if (digitalRead(UP_BTN_PIN) == ACTIVE_LOW) {
        lastTimeout = millis();
        if (guiState == MAIN_MENU_STATE) { // increment menu index
          menuIndex--;
          if (menuIndex < 0) {
            menuIndex = MENU_LENGTH - 1;
          }
          showFastMenu(menuIndex);
        }
      } else if (digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW) {
        lastTimeout = millis();
        if (guiState == MAIN_MENU_STATE) { // decrement menu index
          menuIndex++;
          if (menuIndex > MENU_LENGTH - 1) {
            menuIndex = 0;
          }
          showFastMenu(menuIndex);
        }
      }
    }
  }
}

void Watchy::showMenu(byte menuIndex, bool partialRefresh) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);

  int16_t x1, y1;
  uint16_t w, h;
  int16_t yPos;

  const char *menuItems[] = {
      "About Watchy", "Show Accelerometer", "Set Time", "Sync",
      "Alarm",        "Messages",           "Link Test", "Buzzer", "USB Flash Mode"};
  int start = menuIndex < MENU_VISIBLE ? 0 : menuIndex - MENU_VISIBLE + 1;
  for (int i = start; i < MENU_LENGTH && i < start + MENU_VISIBLE; i++) {
    yPos = MENU_HEIGHT + (MENU_HEIGHT * (i - start));
    display.setCursor(0, yPos);
    if (i == menuIndex) {
      display.getTextBounds(menuItems[i], 0, yPos, &x1, &y1, &w, &h);
      display.fillRect(x1 - 1, y1 - 10, 200, h + 15, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
      display.println(menuItems[i]);
    } else {
      display.setTextColor(GxEPD_BLACK);
      display.println(menuItems[i]);
    }
  }

  display.display(partialRefresh);

  guiState = MAIN_MENU_STATE;
  alreadyInMenu = false;
}

void Watchy::showFastMenu(byte menuIndex) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);

  int16_t x1, y1;
  uint16_t w, h;
  int16_t yPos;

  const char *menuItems[] = {
      "About Watchy", "Show Accelerometer", "Set Time", "Sync",
      "Alarm",        "Messages",           "Link Test", "Buzzer", "USB Flash Mode"};
  int start = menuIndex < MENU_VISIBLE ? 0 : menuIndex - MENU_VISIBLE + 1;
  for (int i = start; i < MENU_LENGTH && i < start + MENU_VISIBLE; i++) {
    yPos = MENU_HEIGHT + (MENU_HEIGHT * (i - start));
    display.setCursor(0, yPos);
    if (i == menuIndex) {
      display.getTextBounds(menuItems[i], 0, yPos, &x1, &y1, &w, &h);
      display.fillRect(x1 - 1, y1 - 10, 200, h + 15, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
      display.println(menuItems[i]);
    } else {
      display.setTextColor(GxEPD_BLACK);
      display.println(menuItems[i]);
    }
  }

  display.display(true);

  guiState = MAIN_MENU_STATE;
}

void Watchy::showAbout() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0, 20);

  display.print("LibVer: ");
  display.println(WATCHY_LIB_VER);

  display.print("Rev: v");
  display.println(getBoardRevision());

  display.print("Batt: ");
  float voltage = getBatteryVoltage();
  display.print(voltage);
  display.println("V");

  #ifndef ARDUINO_ESP32S3_DEV
  display.print("Uptime: ");
  RTC.read(currentTime);
  time_t b = makeTime(bootTime);
  time_t c = makeTime(currentTime);
  int totalSeconds = c-b;
  //int seconds = (totalSeconds % 60);
  int minutes = (totalSeconds % 3600) / 60;
  int hours = (totalSeconds % 86400) / 3600;
  int days = (totalSeconds % (86400 * 30)) / 86400; 
  display.print(days);
  display.print("d");
  display.print(hours);
  display.print("h");
  display.print(minutes);
  display.println("m");  
  #endif
  
  if(WIFI_CONFIGURED){
    display.print("SSID: ");
    display.println(lastSSID);
    display.print("IP: ");
    display.println(IPAddress(lastIPAddress).toString());
  }else{
    display.println("WiFi Not Connected");
  }
  display.display(false); // full refresh

  guiState = APP_STATE;
}

void Watchy::showBuzz() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(70, 80);
  display.println("Buzz!");
  display.display(false); // full refresh
  vibMotor();
  showMenu(menuIndex, false);
}

void Watchy::vibMotor(uint8_t intervalMs, uint8_t length) {
  pinMode(VIB_MOTOR_PIN, OUTPUT);
  bool motorOn = false;
  for (int i = 0; i < length; i++) {
    motorOn = !motorOn;
    digitalWrite(VIB_MOTOR_PIN, motorOn);
    delay(intervalMs);
  }
}

// Called on the per-minute tick while the watch face is showing. Decides whether the alarm, or a snooze
// that was set earlier, should start ringing now. Returns true if it did (in which case it has already
// run the whole ring, start to finish, and redrawn the watch face when it was done).
bool Watchy::_evaluateAlarm() {
  if (alarmFiredDay != currentTime.Day) { // a new day: today's alarm has not gone off yet
    alarmFiredToday = false;
    alarmFiredDay   = currentTime.Day;
  }
  int nowMin = currentTime.Hour * 60 + currentTime.Minute;

  if (alarmSnoozeActive && nowMin == alarmSnoozeTargetMin) {
    alarmSnoozeActive = false;
    _startAlarmRinging();
    return true;
  }
  if (alarmEnabled && !alarmFiredToday) {
    int target = alarmTargetMinutes(alarmMode, alarmHour, alarmMinute, alarmSunOffset, currentWeather, gmtOffset);
    if (target >= 0 && target == nowMin) {
      alarmFiredToday = true;
      _startAlarmRinging();
      return true;
    }
  }
  return false;
}

// Rings right here, awake the whole time (not through deep sleep wakes), so it can pulse every second and
// answer a button within a fraction of a second, the way a real alarm clock does. Returns once dismissed,
// snoozed, or after 5 minutes unattended, having already put the watch face back on screen.
void Watchy::_startAlarmRinging() {
  alarmRinging = true;
  guiState     = ALARM_STATE;
  pinMode(MENU_BTN_PIN, INPUT);
  pinMode(BACK_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(DOWN_BTN_PIN, INPUT);
  pinMode(VIB_MOTOR_PIN, OUTPUT);

  _drawAlarmRinging();

  const unsigned long limitMs = 5UL * 60UL * 1000UL; // give up after 5 minutes unattended
  const unsigned long start   = millis();
  unsigned long lastBuzz      = 0;
  unsigned long lastRedraw    = millis();
  bool snoozed = false, dismissed = false;

  while (!snoozed && !dismissed && millis() - start < limitMs) {
    if (millis() - lastBuzz >= 1000) { // one strong pulse a second
      lastBuzz = millis();
      digitalWrite(VIB_MOTOR_PIN, HIGH);
      delay(150);
      digitalWrite(VIB_MOTOR_PIN, LOW);
    }
    if (millis() - lastRedraw >= 20000) { // keep the shown time from going stale on a long ring
      lastRedraw = millis();
      RTC.read(currentTime);
      _drawAlarmRinging();
    }
    if (digitalRead(MENU_BTN_PIN) == ACTIVE_LOW) {
      snoozed = true;
    } else if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW || digitalRead(UP_BTN_PIN) == ACTIVE_LOW ||
               digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW) {
      dismissed = true;
    }
    delay(20); // a light yield between reads, short enough that a press still feels instant
  }

  _stopAlarm();
  RTC.read(currentTime);
  if (snoozed) {
    alarmSnoozeActive    = true;
    alarmSnoozeTargetMin = ((currentTime.Hour * 60 + currentTime.Minute) + 9) % 1440;
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(10, 90);
    display.println("Snoozed 9 min");
    display.display(false);
    delay(1200);
  }
  guiState = WATCHFACE_STATE;
  showWatchFace(false);
}

void Watchy::_drawAlarmRinging() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(55, 30);
  display.println("ALARM");

  display.setFont(&DSEG7_Classic_Bold_53);
  display.setCursor(5, 110);
  if (currentTime.Hour < 10) {
    display.print("0");
  }
  display.print(currentTime.Hour);
  display.print(":");
  if (currentTime.Minute < 10) {
    display.print("0");
  }
  display.print(currentTime.Minute);

  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(0, 160);
  display.println("Menu = snooze 9m");
  display.println("Back = dismiss");
  display.display(false); // full refresh: this screen must be unmistakable
}

void Watchy::_stopAlarm() {
  alarmRinging = false;
  digitalWrite(VIB_MOTOR_PIN, LOW); // make sure it is not left buzzing
}

void Watchy::setTime() {

  guiState = APP_STATE;

  RTC.read(currentTime);

  #ifdef ARDUINO_ESP32S3_DEV
  uint8_t minute = currentTime.Minute;
  uint8_t hour   = currentTime.Hour;
  uint8_t day    = currentTime.Day;
  uint8_t month  = currentTime.Month;
  uint8_t year   = currentTime.Year;  
  #else
  int8_t minute = currentTime.Minute;
  int8_t hour   = currentTime.Hour;
  int8_t day    = currentTime.Day;
  int8_t month  = currentTime.Month;
  int8_t year   = tmYearToY2k(currentTime.Year);
  #endif

  int8_t setIndex = SET_HOUR;

  int8_t blink = 0;

  pinMode(DOWN_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(MENU_BTN_PIN, INPUT);
  pinMode(BACK_BTN_PIN, INPUT);

  display.setFullWindow();

  while (1) {

    if (digitalRead(MENU_BTN_PIN) == ACTIVE_LOW) {
      setIndex++;
      if (setIndex > SET_DAY) {
        break;
      }
    }
    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      if (setIndex != SET_HOUR) {
        setIndex--;
      }
    }

    blink = 1 - blink;

    if (digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW) {
      blink = 1;
      switch (setIndex) {
      case SET_HOUR:
        hour == 23 ? (hour = 0) : hour++;
        break;
      case SET_MINUTE:
        minute == 59 ? (minute = 0) : minute++;
        break;
      case SET_YEAR:
        year == 99 ? (year = 0) : year++;
        break;
      case SET_MONTH:
        month == 12 ? (month = 1) : month++;
        break;
      case SET_DAY:
        day == 31 ? (day = 1) : day++;
        break;
      default:
        break;
      }
    }

    if (digitalRead(UP_BTN_PIN) == ACTIVE_LOW) {
      blink = 1;
      switch (setIndex) {
      case SET_HOUR:
        hour == 0 ? (hour = 23) : hour--;
        break;
      case SET_MINUTE:
        minute == 0 ? (minute = 59) : minute--;
        break;
      case SET_YEAR:
        year == 0 ? (year = 99) : year--;
        break;
      case SET_MONTH:
        month == 1 ? (month = 12) : month--;
        break;
      case SET_DAY:
        day == 1 ? (day = 31) : day--;
        break;
      default:
        break;
      }
    }

    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&DSEG7_Classic_Bold_53);

    display.setCursor(5, 80);
    if (setIndex == SET_HOUR) { // blink hour digits
      display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
    }
    if (hour < 10) {
      display.print("0");
    }
    display.print(hour);

    display.setTextColor(GxEPD_BLACK);
    display.print(":");

    display.setCursor(108, 80);
    if (setIndex == SET_MINUTE) { // blink minute digits
      display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
    }
    if (minute < 10) {
      display.print("0");
    }
    display.print(minute);

    display.setTextColor(GxEPD_BLACK);

    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(45, 150);
    if (setIndex == SET_YEAR) { // blink minute digits
      display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
    }
    display.print(2000 + year);

    display.setTextColor(GxEPD_BLACK);
    display.print("/");

    if (setIndex == SET_MONTH) { // blink minute digits
      display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
    }
    if (month < 10) {
      display.print("0");
    }
    display.print(month);

    display.setTextColor(GxEPD_BLACK);
    display.print("/");

    if (setIndex == SET_DAY) { // blink minute digits
      display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
    }
    if (day < 10) {
      display.print("0");
    }
    display.print(day);
    display.display(true); // partial refresh
  }

  tmElements_t tm;
  tm.Month  = month;
  tm.Day    = day;
  #ifdef ARDUINO_ESP32S3_DEV
  tm.Year   = year;
  #else
  tm.Year   = y2kYearToTm(year);
  #endif
  tm.Hour   = hour;
  tm.Minute = minute;
  tm.Second = 0;

  RTC.set(tm);
  clockTrusted = true;
  saveState();

  showMenu(menuIndex, false);
}

// The Alarm menu screen: on/off, then either a fixed time or a spot relative to sunrise (0 = at sunrise,
// negative = that many minutes before). Same button pattern as setTime(): Menu moves to the next field
// (or saves and exits from the last one), Back moves to the previous field, Up/Down change the value.
#define ALM_FLD_ENABLED 0
#define ALM_FLD_MODE     1
#define ALM_FLD_VALUE1   2 // hour (fixed time) or offset (sunrise)
#define ALM_FLD_VALUE2   3 // minute (fixed time only)

// Blocks until no button is pressed (up to 2 s), then lets the contacts settle.
static void waitButtonsReleased() {
  uint32_t t0 = millis();
  while ((digitalRead(MENU_BTN_PIN) == ACTIVE_LOW || digitalRead(BACK_BTN_PIN) == ACTIVE_LOW ||
          digitalRead(UP_BTN_PIN) == ACTIVE_LOW || digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW) &&
         millis() - t0 < 2000) {
    delay(20);
  }
  delay(60);
}

void Watchy::setAlarm() {
  guiState = APP_STATE;

  bool enabled   = alarmEnabled;
  uint8_t mode   = alarmMode; // 0 fixed time, 1 sunrise
  uint8_t hour   = alarmHour;
  uint8_t minute = alarmMinute;
  int16_t offset = alarmSunOffset;

  int8_t field = ALM_FLD_ENABLED;
  int8_t blink = 0;

  pinMode(DOWN_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(MENU_BTN_PIN, INPUT);
  pinMode(BACK_BTN_PIN, INPUT);

  // The Menu press that opened this screen is usually still held. With the alarm OFF the first field is also
  // the last one, so that same press would read as "save and exit" and bounce straight back to the menu.
  waitButtonsReleased();

  display.setFullWindow();

  while (1) {
    int8_t lastField = !enabled ? ALM_FLD_ENABLED : (mode == 0 ? ALM_FLD_VALUE2 : ALM_FLD_VALUE1);

    if (digitalRead(MENU_BTN_PIN) == ACTIVE_LOW) {
      if (field >= lastField) {
        break; // save and exit
      }
      field++;
    }
    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      if (field > ALM_FLD_ENABLED) {
        field--;
      }
    }

    blink = 1 - blink;

    if (digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW || digitalRead(UP_BTN_PIN) == ACTIVE_LOW) {
      bool up = digitalRead(UP_BTN_PIN) == ACTIVE_LOW;
      blink   = 1;
      switch (field) {
      case ALM_FLD_ENABLED:
        enabled = !enabled;
        break;
      case ALM_FLD_MODE:
        mode = 1 - mode;
        break;
      case ALM_FLD_VALUE1:
        if (mode == 0) {
          hour = up ? (hour == 23 ? 0 : hour + 1) : (hour == 0 ? 23 : hour - 1);
        } else { // sunrise offset, 5 minute steps, -3h..+3h
          offset = up ? (offset >= 180 ? -180 : offset + 5) : (offset <= -180 ? 180 : offset - 5);
        }
        break;
      case ALM_FLD_VALUE2:
        minute = up ? (minute == 59 ? 0 : minute + 1) : (minute == 0 ? 59 : minute - 1);
        break;
      default:
        break;
      }
    }

    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(10, 25);
    display.println("Alarm");

    display.setCursor(10, 55);
    if (field == ALM_FLD_ENABLED) {
      display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
    }
    display.println(enabled ? "ON" : "OFF");
    display.setTextColor(GxEPD_BLACK);

    if (enabled) {
      display.setCursor(10, 80);
      if (field == ALM_FLD_MODE) {
        display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
      }
      display.println(mode == 0 ? "Fixed time" : "Sunrise");
      display.setTextColor(GxEPD_BLACK);

      if (mode == 0) {
        display.setFont(&DSEG7_Classic_Bold_53);
        display.setCursor(5, 160);
        if (field == ALM_FLD_VALUE1) {
          display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
        }
        if (hour < 10) {
          display.print("0");
        }
        display.print(hour);
        display.setTextColor(GxEPD_BLACK);
        display.print(":");
        if (field == ALM_FLD_VALUE2) {
          display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
        }
        if (minute < 10) {
          display.print("0");
        }
        display.print(minute);
        display.setTextColor(GxEPD_BLACK);
      } else {
        display.setFont(&FreeMonoBold9pt7b);
        display.setCursor(10, 110);
        if (field == ALM_FLD_VALUE1) {
          display.setTextColor(blink ? GxEPD_BLACK : GxEPD_WHITE);
        }
        if (offset == 0) {
          display.println("At sunrise");
        } else {
          display.print(offset < 0 ? -offset : offset);
          display.println(offset < 0 ? " min before" : " min after");
        }
        display.setTextColor(GxEPD_BLACK);

        display.setFont(&FreeMonoBold9pt7b);
        display.setCursor(10, 140);
        if (sunDataValid(currentWeather.sunrise, currentWeather.sunset)) {
          int riseMin = sunLocalMinutes(currentWeather.sunrise, gmtOffset);
          int atMin   = ((riseMin + offset) % 1440 + 1440) % 1440;
          char buf[20];
          snprintf(buf, sizeof(buf), "today ~%02d:%02d", atMin / 60, atMin % 60);
          display.println(buf);
        } else {
          display.println("(no sunrise yet:");
          display.println(" run Sync)");
        }
      }
    }

    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(0, 195);
    display.println("Menu=next Back=prev");
    display.display(true); // partial refresh
  }

  alarmEnabled    = enabled;
  alarmMode       = mode;
  alarmHour       = hour;
  alarmMinute     = minute;
  alarmSunOffset  = offset;
  alarmFiredToday = false; // an edit means "give it a fresh chance today"
  saveState();

  waitButtonsReleased(); // else the fast-menu loop in handleButtonPress sees Menu still held and reopens this
  showMenu(menuIndex, false);
}

// ---- 3-day forecast: the watch's second face ----------------------------------------------------------------
static const char *fcText(int id) {
  if (id >= 200 && id < 300) return "Storm";
  if (id >= 300 && id < 400) return "Drizzle";
  if (id == 511) return "Freezing rain";
  if (id >= 500 && id < 510) return id == 500 ? "Light rain" : "Rain";
  if (id >= 520 && id < 600) return "Showers";
  if (id >= 600 && id < 700) return "Snow";
  if (id == 781) return "Tornado";
  if (id >= 700 && id < 800) return "Fog/haze";
  if (id == 800) return "Clear";
  if (id == 801) return "Mostly clear";
  if (id == 802) return "Partly cloudy";
  if (id >= 803) return "Cloudy";
  return "?";
}

// Downloads the 5-day/3-hour forecast (32 slots = 4 days), and boils it down to hi/lo/condition/rain-chance
// for up to 3 local days. Returns true if fcDays was replaced.
static bool fetchForecast(Watchy &w, const watchySettings &st, long gmtOff, const tmElements_t &now) {
  if (!w.connectWiFi()) {
    DBG("forecast: no wifi\n");
    return false;
  }
  String url = st.weatherURL;
  url.replace("/weather?", "/forecast?");
  url += "&cnt=32";
  if (st.cityID != "") {
    url.replace("{cityID}", st.cityID);
  } else {
    url.replace("{lat}", st.lat);
    url.replace("{lon}", st.lon);
  }
  url.replace("{units}", st.weatherUnit);
  url.replace("{lang}", st.weatherLang);
  url.replace("{apiKey}", st.weatherAPIKey);

  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(8000);
  http.begin(url.c_str());
  int code = http.GET();
  DBG("forecast: http %d\n", code);
  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    JSONVar root   = JSON.parse(payload);
    DBG("forecast: %d bytes, free heap %u\n", (int)payload.length(), (unsigned)ESP.getFreeHeap());
    if (JSON.typeof(root) == "object" && JSON.typeof(root["list"]) == "array") {
      int n = root["list"].length();
      FcDay days[3];
      int cnt = 0, bestDist[3] = {99, 99, 99};
      int lastKey = -1;
      for (int i = 0; i < n; i++) {
        JSONVar e  = root["list"][i];
        long dt    = (long)(int)e["dt"];
        tmElements_t t;
        breakTime((time_t)(dt + gmtOff), t);
        int key = t.Month * 32 + t.Day;
        if (key != lastKey) {
          if (cnt == 3) break; // a fourth day: done
          days[cnt].hi   = -999;
          days[cnt].lo   = 999;
          days[cnt].pop  = 0;
          days[cnt].code = 800;
          days[cnt].wday = t.Wday;
          days[cnt].day  = t.Day;
          bestDist[cnt]  = 99;
          cnt++;
          lastKey = key;
        }
        FcDay &d = days[cnt - 1];
        int hi = (int)lround((double)e["main"]["temp_max"]);
        int lo = (int)lround((double)e["main"]["temp_min"]);
        if (hi > d.hi) d.hi = hi;
        if (lo < d.lo) d.lo = lo;
        int pop = (int)lround((double)e["pop"] * 100.0);
        if (pop > d.pop) d.pop = pop;
        int dist = abs((int)t.Hour - 14); // the slot nearest mid-afternoon stands for the day
        if (dist < bestDist[cnt - 1]) {
          bestDist[cnt - 1] = dist;
          d.code = (int)e["weather"][0]["id"];
        }
      }
      if (cnt > 0) {
        memcpy(fcDays, days, sizeof(days));
        fcCount     = cnt;
        fcFetchedAt = (uint32_t)makeTime(const_cast<tmElements_t &>(now));
        fcFetchDay  = now.Day;
        ok          = true;
      }
    } else {
      DBG("forecast: unexpected reply\n");
    }
  }
  http.end();
  WiFi.mode(WIFI_OFF);
  btStop();
  return ok;
}

void Watchy::showForecast() {
  guiState = APP_STATE;
  RTC.read(currentTime);
  pinMode(BACK_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(DOWN_BTN_PIN, INPUT);
  pinMode(MENU_BTN_PIN, INPUT);
  waitButtonsReleased(); // the Down press that opened this must not close it again straight away

  uint32_t nowSec = (uint32_t)makeTime(currentTime);
  bool stale      = fcCount == 0 || fcFetchDay != currentTime.Day || nowSec < fcFetchedAt || nowSec - fcFetchedAt > 3UL * 3600;
  bool failed     = false;
  if (stale) {
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(10, 90);
    display.println("Forecast");
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(10, 125);
    display.println("Loading...");
    display.display(false);
    failed = !fetchForecast(*this, settings, gmtOffset, currentTime);
  }

  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  static const char *const WD[] = {"???", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  if (fcCount == 0) {
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(10, 60);
    display.println("No forecast.");
    display.setCursor(10, 90);
    display.println(failed ? "Fetch failed." : "");
    display.setCursor(10, 115);
    display.println("Check Wi-Fi with");
    display.setCursor(10, 135);
    display.println("Sync in the menu.");
  } else {
    for (int i = 0; i < fcCount && i < 3; i++) {
      const FcDay &d = fcDays[i];
      int base       = 6 + i * 62;
      char label[8];
      if (d.day == currentTime.Day) {
        snprintf(label, sizeof(label), "Today");
      } else {
        snprintf(label, sizeof(label), "%s", WD[d.wday <= 7 ? d.wday : 0]);
      }
      char line[24];
      snprintf(line, sizeof(line), "%-6s%3d/%d", label, d.hi, d.lo);
      display.setFont(&FreeMonoBold12pt7b);
      display.setCursor(4, base + 20);
      display.print(line);
      char cond[32];
      if (d.pop >= 10) {
        snprintf(cond, sizeof(cond), "%s %d%%", fcText(d.code), d.pop);
      } else {
        snprintf(cond, sizeof(cond), "%s", fcText(d.code));
      }
      display.setFont(&FreeMonoBold9pt7b);
      display.setCursor(4, base + 42);
      display.print(cond);
      if (i < 2) {
        display.drawFastHLine(0, base + 54, 200, GxEPD_BLACK);
      }
    }
    if (failed) { // shows the old copy, and says so
      display.setFont(&FreeMonoBold9pt7b);
      display.setCursor(4, 198);
      display.print("(old, no update)");
    }
  }
  display.display(false);

  // Down (or Up / Back) returns to the clock; so does 30 seconds of nothing.
  unsigned long last = millis();
  while (millis() - last < 30000) {
    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW || digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW ||
        digitalRead(UP_BTN_PIN) == ACTIVE_LOW) {
      break;
    }
    delay(20);
  }
  waitButtonsReleased();
  RTC.read(currentTime);
  showWatchFace(false);
}

// A hand-held buzzer at full power: Menu turns it on/off, Up/Down pick the pattern. Each pattern is a list of
// alternating on/off times in ms, starting with "on" (a single entry means steadily on). The motor is switched
// off by itself after 10 minutes so it can't be left running, and the screen closes after 2 minutes of nothing.
#define BUZZ_MAX_ON_MS (10UL * 60 * 1000)
#define BUZZ_IDLE_MS   (2UL * 60 * 1000)

static const uint16_t BP_STEADY[]   = {1000};
static const uint16_t BP_PULSE[]    = {250, 250};
static const uint16_t BP_FAST[]     = {80, 80};
static const uint16_t BP_SLOW[]     = {600, 600};
static const uint16_t BP_HEART[]    = {110, 110, 110, 700};
static const uint16_t BP_TRIPLE[]   = {120, 120, 120, 120, 120, 900};
static const uint16_t BP_SOS[]      = {100, 100, 100, 100, 100, 300, 300, 100, 300, 100, 300, 300, 100, 100, 100, 100, 100, 1000};
static const uint16_t BP_LONGSHORT[] = {600, 200, 150, 600};
static const uint16_t BP_ESCALATE[] = {400, 400, 300, 300, 200, 200, 100, 100, 60, 60, 60, 60};

struct BuzzPattern {
  const char *name;
  const uint16_t *steps;
  uint8_t n;
};
static const BuzzPattern BUZZ_PATTERNS[] = {
    {"Steady", BP_STEADY, 1},       {"Pulse", BP_PULSE, 2},     {"Fast pulse", BP_FAST, 2},
    {"Slow pulse", BP_SLOW, 2},     {"Heartbeat", BP_HEART, 4}, {"Triple", BP_TRIPLE, 6},
    {"SOS", BP_SOS, 18},            {"Long-short", BP_LONGSHORT, 4}, {"Escalate", BP_ESCALATE, 12}};
#define BUZZ_PATTERN_COUNT (sizeof(BUZZ_PATTERNS) / sizeof(BUZZ_PATTERNS[0]))
RTC_DATA_ATTR uint8_t buzzPattern = 0; // checked on use: RTC memory can hold stale bytes after a flash

void Watchy::showBuzzer() {
  guiState = APP_STATE;
  if (buzzPattern >= BUZZ_PATTERN_COUNT) {
    buzzPattern = 0;
  }
  display.epd2.setBusyCallback(0); // no light sleep while the display refreshes: it would freeze the pattern timing

  pinMode(DOWN_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(MENU_BTN_PIN, INPUT);
  pinMode(BACK_BTN_PIN, INPUT);
  pinMode(VIB_MOTOR_PIN, OUTPUT);
  digitalWrite(VIB_MOTOR_PIN, LOW);

  bool on             = false;
  bool autoOff        = false;
  uint32_t onSince    = 0;
  uint32_t lastActive = millis();
  uint8_t step        = 0;
  uint32_t stepStart  = 0;

  auto restartPattern = [&]() {
    step      = 0;
    stepStart = millis();
    digitalWrite(VIB_MOTOR_PIN, on ? HIGH : LOW); // every pattern starts with "on"
  };
  auto draw = [&](bool partial) {
    const BuzzPattern &bp = BUZZ_PATTERNS[buzzPattern];
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(10, 25);
    display.println("Buzzer");
    display.setCursor(10, 70);
    display.setFont(&DSEG7_Classic_Bold_53);
    display.println(on ? "ON" : "OFF");
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(10, 100);
    display.print(buzzPattern + 1);
    display.print("/");
    display.print((int)BUZZ_PATTERN_COUNT);
    display.print(" ");
    display.println(bp.name);
    // one cycle of the pattern drawn as a bar: black = motor on, white = off
    uint32_t total = 0;
    for (int i = 0; i < bp.n; i++) {
      total += bp.steps[i];
    }
    display.drawRect(9, 111, 182, 24, GxEPD_BLACK);
    if (bp.n == 1) {
      display.fillRect(10, 112, 180, 22, GxEPD_BLACK);
    } else {
      uint32_t t = 0;
      for (int i = 0; i < bp.n; i += 2) { // even entries are the "on" stretches
        uint32_t startT = t;
        t += bp.steps[i];
        if (i + 1 < bp.n) {
          t += bp.steps[i + 1];
        }
        int x0 = 10 + (int)(startT * 180 / total);
        int x1 = 10 + (int)((startT + bp.steps[i]) * 180 / total);
        display.fillRect(x0, 112, max(1, x1 - x0), 22, GxEPD_BLACK);
      }
    }
    display.setCursor(10, 155);
    if (autoOff) {
      display.println("Auto-off (10 min)");
    } else if (on) {
      display.print("On ");
      display.print((millis() - onSince) / 1000);
      display.println(" s");
    }
    display.setCursor(0, 180);
    display.println("Menu=on/off Up/Dn");
    display.setCursor(0, 197);
    display.println("Back=exit");
    display.display(partial);
  };

  waitButtonsReleased(); // the Menu press that opened this screen must not toggle the motor
  draw(false);

  while (1) {
    bool changed = false;
    uint32_t now = millis();
    const BuzzPattern &bp = BUZZ_PATTERNS[buzzPattern];

    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      break;
    }
    if (digitalRead(MENU_BTN_PIN) == ACTIVE_LOW) {
      on         = !on;
      autoOff    = false;
      onSince    = now;
      lastActive = now;
      restartPattern();
      changed = true;
      waitButtonsReleased();
    } else if (digitalRead(UP_BTN_PIN) == ACTIVE_LOW) { // next pattern (wraps)
      buzzPattern = (buzzPattern + 1) % BUZZ_PATTERN_COUNT;
      lastActive  = now;
      restartPattern();
      changed = true;
      waitButtonsReleased();
    } else if (digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW) { // previous pattern (wraps)
      buzzPattern = (buzzPattern + BUZZ_PATTERN_COUNT - 1) % BUZZ_PATTERN_COUNT;
      lastActive  = now;
      restartPattern();
      changed = true;
      waitButtonsReleased();
    }

    if (on && now - onSince >= BUZZ_MAX_ON_MS) { // safety: never leave the motor running
      on      = false;
      autoOff = true;
      digitalWrite(VIB_MOTOR_PIN, LOW);
      changed = true;
    }
    if (on) {
      lastActive = now; // a running buzzer keeps the screen open
      if (bp.n > 1 && now - stepStart >= bp.steps[step]) { // move on to the next on/off stretch
        step      = (step + 1) % bp.n;
        stepStart = now;
        digitalWrite(VIB_MOTOR_PIN, step % 2 == 0 ? HIGH : LOW);
      }
    } else if (now - lastActive >= BUZZ_IDLE_MS) {
      break;
    }
    if (changed) {
      draw(true);
      if (on) { // the refresh took a while: start the pattern fresh so its rhythm is right
        restartPattern();
      }
    } else {
      delay(5);
    }
  }

  digitalWrite(VIB_MOTOR_PIN, LOW);
  waitButtonsReleased();
  showMenu(menuIndex, false);
}

void Watchy::showAccelerometer() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);

  Accel acc;

  long previousMillis = 0;
  long interval       = 200;

  guiState = APP_STATE;

  pinMode(BACK_BTN_PIN, INPUT);

  while (1) {

    unsigned long currentMillis = millis();

    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      break;
    }

    if (currentMillis - previousMillis > interval) {
      previousMillis = currentMillis;
      // Get acceleration data
      bool res          = sensor.getAccel(acc);
      uint8_t direction = sensor.getDirection();
      display.fillScreen(GxEPD_WHITE);
      display.setCursor(0, 30);
      if (res == false) {
        display.println("getAccel FAIL");
      } else {
        display.print("  X:");
        display.println(acc.x);
        display.print("  Y:");
        display.println(acc.y);
        display.print("  Z:");
        display.println(acc.z);

        display.setCursor(30, 130);
        switch (direction) {
        case DIRECTION_DISP_DOWN:
          display.println("FACE DOWN");
          break;
        case DIRECTION_DISP_UP:
          display.println("FACE UP");
          break;
        case DIRECTION_BOTTOM_EDGE:
          display.println("BOTTOM EDGE");
          break;
        case DIRECTION_TOP_EDGE:
          display.println("TOP EDGE");
          break;
        case DIRECTION_RIGHT_EDGE:
          display.println("RIGHT EDGE");
          break;
        case DIRECTION_LEFT_EDGE:
          display.println("LEFT EDGE");
          break;
        default:
          display.println("ERROR!!!");
          break;
        }
      }
      display.display(true); // full refresh
    }
  }

  showMenu(menuIndex, false);
}

void Watchy::showWatchFace(bool partialRefresh) {
  display.setFullWindow();
  // At this point it is sure we are going to update
  display.epd2.asyncPowerOn();
  if (clockTrusted && currentTime.Minute % 5 == 0) {
    saveState();
  }
  drawWatchFace();
  display.display(partialRefresh); // partial refresh
  guiState = WATCHFACE_STATE;
}

void Watchy::drawWatchFace() {
  display.setFont(&DSEG7_Classic_Bold_53);
  display.setCursor(5, 53 + 60);
  if (currentTime.Hour < 10) {
    display.print("0");
  }
  display.print(currentTime.Hour);
  display.print(":");
  if (currentTime.Minute < 10) {
    display.print("0");
  }
  display.println(currentTime.Minute);
}

weatherData Watchy::getWeatherData() {
  return _getWeatherData(settings.cityID, settings.lat, settings.lon,
    settings.weatherUnit, settings.weatherLang, settings.weatherURL,
    settings.weatherAPIKey, settings.weatherUpdateInterval);
}

weatherData Watchy::_getWeatherData(String cityID, String lat, String lon, String units, String lang,
                                   String url, String apiKey,
                                   uint8_t updateInterval) {
  currentWeather.isMetric = units == String("metric");
  if (weatherIntervalCounter < 0) { //-1 on first run, set to updateInterval
    weatherIntervalCounter = updateInterval;
  }
  if (weatherIntervalCounter >=
      updateInterval) { // only update if WEATHER_UPDATE_INTERVAL has elapsed
                        // i.e. 30 minutes
    bool fetchOk = false;
    if (connectWiFi()) {
      HTTPClient http; // Use Weather API for live data if WiFi is connected
      http.setConnectTimeout(3000); // 3 second max timeout
      String weatherQueryURL = url;
      if(cityID != ""){
        weatherQueryURL.replace("{cityID}", cityID);
      }else{
        weatherQueryURL.replace("{lat}", lat);
        weatherQueryURL.replace("{lon}", lon);
      }
      weatherQueryURL.replace("{units}", units);
      weatherQueryURL.replace("{lang}", lang);
      weatherQueryURL.replace("{apiKey}", apiKey);
      http.begin(weatherQueryURL.c_str());
      int httpResponseCode = http.GET();
      DBG("weather: http %d\n", httpResponseCode);
      if (httpResponseCode == 200) {
        fetchOk = true;
        String payload             = http.getString();
        JSONVar responseObject     = JSON.parse(payload);
        currentWeather.temperature = int(responseObject["main"]["temp"]);
        currentWeather.weatherConditionCode =
            int(responseObject["weather"][0]["id"]);
        currentWeather.weatherDescription =
		        JSONVar::stringify(responseObject["weather"][0]["main"]);
	      currentWeather.external = true;
		        breakTime((time_t)(int)responseObject["sys"]["sunrise"], currentWeather.sunrise);
		        breakTime((time_t)(int)responseObject["sys"]["sunset"], currentWeather.sunset);
        // sync NTP during weather API call and use timezone of lat & lon
        gmtOffset = int(responseObject["timezone"]);
        bool ntpOk = syncNTP(gmtOffset);
        DBG("ntp: %s, gmtOffset=%ld\n", ntpOk ? "ok" : "FAILED", gmtOffset);
        if (ntpOk) {
          clockTrusted = true;
        } else {
          fetchOk = false; // weather came through but the clock did not sync: try again soon
        }
        saveState();
      } else {
        // http error
      }
      http.end();
      // turn off radios
      WiFi.mode(WIFI_OFF);
      btStop();
    } else { // No WiFi, use internal temperature sensor
      uint8_t temperature = sensor.readTemperature(); // celsius
      if (!currentWeather.isMetric) {
        temperature = temperature * 9. / 5. + 32.; // fahrenheit
      }
      currentWeather.temperature          = temperature;
      currentWeather.weatherConditionCode = 800;
      currentWeather.external             = false;
    }
    if (!fetchOk && weatherQuickRetries < 1) {
      weatherQuickRetries++;
      weatherIntervalCounter = updateInterval - 1; // try again on the next minute
    } else {
      if (fetchOk) {
        weatherQuickRetries = 0;
      }
      weatherIntervalCounter = 0;
    }
  } else {
    weatherIntervalCounter++;
  }
  return currentWeather;
}

float Watchy::getBatteryVoltage() {
  #ifdef ARDUINO_ESP32S3_DEV
    return analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f * ADC_VOLTAGE_DIVIDER;
  #else
  if (RTC.rtcType == DS3231) {
    return analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f *
           2.0f; // Battery voltage goes through a 1/2 divider.
  } else {
    return analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f * 2.0f;
  }
  #endif
}

uint8_t Watchy::getBoardRevision() {
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  if(chip_info.model == CHIP_ESP32){ //Revision 1.0 - 2.0
    Wire.beginTransmission(0x68); //v1.0 has DS3231
    if (Wire.endTransmission() == 0){
      return 10;
    }
    delay(1);
    Wire.beginTransmission(0x51); //v1.5 and v2.0 have PCF8563
    if (Wire.endTransmission() == 0){
        pinMode(35, INPUT);
        if(digitalRead(35) == 0){
          return 20; //in rev 2.0, pin 35 is BTN 3 and has a pulldown
        }else{
          return 15; //in rev 1.5, pin 35 is the battery ADC
        }
    }
  }
  if(chip_info.model == CHIP_ESP32S3){ //Revision 3.0
    return 30;
  }
  return -1;
}

uint16_t Watchy::_readRegister(uint8_t address, uint8_t reg, uint8_t *data,
                               uint16_t len) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)address, (uint8_t)len);
  uint8_t i = 0;
  while (Wire.available()) {
    data[i++] = Wire.read();
  }
  return 0;
}

uint16_t Watchy::_writeRegister(uint8_t address, uint8_t reg, uint8_t *data,
                                uint16_t len) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(data, len);
  return (0 != Wire.endTransmission());
}

void Watchy::_bmaConfig() {

  if (sensor.begin(_readRegister, _writeRegister, delay) == false) {
    // fail to init BMA
    return;
  }

  // Accel parameter structure
  Acfg cfg;
  /*!
      Output data rate in Hz, Optional parameters:
          - BMA4_OUTPUT_DATA_RATE_0_78HZ
          - BMA4_OUTPUT_DATA_RATE_1_56HZ
          - BMA4_OUTPUT_DATA_RATE_3_12HZ
          - BMA4_OUTPUT_DATA_RATE_6_25HZ
          - BMA4_OUTPUT_DATA_RATE_12_5HZ
          - BMA4_OUTPUT_DATA_RATE_25HZ
          - BMA4_OUTPUT_DATA_RATE_50HZ
          - BMA4_OUTPUT_DATA_RATE_100HZ
          - BMA4_OUTPUT_DATA_RATE_200HZ
          - BMA4_OUTPUT_DATA_RATE_400HZ
          - BMA4_OUTPUT_DATA_RATE_800HZ
          - BMA4_OUTPUT_DATA_RATE_1600HZ
  */
  cfg.odr = BMA4_OUTPUT_DATA_RATE_100HZ;
  /*!
      G-range, Optional parameters:
          - BMA4_ACCEL_RANGE_2G
          - BMA4_ACCEL_RANGE_4G
          - BMA4_ACCEL_RANGE_8G
          - BMA4_ACCEL_RANGE_16G
  */
  cfg.range = BMA4_ACCEL_RANGE_2G;
  /*!
      Bandwidth parameter, determines filter configuration, Optional parameters:
          - BMA4_ACCEL_OSR4_AVG1
          - BMA4_ACCEL_OSR2_AVG2
          - BMA4_ACCEL_NORMAL_AVG4
          - BMA4_ACCEL_CIC_AVG8
          - BMA4_ACCEL_RES_AVG16
          - BMA4_ACCEL_RES_AVG32
          - BMA4_ACCEL_RES_AVG64
          - BMA4_ACCEL_RES_AVG128
  */
  cfg.bandwidth = BMA4_ACCEL_NORMAL_AVG4;

  /*! Filter performance mode , Optional parameters:
      - BMA4_CIC_AVG_MODE
      - BMA4_CONTINUOUS_MODE
  */
  cfg.perf_mode = BMA4_CONTINUOUS_MODE;

  // Configure the BMA423 accelerometer
  sensor.setAccelConfig(cfg);

  // Enable BMA423 accelerometer
  // Warning : Need to use feature, you must first enable the accelerometer
  // Warning : Need to use feature, you must first enable the accelerometer
  sensor.enableAccel();

  struct bma4_int_pin_config config;
  config.edge_ctrl = BMA4_LEVEL_TRIGGER;
  config.lvl       = BMA4_ACTIVE_HIGH;
  config.od        = BMA4_PUSH_PULL;
  config.output_en = BMA4_OUTPUT_ENABLE;
  config.input_en  = BMA4_INPUT_DISABLE;
  // The correct trigger interrupt needs to be configured as needed
  sensor.setINTPinConfig(config, BMA4_INTR1_MAP);

  struct bma423_axes_remap remap_data;
  remap_data.x_axis      = 1;
  remap_data.x_axis_sign = 0xFF;
  remap_data.y_axis      = 0;
  remap_data.y_axis_sign = 0xFF;
  remap_data.z_axis      = 2;
  remap_data.z_axis_sign = 0xFF;
  // Need to raise the wrist function, need to set the correct axis
  sensor.setRemapAxes(&remap_data);

  // Enable BMA423 isStepCounter feature
  sensor.enableFeature(BMA423_STEP_CNTR, true);
  // Enable BMA423 isTilt feature
  sensor.enableFeature(BMA423_TILT, true);
  // Enable BMA423 isDoubleClick feature
  sensor.enableFeature(BMA423_WAKEUP, true);

  // Reset steps
  sensor.resetStepCounter();

  // Turn on feature interrupt
  sensor.enableStepCountInterrupt();
  sensor.enableTiltInterrupt();
  // It corresponds to isDoubleClick interrupt
  sensor.enableWakeupInterrupt();
}

void Watchy::setupWifi() {
  display.epd2.setBusyCallback(0); // temporarily disable lightsleep on busy
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  wifiManager.setTimeout(WIFI_AP_TIMEOUT);
  wifiManager.setAPCallback(_configModeCallback);
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  if (!wifiManager.autoConnect(WIFI_AP_SSID)) { // WiFi setup failed
    display.println("Setup failed &");
    display.println("timed out!");
  } else {
    display.println("Connected to:");
    display.println(WiFi.SSID());
		display.println("Local IP:");
		display.println(WiFi.localIP());
    saveWifiCreds(WiFi.SSID(), WiFi.psk()); // keep it in flash across resets
    weatherIntervalCounter = -1; // Reset to force weather to be read again
    lastIPAddress = WiFi.localIP();
    WiFi.SSID().toCharArray(lastSSID, 30);
  }
  display.display(false); // full refresh
  // turn off radios
  WiFi.mode(WIFI_OFF);
  btStop();
  // enable lightsleep on busy
  display.epd2.setBusyCallback(WatchyDisplay::busyCallback);
  guiState = APP_STATE;
}

void Watchy::_configModeCallback(WiFiManager *myWiFiManager) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0, 20);
  display.println("1 On your phone,");
  display.println("  join Wi-Fi:");
  display.println("  " WIFI_AP_SSID);
  display.println("2 Open (not https):");
  display.print("http://");
  display.println(WiFi.softAPIP());
  display.println("3 Pick your");
  display.println("  network, enter");
  display.println("  password, Save");
  display.print("You have ");
  display.print(WIFI_AP_TIMEOUT / 60);
  display.println(" min");
  display.display(false); // full refresh
}

bool Watchy::connectWiFi() {
  Preferences prefs;
  prefs.begin("watchy", true); // read-only
  String savedSsid = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();

  DBG("wifi: saved ssid len=%d pass len=%d\n", (int)savedSsid.length(), (int)savedPass.length());
  wl_status_t began;
  if (savedSsid.length() > 0) {
    began = WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  } else {
    began = WiFi.begin(); // login stored by WiFiManager, if there is one
  }

  DBG("wifi: begin returned %d\n", (int)began);
  if (WL_CONNECT_FAILED == began) { // WiFi not setup
    WIFI_CONFIGURED = false;
  } else {
    int wifiResult = WiFi.waitForConnectResult(); // attempt to connect for 10s
    DBG("wifi: result %d (3=connected)\n", wifiResult);
    if (WL_CONNECTED == wifiResult) {
      lastIPAddress = WiFi.localIP();
      WiFi.SSID().toCharArray(lastSSID, 30);
      WIFI_CONFIGURED = true;
      if (savedSsid.length() == 0) {
        saveWifiCreds(WiFi.SSID(), WiFi.psk()); // first success: keep our own copy
      }
    } else { // connection failed, time out
      WIFI_CONFIGURED = false;
      // turn off radios
      WiFi.mode(WIFI_OFF);
      btStop();
    }
  }
  return WIFI_CONFIGURED;
}

/*
void Watchy::showUpdateFW() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0, 30);
  display.println("Please visit");
  display.println("watchy.sqfmi.com");
  display.println("with a Bluetooth");
  display.println("enabled device");
  display.println(" ");
  display.println("Press menu button");
  display.println("again when ready");
  display.println(" ");
  display.println("Keep USB powered");
  display.display(false); // full refresh

  guiState = FW_UPDATE_STATE;
}

void Watchy::updateFWBegin() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0, 30);
  display.println("Bluetooth Started");
  display.println(" ");
  display.println("Watchy BLE OTA");
  display.println(" ");
  display.println("Waiting for");
  display.println("connection...");
  display.display(false); // full refresh

  BLE BT;
  BT.begin("Watchy BLE OTA");
  int prevStatus = -1;
  int currentStatus;

  while (1) {
    currentStatus = BT.updateStatus();
    if (prevStatus != currentStatus || prevStatus == 1) {
      if (currentStatus == 0) {
        display.setFullWindow();
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(0, 30);
        display.println("BLE Connected!");
        display.println(" ");
        display.println("Waiting for");
        display.println("upload...");
        display.display(false); // full refresh
      }
      if (currentStatus == 1) {
        display.setFullWindow();
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(0, 30);
        display.println("Downloading");
        display.println("firmware:");
        display.println(" ");
        display.print(BT.howManyBytes());
        display.println(" bytes");
        display.display(true); // partial refresh
      }
      if (currentStatus == 2) {
        display.setFullWindow();
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(0, 30);
        display.println("Download");
        display.println("completed!");
        display.println(" ");
        display.println("Rebooting...");
        display.display(false); // full refresh

        delay(2000);
        esp_restart();
      }
      if (currentStatus == 4) {
        display.setFullWindow();
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(0, 30);
        display.println("BLE Disconnected!");
        display.println(" ");
        display.println("exiting...");
        display.display(false); // full refresh
        delay(1000);
        break;
      }
      prevStatus = currentStatus;
    }
    delay(100);
  }

  // turn off radios
  WiFi.mode(WIFI_OFF);
  btStop();
  showMenu(menuIndex, false);
}
*/
// One menu item that used to be three: connect to the saved Wi-Fi (or run the setup portal if there is
// none, or it will not connect), sync the clock, and refresh the weather (which also refreshes sunrise
// and sunset, so the alarm's sunrise mode stays accurate).
void Watchy::showSync() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0, 20);
  display.println("Connecting...");
  display.display(false); // full refresh

  bool connected = connectWiFi();
  if (!connected) { // no saved network, or it would not connect: offer to set one up
    setupWifi(); // its own full screen; saves credentials and turns WiFi off when done
    connected = connectWiFi(); // try again, now that a network may be saved
  }

  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0, 20);
  if (connected) {
    display.println("Connected:");
    display.println(WiFi.SSID());
    display.println();
    if (syncNTP()) {
      clockTrusted = true;
      RTC.read(currentTime);
      display.println("Clock synced");
    } else {
      display.println("Clock sync failed");
    }
    weatherIntervalCounter = -1; // force a fetch now
    getWeatherData();
    saveState();
    display.println("Weather updated");
  } else {
    display.println("No Wi-Fi network");
    display.println("available");
  }
  display.display(true); // full refresh
  delay(2500);
  WiFi.mode(WIFI_OFF);
  btStop();
  RTC.read(currentTime);
  showWatchFace(false);
}

void Watchy::showSyncNTP() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0, 30);
  display.println("Syncing NTP... ");
  display.print("GMT offset: ");
  display.println(gmtOffset);
  display.display(false); // full refresh
  if (connectWiFi()) {
    if (syncNTP()) {
      clockTrusted = true;
      saveState();
      display.println("NTP Sync Success\n");
      display.println("Current Time Is:");

      RTC.read(currentTime);

      display.print(tmYearToCalendar(currentTime.Year));
      display.print("/");
      display.print(currentTime.Month);
      display.print("/");
      display.print(currentTime.Day);
      display.print(" - ");

      if (currentTime.Hour < 10) {
        display.print("0");
      }
      display.print(currentTime.Hour);
      display.print(":");
      if (currentTime.Minute < 10) {
        display.print("0");
      }
      display.println(currentTime.Minute);
    } else {
      display.println("NTP Sync Failed");
    }
  } else {
    display.println("WiFi Not Configured");
  }
  display.display(true); // full refresh
  delay(3000);
  showMenu(menuIndex, false);
}

bool Watchy::syncNTP() { // NTP sync - call after connecting to WiFi and
                         // remember to turn it back off
  return syncNTP(gmtOffset,
                 settings.ntpServer.c_str());
}

bool Watchy::syncNTP(long gmt) {
  return syncNTP(gmt, settings.ntpServer.c_str());
}

bool Watchy::syncNTP(long gmt, String ntpServer) {
  // NTP sync - call after connecting to
  // WiFi and remember to turn it back off
  WiFiUDP ntpUDP;
  NTPClient timeClient(ntpUDP, ntpServer.c_str(), gmt);
  timeClient.begin();
  bool synced = false;
  for (int attempt = 0; attempt < 3 && !synced; attempt++) { // right after Wi-Fi connects the first try can miss
    synced = timeClient.forceUpdate();
    if (!synced) {
      delay(300);
    }
  }
  if (!synced) {
    return false; // NTP sync failed
  }
  tmElements_t tm;
  breakTime((time_t)timeClient.getEpochTime(), tm);
  RTC.set(tm);
  return true;
}


void Watchy::showRefreshWeather() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0, 80);
  display.println("Refreshing");
  display.println("weather...");
  display.display(false); // full refresh

  weatherIntervalCounter = -1; // makes the next getWeatherData() fetch immediately
  RTC.read(currentTime);
  showWatchFace(false);
}

// Keeps the watch awake with USB up so esptool can reach it. esptool resets the running
// app into its bootloader by itself, so there is no ROM download mode to get stuck in.
// The screen shows what the watch sees, and BACK (or a 10 minute timeout) leaves.
void Watchy::showUsbMode() {
  guiState = APP_STATE;
  display.epd2.setBusyCallback(0); // no light sleep while waiting on the display: it would drop USB
  pinMode(BACK_BTN_PIN, INPUT);
  #ifdef ARDUINO_ESP32S3_DEV
  pinMode(USB_DET_PIN, INPUT);
  #endif

  const char *bootReason = "unknown";
  switch (esp_reset_reason()) {
  case ESP_RST_POWERON:   bootReason = "power on"; break;
  case ESP_RST_SW:        bootReason = "software"; break;
  case ESP_RST_DEEPSLEEP: bootReason = "deep sleep"; break;
  case ESP_RST_WDT:
  case ESP_RST_INT_WDT:
  case ESP_RST_TASK_WDT:  bootReason = "watchdog"; break;
  case ESP_RST_PANIC:     bootReason = "crash"; break;
  case ESP_RST_BROWNOUT:  bootReason = "brownout"; break;
  default: break;
  }

  const unsigned long limitMs = 10UL * 60UL * 1000UL;
  unsigned long start    = millis();
  unsigned long lastDraw = 0;
  bool first = true;
  while (millis() - start < limitMs) {
    if (!first && digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      break;
    }
    if (first || millis() - lastDraw >= 15000) {
      unsigned long left = (limitMs - (millis() - start)) / 1000;
      display.setFullWindow();
      display.fillScreen(GxEPD_WHITE);
      display.setFont(&FreeMonoBold9pt7b);
      display.setTextColor(GxEPD_BLACK);
      display.setCursor(0, 20);
      display.println("USB Flash Mode");
      display.println();
      #ifdef ARDUINO_ESP32S3_DEV
      display.println(digitalRead(USB_DET_PIN) == 1 ? "USB: cable in" : "USB: no cable");
      #endif
      display.print("Awake: ");
      display.print(left / 60);
      display.print(":");
      if (left % 60 < 10) {
        display.print("0");
      }
      display.print(left % 60);
      display.println(" left");
      display.print("Boot: ");
      display.println(bootReason);
      display.println();
      display.println("Flash from PC.");
      display.println("BACK = exit");
      display.display(!first); // full refresh once, then partial
      lastDraw = millis();
      first    = false;
    }
    delay(50);
  }

  display.epd2.setBusyCallback(WatchyDisplay::busyCallback);
  RTC.read(currentTime);
  showWatchFace(false);
}


// Texts forwarded from the T-Deck. UP/DOWN page through them, BACK leaves.
void Watchy::showMessages() {
  guiState = APP_STATE;
  pinMode(BACK_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(DOWN_BTN_PIN, INPUT);

  int n   = wlCount();
  int idx = 0; // 0 = newest

  auto draw = [&](bool partial) {
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(0, 18);
    if (n == 0) {
      display.println("No messages");
      display.println();
      display.println("Texts from your");
      display.println("T-Deck show here.");
    } else {
      const WlMsg *m = wlGet(idx);
      display.print(idx + 1);
      display.print("/");
      display.print(n);
      display.print(" ");
      display.println(m->from);
      display.println();
      const char *t = m->text;
      char line[24];
      while (*t) { // wrap at 18 characters, at a space when there is one
        int len = 0, lastSpace = -1;
        while (t[len] && len < 18) {
          if (t[len] == ' ')
            lastSpace = len;
          len++;
        }
        if (t[len] && lastSpace > 0)
          len = lastSpace;
        memcpy(line, t, len);
        line[len] = 0;
        display.println(line);
        t += len;
        while (*t == ' ')
          t++;
      }
    }
    display.display(partial);
  };

  wlClearUnread();
  draw(false);

  unsigned long last = millis();
  while (millis() - last < 60000) {
    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      break;
    }
    if (digitalRead(UP_BTN_PIN) == ACTIVE_LOW && idx < n - 1) {
      idx++;
      draw(true);
      last = millis();
      delay(250);
    }
    if (digitalRead(DOWN_BTN_PIN) == ACTIVE_LOW && idx > 0) {
      idx--;
      draw(true);
      last = millis();
      delay(250);
    }
    delay(30);
  }
  delay(300);
  showMenu(menuIndex, false);
}


// Temporary always-awake test of the T-Deck link: asks every few seconds for up to 5 minutes,
// shows the results and prints each attempt over USB. BACK leaves.
void Watchy::showLinkTest() {
  guiState = APP_STATE;
  display.epd2.setBusyCallback(0); // no light sleep while waiting on the display: it would drop USB
  pinMode(BACK_BTN_PIN, INPUT);

  int tries = 0, answered = 0, fresh = 0;
  unsigned long start = millis(), lastDraw = 0;
  bool first = true;
  Serial.println("linktest: start");
  while (millis() - start < 300000) {
    if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
      break;
    }
    float v = getBatteryVoltage();
    int pct = (int)((v - 3.2f) / 1.0f * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int n = wlCheckin((uint8_t)pct, true);
    tries++;
    if (wlLastAnswered())
      answered++;
    fresh += n;
    Serial.printf("linktest: try %d answered=%d new=%d (answered %d, new total %d, unread %d)\n", tries, (int)wlLastAnswered(), n,
                  answered, fresh, wlUnread());
    if (first || millis() - lastDraw >= 8000) {
      display.setFullWindow();
      display.fillScreen(GxEPD_WHITE);
      display.setFont(&FreeMonoBold9pt7b);
      display.setTextColor(GxEPD_BLACK);
      display.setCursor(0, 20);
      display.println("Link Test");
      display.println();
      display.print("Tries:    ");
      display.println(tries);
      display.print("Answered: ");
      display.println(answered);
      display.print("New texts: ");
      display.println(fresh);
      display.print("Unread:   ");
      display.println(wlUnread());
      display.println();
      display.println("BACK = exit");
      display.display(!first);
      first    = false;
      lastDraw = millis();
    }
    for (int w = 0; w < 75; w++) { // wait about 1.5 s, but keep watching BACK
      if (digitalRead(BACK_BTN_PIN) == ACTIVE_LOW) {
        break;
      }
      delay(20);
    }
  }
  Serial.println("linktest: end");
  display.epd2.setBusyCallback(WatchyDisplay::busyCallback);
  delay(300);
  showMenu(menuIndex, false);
}
