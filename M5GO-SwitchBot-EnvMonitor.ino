#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <esp_heap_caps.h>
#include "config.h"

#if __has_include("JetBrainsMono9pt7b.h") && __has_include("JetBrainsMono12pt7b.h") && __has_include("JetBrainsMono24pt7b.h")
  #include "JetBrainsMono9pt7b.h"
  #include "JetBrainsMono12pt7b.h"
  #include "JetBrainsMono24pt7b.h"
  #define HAVE_JETBRAINS_MONO 1
#else
  #define HAVE_JETBRAINS_MONO 0
#endif

static constexpr char APP_NAME[] = "M5GO-SwitchBot-EnvMonitor";
static constexpr char APP_VERSION[] = "0.3.1";
static constexpr int SCREEN_W = 320, SCREEN_H = 240;
static constexpr uint16_t COLOR_BG=0x1082, COLOR_TEXT=0xE71C, COLOR_MUTED=0x8410;
static constexpr uint16_t COLOR_CYAN=0x05DB, COLOR_ORANGE=0xFC60, COLOR_PURPLE=0xB29F;
static constexpr uint16_t COLOR_GREEN=0x5E8B, COLOR_YELLOW=0xDDA0, COLOR_RED=0xE986, COLOR_DIVIDER=0x3186;
M5Canvas canvas(&M5.Display);

enum class Page { MAIN, STATUS };
Page currentPage = Page::MAIN;
static constexpr uint32_t DISPLAY_SLEEP_MS=180000UL, LONG_PRESS_MS=800UL;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS=10000UL, MQTT_RETRY_INTERVAL_MS=5000UL, DISPLAY_INTERVAL_MS=1000UL;
static constexpr uint32_t DATA_STALE_SEC=180, DATA_OFFLINE_SEC=600;
bool displaySleeping=false;
uint32_t lastUserActivityMs=0;
static constexpr uint8_t BRIGHTNESS_LEVELS[]={40,80,120,160,220};
static constexpr size_t BRIGHTNESS_LEVEL_COUNT=sizeof(BRIGHTNESS_LEVELS)/sizeof(BRIGHTNESS_LEVELS[0]);
uint8_t brightnessIndex=3;
Preferences preferences;
struct ButtonState { bool previousPressed=false; bool longActionDone=false; uint32_t pressedAt=0; };
ButtonState buttonA,buttonB,buttonC;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
float temperature=NAN, humidity=NAN;
bool hasSensorData=false, ntpReady=false;
time_t lastDataReceivedAt=0;
uint32_t lastWifiAttemptMs=0,lastMqttAttemptMs=0,lastDisplayMs=0;

void drawScreen();

void setSmallFont(){
#if HAVE_JETBRAINS_MONO
  canvas.setFont(&JetBrainsMono9pt7b);
#else
  canvas.setFont(&fonts::FreeMono9pt7b);
#endif
}
void setMediumFont(){
#if HAVE_JETBRAINS_MONO
  canvas.setFont(&JetBrainsMono12pt7b);
#else
  canvas.setFont(&fonts::FreeMonoBold12pt7b);
#endif
}
void setLargeFont(){
#if HAVE_JETBRAINS_MONO
  canvas.setFont(&JetBrainsMono24pt7b);
#else
  canvas.setFont(&fonts::FreeMonoBold24pt7b);
#endif
}
void drawTextSmall(const String&t,int x,int y,textdatum_t d,uint16_t c){canvas.setTextDatum(d);canvas.setTextColor(c);setSmallFont();canvas.drawString(t,x,y);}
void drawTextMedium(const String&t,int x,int y,textdatum_t d,uint16_t c){canvas.setTextDatum(d);canvas.setTextColor(c);setMediumFont();canvas.drawString(t,x,y);}
void drawTextLarge(const String&t,int x,int y,textdatum_t d,uint16_t c){canvas.setTextDatum(d);canvas.setTextColor(c);setLargeFont();canvas.drawString(t,x,y);}

