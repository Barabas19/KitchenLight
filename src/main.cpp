#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <NTPClient.h>
#include <TimeLib.h>

#define SUNRISE_PROVIDER_URL      "http://api.sunrise-sunset.org/json?lat=_LATITUDE_&lng=_LONGITUDE_"
#define RELAY_PIN   0
#define TIME_ZONE   +1

String sunrise_provider_url;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, TIME_ZONE * 3600);
HTTPClient http;
WiFiClient client;
struct Twilight
{
  int sunrise_begin;
  int sunrise_end;
  int sunset_begin;
  int sunset_end;
} twilight;

String sendHttpGetRequest(String _url) {
  String payload = "";
  if(http.begin(client, _url) && (http.GET() == HTTP_CODE_OK))
  {
    payload = http.getString();
    http.end();
  }
  return payload;
}

bool isSummerTime(time_t epoch_time) {
  auto dst = false;
  auto y = year(epoch_time);
  auto x_march = (y + y/4 + 5) % 7;
  auto x_octob = (y + y/4 + 2) % 7;
  auto m = month(epoch_time);
  auto d = day(epoch_time);
  auto h = hour(epoch_time);
  if((m == 3 && d == (31 - x_march) && h >= 2) || ((m == 3 && d > (31 - x_march)) || m > 3)) {
    dst = true;
  }
  if((m == 1 && d == (31 - x_octob) && h >= 2) || ((m == 10 && d == (31 - x_octob)) || m > 10 || m < 3)) {
    dst = false;
  }

  return dst;
}

bool extractTwilight(Twilight* tl, String payload) {
  if(tl == nullptr) return false;
  if(payload == "") return false;
  // payload example: { "results":{"sunrise":"5:32:38 AM","sunset":"5:01:25 PM","solar_noon":"11:17:01 AM","day_length":"11:28:47",
  //                    "civil_twilight_begin":"5:00:42 AM","civil_twilight_end":"5:33:20 PM",
  //                    "nautical_twilight_begin":"4:23:27 AM","nautical_twilight_end":"6:10:35 PM",
  //                    "astronomical_twilight_begin":"3:45:34 AM","astronomical_twilight_end":"6:48:28 PM"},"status":"OK"}
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    return false;
  }
  String twilight_begin = doc["results"]["civil_twilight_begin"];
  String sunrise        = doc["results"]["sunrise"];
  String sunset         = doc["results"]["sunset"];
  String twilight_end   = doc["results"]["civil_twilight_end"];

  tl->sunrise_begin   = twilight_begin.substring(0,1).toInt() * 60 + twilight_begin.substring(2,4).toInt();
  tl->sunrise_end     = sunrise.substring(0,1).toInt() * 60 + sunrise.substring(2,4).toInt();
  if(sunset.length() > 10){
    tl->sunset_begin = sunset.substring(0,2).toInt() * 60 + sunset.substring(3,5).toInt();
  } else {
    tl->sunset_begin = sunset.substring(0,1).toInt() * 60 + sunset.substring(2,4).toInt();
  }
  if(twilight_end.length() > 10) {
    tl->sunset_end = twilight_end.substring(0,2).toInt() * 60 + twilight_end.substring(3,5).toInt();
  } else {
    tl->sunset_end = twilight_end.substring(0,1).toInt() * 60 + twilight_end.substring(2,4).toInt();
  }

  return true;
}

void correctTwilight(Twilight* tl, const bool dst, const long time_zone_h) {
  // 12h -> 24h
  tl->sunset_begin += 720;
  tl->sunset_end += 720;
  
  // Correct time zone
  tl->sunset_begin += time_zone_h * 60;
  tl->sunset_end += time_zone_h * 60;
  tl->sunrise_begin += time_zone_h * 60;
  tl->sunrise_end += time_zone_h * 60;

  // Correct summer time
  if(dst) {
    tl->sunset_begin += 60;
    tl->sunset_end += 60;
    tl->sunrise_begin += 60;
    tl->sunrise_end += 60;
  }
}

void turnLightOnOff(Twilight* tl, time_t epochTime) {
  int hours = hour(epochTime);
  int minutes = minute(epochTime);
  int now = hours * 60 + minutes;
  bool desiredRelayStatus = (now <= tl->sunrise_begin) || (now >= tl->sunset_end);
  digitalWrite(RELAY_PIN, desiredRelayStatus);
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    delay(5000);
    ESP.restart();
  }
  
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
  timeClient.begin();
  timeClient.update();

  sunrise_provider_url = String(SUNRISE_PROVIDER_URL);
  sunrise_provider_url.replace("_LATITUDE_", LATITUDE);
  sunrise_provider_url.replace("_LONGITUDE_", LONGITUDE);
}

void loop() {
  ArduinoOTA.handle();

  time_t epoch_time = timeClient.getEpochTime();
  bool summer_time = isSummerTime(epoch_time);

  // update time - every hour
  static int hours = 0;
  int hours_now = hour(epoch_time);
  if(hours_now != hours) {
    timeClient.update();
    timeClient.setTimeOffset((TIME_ZONE + int(summer_time)) * 3600);
    hours = hours_now;
  }

  // update twilight - every minute
  static int minutes = 0;
  int minutes_now = minute(epoch_time);
  if(minutes_now != minutes) {
    String payload = sendHttpGetRequest(sunrise_provider_url);
    if(extractTwilight(&twilight, payload)) {
      correctTwilight(&twilight, summer_time, TIME_ZONE);
      turnLightOnOff(&twilight, epoch_time);
      minutes = minutes_now;
    }
  }
}