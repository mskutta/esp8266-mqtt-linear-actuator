#ifdef ESP8266 || ESP32
  #define ISR_PREFIX ICACHE_RAM_ATTR
#else
  #define ISR_PREFIX
#endif

#if !(defined(ESP_NAME))
  #define ESP_NAME "linear"
#endif

#if !(defined(DISPLAY_128X32) || defined(DISPLAY_128X64))
  #define DISPLAY_128X64 1 
#endif

#if !(defined(MAX_POSITION))
  #define MAX_POSITION 8500 
#endif

#include <Arduino.h>

#include <ESP8266WiFi.h> // WIFI support
#include <ArduinoOTA.h> // Updates over the air

// WiFi Manager
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> 

// I2C
#include <Wire.h>

// Display (SSD1306)
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"

// Stepper Motor Control
#include <Tic.h>

// MQTT
#include <PubSubClient.h>

/* Display */
SSD1306AsciiWire oled;
uint8_t rowHeight; // pixels per row.

/* WIFI */
char hostname[32] = {0};

/* MQTT */
WiFiClient wifiClient;
PubSubClient client(wifiClient);
const char* broker = "10.81.95.165";
char topicMove[40] = {0};

/* TIC */
TicSerial tic(Serial);

void configModeCallback (WiFiManager *myWiFiManager) {
  oled.println(F("Config Mode"));
  oled.println(WiFi.softAPIP());
  oled.println(myWiFiManager->getConfigPortalSSID());
}

void reconnect() {
  while (!client.connected()) {
    oled.println(F("MQTT Connecting..."));
    if (client.connect(hostname)) {
      oled.println(F("MQTT connected"));
      client.subscribe(topicMove); // "linear##/move"
    } else {
      oled.print(F("."));
      delay(1000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  oled.print(F("t:"));
  oled.println(topic);
  oled.print(F("p:"));

  for (unsigned int i = 0; i < length; i++) {
    oled.print((char)payload[i]);
  }
  oled.println();

  if(strcmp(topic, topicMove) == 0 && length > 0) 
  {
    int value = atoi((char*)payload);
    if (value > MAX_POSITION) {
      value = MAX_POSITION;
    } else if (value < 0) {
      value = 0;
    }
    tic.setTargetPosition(value);
  }
}

// Delays for the specified number of milliseconds while
// resetting the Tic's command timeout so that its movement does
// not get interrupted by errors.
void delayWhileResettingCommandTimeout(uint32_t ms)
{
  uint32_t start = millis();
  do
  {
    delay(20);
    tic.resetCommandTimeout();
  } while ((uint32_t)(millis() - start) <= ms);
}

void setup() {
  
  /* Serial and I2C */
  Serial.begin(9600);
  Wire.begin(D2, D1); // join i2c bus with SDA=D1 and SCL=D2 of NodeMCU

  delay(1000);

  /* Display */
#if defined DISPLAY_128X64
  oled.begin(&Adafruit128x64, 0x3C);  // initialize with the I2C addr 0x3C (for the 128x64)
#else
  oled.begin(&Adafruit128x32, 0x3C);
#endif
  oled.setFont(System5x7);
  oled.setScrollMode(SCROLL_MODE_AUTO);
  oled.clear();

  /* LED */
  pinMode(LED_BUILTIN, OUTPUT);

  /* Function Select */
  oled.println(ESP_NAME);
  
  /* WiFi */
  sprintf(hostname, "%s-%06X", ESP_NAME, ESP.getChipId());
  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  if(!wifiManager.autoConnect(hostname)) {
    oled.println(F("WiFi Connect Failed"));
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }

  oled.println(hostname);
  oled.print(F("  "));
  oled.println(WiFi.localIP());
  oled.print(F("  "));
  oled.println(WiFi.macAddress());

  /* OTA */
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    oled.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    oled.println(F("End"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    oled.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    oled.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) { oled.println("Auth Failed"); } 
    else if (error == OTA_BEGIN_ERROR) { oled.println("Begin Failed"); } 
    else if (error == OTA_CONNECT_ERROR) { oled.println("Connect Failed"); } 
    else if (error == OTA_RECEIVE_ERROR) { oled.println("Receive Failed"); } 
    else if (error == OTA_END_ERROR) { oled.println("End Failed"); }
  });
  ArduinoOTA.begin();

  /* TIC */
  // Set the TIC product
  tic.setProduct(TicProduct::T500);
  tic.setStepMode(TicStepMode::Microstep2);
  tic.setMaxAccel(500000);
  tic.setMaxDecel(500000);

  // Home
  oled.println(F("Homing..."));
  tic.setCurrentLimit(500);
  tic.setMaxSpeed(10000000);
  tic.haltAndSetPosition(MAX_POSITION);
  tic.setTargetPosition(0);
  tic.exitSafeStart();
  delayWhileResettingCommandTimeout(10000);
  oled.println(F("Homed"));

  // Run
  tic.setCurrentLimit(1000);
  tic.setMaxSpeed(50000000);
  tic.haltAndSetPosition(0);
  tic.exitSafeStart();
  oled.println(F("Running..."));

  /* MQTT */
  sprintf(topicMove, "%s/move", ESP_NAME);
  client.setServer(broker, 1883);
  client.setCallback(callback);
}

void loop() {
  ArduinoOTA.handle();
  if (!client.connected())
  {
    reconnect();
  }
  client.loop();

  tic.resetCommandTimeout();
}