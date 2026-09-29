#include "Watchy_7_SEG.h"

#define DARKMODE false

const uint8_t WEATHER_ICON_WIDTH = 48;
const uint8_t WEATHER_ICON_HEIGHT = 32;

extern long gmtOffset; // seconds from UTC, updated by the weather fetch

static const uint16_t INK   = DARKMODE ? GxEPD_WHITE : GxEPD_BLACK;
static const uint16_t PAPER = DARKMODE ? GxEPD_BLACK : GxEPD_WHITE;

// Solar ring: a square loading bar running around the screen edge, clockwise from
// top-centre. One lap is 24 hours and starts at sunrise, so sunrise is always at the top.
// A 6px square drawn at (x, y) covers x-3..x+2, so its centre is x-0.5. With the path on
// x/y = 3 and 197 the band covers pixels 0..5 on the top/left and 194..199 on the bottom/right:
// the same distance from every edge, touching the edge of the screen.
const int RING_MIN   = 3;    // path on the top and left edges
const int RING_MAX   = 197;  // path on the bottom and right edges
const int RING_SIDE  = 194;  // RING_MAX - RING_MIN
const int RING_START = 97;   // top edge, from top-centre (x=100) to the top-right corner
const int RING_TOTAL = 776;  // 97 + 3 * 194 + 97

// Position along the ring: d = 0 is top-centre, growing clockwise.
static void ringPoint(int d, int &x, int &y, int &side){
    d = ((d % RING_TOTAL) + RING_TOTAL) % RING_TOTAL;
    int b = RING_START + RING_SIDE;
    int c = b + RING_SIDE;
    int e = c + RING_SIDE;
    if(d < RING_START){ x = 100 + d;            y = RING_MIN;                  side = 0; } // top
    else if(d < b)    { x = RING_MAX;           y = RING_MIN + (d - RING_START); side = 1; } // right
    else if(d < c)    { x = RING_MAX - (d - b); y = RING_MAX;                  side = 2; } // bottom
    else if(d < e)    { x = RING_MIN;           y = RING_MAX - (d - c);        side = 3; } // left
    else              { x = RING_MIN + (d - e); y = RING_MIN;                  side = 0; } // top, left half
}

// Minutes since local midnight. The library keeps sunrise/sunset as UTC, so shift by the
// location's offset first.
static int localMinutes(tmElements_t utc){
    tmElements_t local;
    breakTime(makeTime(utc) + ::gmtOffset, local);
    return local.Hour * 60 + local.Minute;
}

static bool sunDataValid(tmElements_t rise, tmElements_t set){
    return makeTime(rise) > 86400UL * 365 && makeTime(set) > 86400UL * 365; // 0 means never fetched
}

static String formatClock(int minutes){
    int hour = minutes / 60;
    int minute = minutes % 60;
    if(HOUR_12_24 == 12){
        hour = ((hour + 11) % 12) + 1;
    }
    return String(hour) + ":" + (minute < 10 ? "0" : "") + String(minute);
}

// Typical single-cell LiPo discharge curve, interpolated linearly between points.
static int batteryPercent(float v){
    static const float volts[] = {3.27, 3.61, 3.69, 3.71, 3.73, 3.75, 3.77, 3.79, 3.80, 3.82, 3.84,
                                  3.85, 3.87, 3.91, 3.95, 3.98, 4.02, 4.08, 4.11, 4.15, 4.20};
    static const uint8_t pct[]  = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50,
                                   55, 60, 65, 70, 75, 80, 85, 90, 95, 100};
    const int n = sizeof(volts) / sizeof(volts[0]);
    if(v <= volts[0]) return 0;
    if(v >= volts[n-1]) return 100;
    for(int i = 1; i < n; i++){
        if(v < volts[i]){
            return pct[i-1] + (v - volts[i-1]) * (pct[i] - pct[i-1]) / (volts[i] - volts[i-1]);
        }
    }
    return 100;
}

void Watchy7SEG::drawWatchFace(){
    display.fillScreen(PAPER);
    display.setTextColor(INK);
    // Ask the T-Deck for texts first, while it is listening around the minute mark; buzz for new ones
    int fresh = wlCheckin(batteryPercent(getBatteryVoltage()));
    if(fresh > 0){
        vibMotor(100, 6);
    }
    weatherData weather = getWeatherData(); // first: it updates Wi-Fi state and sun times
    RTC.read(currentTime); // the fetch also syncs the clock
    drawRing(weather);
    drawStatus();
    drawWeather(weather);
    drawTime();
    drawSunRow(weather);
    drawDate();
    drawSteps();
}