bool isTimeValid(){return time(nullptr)>1704067200;}
String getTimeString(){struct tm t;if(!getLocalTime(&t,10))return "--:--";char b[8];snprintf(b,sizeof(b),"%02d:%02d",t.tm_hour,t.tm_min);return b;}
String getDateString(){struct tm t;if(!getLocalTime(&t,10))return "--/--";char b[8];snprintf(b,sizeof(b),"%02d/%02d",t.tm_mon+1,t.tm_mday);return b;}
String getDataStateString(){if(!hasSensorData)return "WAIT";if(!isTimeValid()||!lastDataReceivedAt)return "LIVE";time_t n=time(nullptr);uint32_t a=n>=lastDataReceivedAt?(uint32_t)(n-lastDataReceivedAt):0;if(a<DATA_STALE_SEC)return "LIVE";if(a<DATA_OFFLINE_SEC)return "STALE";return "OFFLINE";}
uint16_t getDataStateColor(){String s=getDataStateString();if(s=="LIVE")return COLOR_GREEN;if(s=="STALE")return COLOR_YELLOW;if(s=="OFFLINE")return COLOR_RED;return COLOR_PURPLE;}
String getDataAgeString(){if(!hasSensorData)return "waiting";if(!isTimeValid()||!lastDataReceivedAt)return "received";time_t n=time(nullptr);uint32_t a=n>=lastDataReceivedAt?(uint32_t)(n-lastDataReceivedAt):0;if(a<60)return String(a)+"s ago";if(a<3600)return String(a/60)+"m ago";if(a<86400)return String(a/3600)+"h ago";return String(a/86400)+"d ago";}
String getIPAddressString(){return WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():"-";}
String getUptimeString(){uint32_t s=millis()/1000UL,d=s/86400UL,h=(s%86400UL)/3600UL,m=(s%3600UL)/60UL;char b[24];if(d)snprintf(b,sizeof(b),"%lud %02luh",(unsigned long)d,(unsigned long)h);else snprintf(b,sizeof(b),"%luh %02lum",(unsigned long)h,(unsigned long)m);return b;}

void drawHeader(){String title=currentPage==Page::MAIN?"ENV / B3D8":"SYSTEM STATUS";drawTextSmall(title,14,21,middle_left,COLOR_CYAN);drawTextSmall(isTimeValid()?getDateString()+" "+getTimeString():"--/-- --:--",306,21,middle_right,COLOR_TEXT);canvas.drawFastHLine(14,39,292,COLOR_DIVIDER);}
void drawMainPage(){const int lx=82,rx=238;canvas.drawFastVLine(160,52,135,COLOR_DIVIDER);drawTextSmall("TEMPERATURE",lx,61,middle_center,COLOR_MUTED);drawTextSmall("HUMIDITY",rx,61,middle_center,COLOR_MUTED);char b[16];if(hasSensorData&&!isnan(temperature)){snprintf(b,sizeof(b),"%.1f",temperature);drawTextLarge(b,lx,119,middle_center,COLOR_ORANGE);}else drawTextLarge("--.-",lx,119,middle_center,COLOR_ORANGE);if(hasSensorData&&!isnan(humidity)){snprintf(b,sizeof(b),"%.0f",humidity);drawTextLarge(b,rx,119,middle_center,COLOR_CYAN);}else drawTextLarge("--",rx,119,middle_center,COLOR_CYAN);
  // Draw degree symbol geometrically so the ASCII-subset font stays small.
  canvas.drawCircle(lx-11,158,3,COLOR_ORANGE); drawTextMedium("C",lx+4,171,middle_center,COLOR_ORANGE); drawTextMedium("%",rx,171,middle_center,COLOR_CYAN);
}
void drawStatusPage(){const int l=18,r=302;const int ys[]={60,84,108,132,156,180};drawTextSmall("Wi-Fi",l,ys[0],middle_left,COLOR_MUTED);drawTextSmall(WiFi.status()==WL_CONNECTED?String(WiFi.RSSI())+" dBm":"OFFLINE",r,ys[0],middle_right,WiFi.status()==WL_CONNECTED?COLOR_GREEN:COLOR_RED);drawTextSmall("MQTT",l,ys[1],middle_left,COLOR_MUTED);drawTextSmall(mqttClient.connected()?"CONNECTED":"OFFLINE",r,ys[1],middle_right,mqttClient.connected()?COLOR_GREEN:COLOR_RED);drawTextSmall("Sensor",l,ys[2],middle_left,COLOR_MUTED);drawTextSmall("B3D8",r,ys[2],middle_right,COLOR_TEXT);drawTextSmall("Data age",l,ys[3],middle_left,COLOR_MUTED);drawTextSmall(getDataAgeString(),r,ys[3],middle_right,getDataStateColor());drawTextSmall("IP",l,ys[4],middle_left,COLOR_MUTED);drawTextSmall(getIPAddressString(),r,ys[4],middle_right,COLOR_TEXT);drawTextSmall("Uptime",l,ys[5],middle_left,COLOR_MUTED);drawTextSmall(getUptimeString(),r,ys[5],middle_right,COLOR_TEXT);}
void drawFooter(){canvas.drawFastHLine(14,195,292,COLOR_DIVIDER);canvas.fillCircle(18,218,4,mqttClient.connected()?COLOR_GREEN:COLOR_RED);drawTextSmall("MQTT",28,218,middle_left,COLOR_MUTED);if(currentPage==Page::MAIN){drawTextSmall(getDataStateString(),83,218,middle_left,getDataStateColor());drawTextSmall(getDataAgeString(),306,218,middle_right,COLOR_MUTED);}else drawTextSmall(String("v")+APP_VERSION,306,218,middle_right,COLOR_MUTED);}
void drawScreen(){if(displaySleeping)return;canvas.fillSprite(COLOR_BG);drawHeader();if(currentPage==Page::MAIN)drawMainPage();else drawStatusPage();drawFooter();canvas.pushSprite(0,0);}

