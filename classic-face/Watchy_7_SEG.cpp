#include "Watchy_7_SEG.h"

#define DARKMODE false

const uint8_t BATTERY_SEGMENT_WIDTH = 7;
const uint8_t BATTERY_SEGMENT_HEIGHT = 11;
const uint8_t BATTERY_SEGMENT_SPACING = 9;
const uint8_t WEATHER_ICON_WIDTH = 48;
const uint8_t WEATHER_ICON_HEIGHT = 32;

extern long gmtOffset; // seconds from UTC, updated by the weather fetch

void Watchy7SEG::drawWatchFace(){
    display.fillScreen(DARKMODE ? GxEPD_BLACK : GxEPD_WHITE);
    display.setTextColor(DARKMODE ? GxEPD_WHITE : GxEPD_BLACK);
    drawTime();
    drawDate();
    drawSteps();
    drawWeather();
    drawBattery();
    display.drawBitmap(116, 75, WIFI_CONFIGURED ? wifi : wifioff, 26, 18, DARKMODE ? GxEPD_WHITE : GxEPD_BLACK);
    if(BLE_CONFIGURED){
        display.drawBitmap(100, 73, bluetooth, 13, 21, DARKMODE ? GxEPD_WHITE : GxEPD_BLACK);
    }
    #ifdef ARDUINO_ESP32S3_DEV
    if(USB_PLUGGED_IN){
      display.drawBitmap(140, 75, charge, 16, 18, DARKMODE ? GxEPD_WHITE : GxEPD_BLACK);
    }
    #endif
}

void Watchy7SEG::drawTime(){
    display.setFont(&DSEG7_Classic_Bold_53);
    display.setCursor(5, 53+5);
    int displayHour;
    if(HOUR_12_24==12){
      displayHour = ((currentTime.Hour+11)%12)+1;
    } else {
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

void Watchy7SEG::drawDate(){
    display.setFont(&Seven_Segment10pt7b);

    int16_t  x1, y1;
    uint16_t w, h;

    String dayOfWeek = dayStr(currentTime.Wday);
    display.getTextBounds(dayOfWeek, 5, 85, &x1, &y1, &w, &h);
    if(currentTime.Wday == 4){
        w = w - 5;
    }
    display.setCursor(85 - w, 85);
    display.println(dayOfWeek);

    String month = monthShortStr(currentTime.Month);
    display.getTextBounds(month, 60, 110, &x1, &y1, &w, &h);
    display.setCursor(85 - w, 110);
    display.println(month);

    display.setFont(&DSEG7_Classic_Bold_25);
    display.setCursor(5, 120);
    if(currentTime.Day < 10){
    display.print("0");
    }
    display.println(currentTime.Day);
    display.setCursor(5, 150);
    display.println(tmYearToCalendar(currentTime.Year));// offset from 1970, since year is stored in uint8_t
}
void Watchy7SEG::drawSteps(){
    // reset step counter at midnight
    if (currentTime.Hour == 0 && currentTime.Minute == 0){
      sensor.resetStepCounter();
    }
    uint32_t stepCount = sensor.getCounter();
    display.drawBitmap(10, 165, steps, 19, 23, DARKMODE ? GxEPD_WHITE : GxEPD_BLACK);
    display.setCursor(35, 190);
    display.println(stepCount);
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

void Watchy7SEG::drawBattery(){
    String text = String(batteryPercent(getBatteryVoltage())) + "%";

    display.setFont(&Seven_Segment10pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 90, &x1, &y1, &w, &h);
    display.setCursor(195 - w - x1, 90); // right edge where the battery icon ended
    display.print(text);
}

void Watchy7SEG::drawWeather(){

    weatherData currentWeather = getWeatherData();

    drawSunTimes(currentWeather);

    int8_t temperature = currentWeather.temperature;
    int16_t weatherConditionCode = currentWeather.weatherConditionCode;

    display.setFont(&DSEG7_Classic_Regular_39);
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(String(temperature), 0, 0, &x1, &y1, &w, &h);
    if(159 - w - x1 > 87){
        display.setCursor(159 - w - x1, 150);
    }else{
        display.setFont(&DSEG7_Classic_Bold_25);
        display.getTextBounds(String(temperature), 0, 0, &x1, &y1, &w, &h);
        display.setCursor(159 - w - x1, 136);
    }
    display.println(temperature);
    display.drawBitmap(165, 110, currentWeather.isMetric ? celsius : fahrenheit, 26, 20, DARKMODE ? GxEPD_WHITE : GxEPD_BLACK);
    const unsigned char* weatherIcon;

    if(WIFI_CONFIGURED){
      //https://openweathermap.org/weather-conditions
      if(weatherConditionCode > 801){//Cloudy
        weatherIcon = cloudy;
      }else if(weatherConditionCode == 801){//Few Clouds
        weatherIcon = cloudsun;
      }else if(weatherConditionCode == 800){//Clear
        weatherIcon = sunny;
      }else if(weatherConditionCode >=700){//Atmosphere
        weatherIcon = atmosphere;
      }else if(weatherConditionCode >=600){//Snow
        weatherIcon = snow;
      }else if(weatherConditionCode >=500){//Rain
        weatherIcon = rain;
      }else if(weatherConditionCode >=300){//Drizzle
        weatherIcon = drizzle;
      }else if(weatherConditionCode >=200){//Thunderstorm
        weatherIcon = thunderstorm;
      }else 
      return;
    }else{
      weatherIcon = chip;
    }
    
    display.drawBitmap(145, 158, weatherIcon, WEATHER_ICON_WIDTH, WEATHER_ICON_HEIGHT, DARKMODE ? GxEPD_WHITE : GxEPD_BLACK);
}

// Sunrise/sunset in the free strip left of the weather icon.
// The library keeps them as UTC, so shift by the location's offset first.
void Watchy7SEG::drawSunTimes(weatherData &weather){
    if(!WIFI_CONFIGURED || !weather.external){
      return; // no fetched data yet
    }
    drawSunTime(173, true, weather.sunrise);
    drawSunTime(190, false, weather.sunset);
}

void Watchy7SEG::drawSunTime(int16_t baseline, bool rising, tmElements_t utc){
    tmElements_t local;
    breakTime(makeTime(utc) + ::gmtOffset, local);

    int hour = (HOUR_12_24==12) ? ((local.Hour+11)%12)+1 : local.Hour;
    String text = String(hour) + ":" + (local.Minute < 10 ? "0" : "") + String(local.Minute);

    display.setFont(&Seven_Segment10pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, baseline, &x1, &y1, &w, &h);
    int16_t right = 142;
    display.setCursor(right - w - x1, baseline);
    display.print(text);

    // arrow: up = sunrise, down = sunset
    uint16_t color = DARKMODE ? GxEPD_WHITE : GxEPD_BLACK;
    int16_t ax = right - w - 12;
    int16_t top = baseline - 11;
    if(rising){
        display.fillTriangle(ax, top + 8, ax + 8, top + 8, ax + 4, top, color);
    }else{
        display.fillTriangle(ax, top, ax + 8, top, ax + 4, top + 8, color);
    }
}