// Perimeter dial, one lap = 24 hours starting at sunrise (12 o'clock):
//   night         small dotted line
//   day           clear
//   sunrise/set   two small hollow circles
//   time so far   a solid line filling in from sunrise, clockwise
void Watchy7SEG::drawRing(weatherData &weather){
    if(!sunDataValid(weather.sunrise, weather.sunset)){
        return; // nothing known yet: leave the edge blank
    }
    int nowMin  = currentTime.Hour * 60 + currentTime.Minute;
    int rise    = localMinutes(weather.sunrise);
    int dayLen  = (localMinutes(weather.sunset) - rise + 1440) % 1440;
    int elapsed = (nowMin - rise + 1440) % 1440;
    if(dayLen < 4 * 60 || dayLen > 20 * 60){
        return; // not a believable day length (bad or stale data): draw nothing rather than nonsense
    }
    int dNow = (long)elapsed * RING_TOTAL / 1440;
    int dSet = (long)dayLen * RING_TOTAL / 1440;

    for(int d = 0; d < RING_TOTAL; d++){
        int x, y, side;
        ringPoint(d, x, y, side);
        if(d <= dNow){
            display.fillRect(x - 3, y - 3, 6, 6, INK);        // solid line for the time so far
        }else if(d >= dSet && d % 4 == 0){
            display.fillRect(x - 1, y - 1, 2, 2, INK);        // night still to come: small dots
        }
    }

    // small circle on the line, nudged 1px inward so it stays inside the screen edge
    auto marker = [&](int d, bool filled){
        int cx, cy, side;
        ringPoint(d, cx, cy, side);
        switch(side){        // centre the circle 4px from its edge, the same on all four sides
            case 0: cy = 4; break;
            case 1: cx = 195; break;
            case 2: cy = 195; break;
            default: cx = 4; break;
        }
        if(filled){
            display.fillCircle(cx, cy, 5, PAPER);  // clear halo so it stays visible on the solid line
            display.fillCircle(cx, cy, 4, INK);
        }else{
            display.fillCircle(cx, cy, 4, PAPER);
            display.drawCircle(cx, cy, 4, INK);
        }
    };
    marker(0, false);     // sunrise: hollow, always 12 o'clock
    marker(dSet, true);   // sunset: filled, wherever the day length puts it
}