void connectWiFi(){Serial.printf("WiFi: connecting to %s\n",WIFI_SSID);WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.persistent(false);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);uint32_t s=millis();while(WiFi.status()!=WL_CONNECTED&&millis()-s<15000UL){delay(250);Serial.print('.');}Serial.println();if(WiFi.status()==WL_CONNECTED){Serial.printf("WiFi: connected IP=%s RSSI=%d dBm\n",WiFi.localIP().toString().c_str(),WiFi.RSSI());}else Serial.println("WiFi: initial connection failed");lastWifiAttemptMs=millis();}
void maintainWiFi(){if(WiFi.status()==WL_CONNECTED)return;uint32_t n=millis();if(n-lastWifiAttemptMs<WIFI_RETRY_INTERVAL_MS)return;lastWifiAttemptMs=n;Serial.println("WiFi: reconnecting...");WiFi.disconnect();WiFi.begin(WIFI_SSID,WIFI_PASSWORD);}
void setupTime(){if(WiFi.status()!=WL_CONNECTED)return;configTzTime(TZ_INFO,NTP_SERVER_1,NTP_SERVER_2,NTP_SERVER_3);struct tm t;for(int i=0;i<20;i++){if(getLocalTime(&t,250)){ntpReady=true;Serial.println("NTP: synchronized");return;}delay(250);}Serial.println("NTP: synchronization pending");}
void updateTimeState(){if(!ntpReady&&isTimeValid()){ntpReady=true;Serial.println("NTP: time became valid");}}

void mqttCallback(char*topic,byte*payload,unsigned int length){if(strcmp(topic,MQTT_TOPIC_ENV))return;JsonDocument doc;auto e=deserializeJson(doc,payload,length);if(e){Serial.printf("JSON: %s\n",e.c_str());return;}float t=doc["temperature"].as<float>(),h=doc["humidity"].as<float>();if(t< -50||t>80||h<0||h>100){Serial.println("MQTT: value out of range");return;}temperature=t;humidity=h;hasSensorData=true;if(isTimeValid())lastDataReceivedAt=time(nullptr);Serial.printf("ENV: %.1f C %.1f %%\n",temperature,humidity);if(!displaySleeping)drawScreen();}
void setupMQTT(){mqttClient.setServer(MQTT_HOST,MQTT_PORT);mqttClient.setCallback(mqttCallback);mqttClient.setBufferSize(512);}
void maintainMQTT(){if(WiFi.status()!=WL_CONNECTED||mqttClient.connected())return;uint32_t n=millis();if(n-lastMqttAttemptMs<MQTT_RETRY_INTERVAL_MS)return;lastMqttAttemptMs=n;bool ok=strlen(MQTT_USERNAME)?mqttClient.connect(MQTT_CLIENT_ID,MQTT_USERNAME,MQTT_PASSWORD):mqttClient.connect(MQTT_CLIENT_ID);if(!ok){Serial.printf("MQTT: failed state=%d\n",mqttClient.state());return;}Serial.println("MQTT: connected");mqttClient.subscribe(MQTT_TOPIC_ENV,0);}
void forceNetworkReconnect(){Serial.println("Network: manual reconnect");mqttClient.disconnect();WiFi.disconnect();delay(100);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);lastWifiAttemptMs=lastMqttAttemptMs=0;}