// Wi-Fi, charging and battery percent, centred above the weather.
void Watchy7SEG::drawStatus(){
    String pct = String(batteryPercent(getBatteryVoltage())) + "%";
    display.setFont(&Seven_Segment10pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(pct, 0, 0, &x1, &y1, &w, &h);

    bool plugged = false;
    #ifdef ARDUINO_ESP32S3_DEV
    plugged = USB_PLUGGED_IN;
    #endif

    int unread = wlUnread();
    String cnt = String(unread);
    int16_t cx1 = 0, cy1;
    uint16_t cw = 0, ch;
    if(unread > 0){
        display.getTextBounds(cnt, 0, 0, &cx1, &cy1, &cw, &ch);
    }
    int total = 26 + 8 + (plugged ? 16 + 8 : 0) + w + (unread > 0 ? 10 + 16 + 4 + cw : 0);
    int x = (200 - total) / 2;
    display.drawBitmap(x, 12, WIFI_CONFIGURED ? wifi : wifioff, 26, 18, INK);
    x += 26 + 8;
    if(plugged){
        display.drawBitmap(x, 12, charge, 16, 18, INK);
        x += 16 + 8;
    }
    display.setCursor(x - x1, 27);
    display.print(pct);
    if(unread > 0){ // envelope and how many texts are unread
        x += w + 10;
        display.drawRect(x, 15, 16, 12, INK);
        display.drawLine(x, 15, x + 8, 22, INK);
        display.drawLine(x + 15, 15, x + 8, 22, INK);
        display.setCursor(x + 20 - cx1, 27);
        display.print(cnt);
    }
}

void Watchy7SEG::drawWeather(weatherData &weather){
    int8_t temperature = weather.temperature;
    int16_t weatherConditionCode = weather.weatherConditionCode;

    display.setFont(&DSEG7_Classic_Bold_25);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(String(temperature), 0, 0, &x1, &y1, &w, &h);

    int total = WEATHER_ICON_WIDTH + 8 + w + 4 + 26;
    int x = (200 - total) / 2;

    const unsigned char* weatherIcon = nullptr;
    if(WIFI_CONFIGURED){
        //https://openweathermap.org/weather-conditions
        if(weatherConditionCode > 801){         weatherIcon = cloudy;
        }else if(weatherConditionCode == 801){  weatherIcon = cloudsun;
        }else if(weatherConditionCode == 800){  weatherIcon = sunny;
        }else if(weatherConditionCode >= 700){  weatherIcon = atmosphere;
        }else if(weatherConditionCode >= 600){  weatherIcon = snow;
        }else if(weatherConditionCode >= 500){  weatherIcon = rain;
        }else if(weatherConditionCode >= 300){  weatherIcon = drizzle;
        }else if(weatherConditionCode >= 200){  weatherIcon = thunderstorm;
        }
    }else{
        weatherIcon = chip;
    }
    if(weatherIcon){
        display.drawBitmap(x, 33, weatherIcon, WEATHER_ICON_WIDTH, WEATHER_ICON_HEIGHT, INK);
    }
    x += WEATHER_ICON_WIDTH + 8;

    display.setCursor(x - x1, 62);
    display.print(temperature);
    x += w + 4;
    display.drawBitmap(x, 39, weather.isMetric ? celsius : fahrenheit, 26, 20, INK);
}

// Clock in the middle. Fixed x so the digits don't shift when a "1" changes the text width.
void Watchy7SEG::drawTime(){
    display.setFont(&DSEG7_Classic_Bold_53);
    display.setCursor(6, 122);
    int displayHour;
    if(HOUR_12_24 == 12){
        displayHour = ((currentTime.Hour + 11) % 12) + 1;
    }else{
        displayHour = currentTime.Hour;
    }
    if(displayHour < 10){
        display.print("0");
    }
    display.print(displayHour);
    display.print(":");
    if(currentTime.Minute < 10){
        display.print("0");
    }
    display.println(currentTime.Minute);
}

// Sunrise (up arrow) and sunset (down arrow), centred under the time.
void Watchy7SEG::drawSunRow(weatherData &weather){
    bool valid = sunDataValid(weather.sunrise, weather.sunset);
    String rise = valid ? formatClock(localMinutes(weather.sunrise)) : String("--:--");
    String set  = valid ? formatClock(localMinutes(weather.sunset))  : String("--:--");

    display.setFont(&Seven_Segment10pt7b);
    int16_t x1, y1, x2, y2;
    uint16_t w1, h1, w2, h2;
    display.getTextBounds(rise, 0, 0, &x1, &y1, &w1, &h1);
    display.getTextBounds(set, 0, 0, &x2, &y2, &w2, &h2);

    const int baseline = 142;
    const int top = baseline - 11;
    int total = (9 + 3 + w1) + 16 + (9 + 3 + w2);
    int x = (200 - total) / 2;

    display.fillTriangle(x, top + 8, x + 8, top + 8, x + 4, top, INK);
    x += 9 + 3;
    display.setCursor(x - x1, baseline);
    display.print(rise);
    x += w1 + 16;

    display.fillTriangle(x, top, x + 8, top, x + 4, top + 8, INK);
    x += 9 + 3;
    display.setCursor(x - x2, baseline);
    display.print(set);
}

// Month, day and year centred under the sun row.
void Watchy7SEG::drawDate(){
    display.setFont(&Seven_Segment10pt7b);
    String date = String(monthShortStr(currentTime.Month)) + " " + String(currentTime.Day) + " " +
                  String(tmYearToCalendar(currentTime.Year)); // year is stored as an offset from 1970
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(date, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(100 - w / 2 - x1, 162);
    display.print(date);
}

void Watchy7SEG::drawSteps(){
    // reset step counter at midnight
    if(currentTime.Hour == 0 && currentTime.Minute == 0){
        sensor.resetStepCounter();
    }
    String count = String(sensor.getCounter());
    display.setFont(&DSEG7_Classic_Regular_15);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(count, 0, 0, &x1, &y1, &w, &h);

    int total = 19 + 8 + w;
    int x = (200 - total) / 2;
    display.drawBitmap(x, 166, steps, 19, 23, INK);
    x += 19 + 8;
    display.setCursor(x - x1, 183);
    display.print(count);
}