void registerUserActivity(){lastUserActivityMs=millis();}
void sleepDisplay(){if(displaySleeping)return;displaySleeping=true;M5.Display.setBrightness(0);Serial.println("Display: sleep");}
void wakeDisplay(){displaySleeping=false;lastUserActivityMs=millis();M5.Display.setBrightness(BRIGHTNESS_LEVELS[brightnessIndex]);drawScreen();Serial.println("Display: wake");}
void loadBrightness(){brightnessIndex=preferences.getUChar("brightness",3);if(brightnessIndex>=BRIGHTNESS_LEVEL_COUNT)brightnessIndex=3;}
void cycleBrightness(){brightnessIndex=(brightnessIndex+1)%BRIGHTNESS_LEVEL_COUNT;M5.Display.setBrightness(BRIGHTNESS_LEVELS[brightnessIndex]);preferences.putUChar("brightness",brightnessIndex);Serial.printf("Brightness: %u\n",BRIGHTNESS_LEVELS[brightnessIndex]);}
void buttonAShort(){if(displaySleeping)wakeDisplay();else sleepDisplay();}
void buttonALong(){if(displaySleeping){wakeDisplay();return;}registerUserActivity();cycleBrightness();drawScreen();}
void buttonBShort(){if(displaySleeping){wakeDisplay();return;}registerUserActivity();currentPage=currentPage==Page::MAIN?Page::STATUS:Page::MAIN;drawScreen();}
void buttonBLong(){if(displaySleeping){wakeDisplay();return;}registerUserActivity();}
void buttonCShort(){if(displaySleeping){wakeDisplay();return;}registerUserActivity();currentPage=Page::MAIN;drawScreen();}
void buttonCLong(){if(displaySleeping){wakeDisplay();return;}registerUserActivity();forceNetworkReconnect();drawScreen();}
void processOne(ButtonState&s,bool pressed,uint32_t now,void(*shortFn)(),void(*longFn)()){if(pressed&&!s.previousPressed){s.pressedAt=now;s.longActionDone=false;}if(pressed&&!s.longActionDone&&now-s.pressedAt>=LONG_PRESS_MS){s.longActionDone=true;longFn();}if(!pressed&&s.previousPressed&&!s.longActionDone)shortFn();s.previousPressed=pressed;}
void processButtons(){uint32_t n=millis();processOne(buttonA,M5.BtnA.isPressed(),n,buttonAShort,buttonALong);processOne(buttonB,M5.BtnB.isPressed(),n,buttonBShort,buttonBLong);processOne(buttonC,M5.BtnC.isPressed(),n,buttonCShort,buttonCLong);}

void setup(){auto cfg=M5.config();M5.begin(cfg);M5.Display.setRotation(1);Serial.begin(115200);delay(100);Serial.printf("\n%s v%s\n",APP_NAME,APP_VERSION);Serial.printf("Font: %s\n",HAVE_JETBRAINS_MONO?"JetBrains Mono embedded":"FreeMono fallback (run tools/install_jetbrains_mono.sh)");preferences.begin("envmonitor",false);loadBrightness();M5.Display.setBrightness(BRIGHTNESS_LEVELS[brightnessIndex]);Serial.printf("Heap before canvas: free=%u largest=%u\n",ESP.getFreeHeap(),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));canvas.setColorDepth(8);if(canvas.createSprite(SCREEN_W,SCREEN_H)==nullptr){Serial.println("ERROR: canvas allocation failed");while(true)delay(1000);}canvas.setTextWrap(false);lastUserActivityMs=millis();drawScreen();connectWiFi();setupTime();setupMQTT();maintainMQTT();drawScreen();}
void loop(){M5.update();processButtons();maintainWiFi();if(WiFi.status()==WL_CONNECTED){updateTimeState();maintainMQTT();if(mqttClient.connected())mqttClient.loop();}uint32_t n=millis();if(!displaySleeping&&n-lastUserActivityMs>=DISPLAY_SLEEP_MS)sleepDisplay();if(!displaySleeping&&n-lastDisplayMs>=DISPLAY_INTERVAL_MS){lastDisplayMs=n;drawScreen();}delay(5);}
