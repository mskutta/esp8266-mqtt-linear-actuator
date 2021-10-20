#ifdef ESP8266 || ESP32
  #define ISR_PREFIX ICACHE_RAM_ATTR
#else
  #define ISR_PREFIX
#endif

#if !(defined(ESP_NAME))
  #define ESP_NAME "linear"
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

// Stepper Motor Control
#include <Tic.h>

// MQTT
#include <PubSubClient.h>

/* WIFI */
char hostname[32] = {0};

/* MQTT */
WiFiClient wifiClient;
PubSubClient client(wifiClient);
const char* broker = "10.81.95.165";
char topicMove[40] = {0};
char topicMoveAndHold[40] = {0};

/* TIC */
TicI2C tic;
int32_t currentPosition;
int32_t lastPosition;
unsigned long positionTimeout = 0;
int32_t targetPosition;
bool setTargetPosition = false;
bool hold = false;

unsigned long currentMillis = 0;
unsigned long nextRun = 0;

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println(F("Config Mode"));
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void reconnect() {
  while (!client.connected()) {
    Serial.println(F("MQTT Connecting..."));
    if (client.connect(hostname)) {
      Serial.println(F("MQTT connected"));
      client.subscribe(topicMove); // "linear##/move"
      client.subscribe(topicMoveAndHold);
    } else {
      Serial.print(F("."));
      delay(1000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  if(strcmp(topic, topicMove) == 0 && length > 0) 
  {
    targetPosition = -atoi((char*)payload);
    setTargetPosition = true;
    hold = false;
  }
  else if(strcmp(topic, topicMoveAndHold) == 0 && length > 0) 
  {
    targetPosition = -atoi((char*)payload);
    setTargetPosition = true;
    hold = true;
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
  Wire.begin(D2, D1); // join i2c bus with SDA=D2 and SCL=D1 of NodeMCU

  delay(20);

  /* LED */
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  /* Function Select */
  Serial.println(ESP_NAME);
  
  /* WiFi */
  sprintf(hostname, "%s-%06X", ESP_NAME, ESP.getChipId());
  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  if(!wifiManager.autoConnect(hostname)) {
    Serial.println(F("WiFi Connect Failed"));
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }

  Serial.println(hostname);
  Serial.print(F("  "));
  Serial.println(WiFi.localIP());
  Serial.print(F("  "));
  Serial.println(WiFi.macAddress());

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
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("End"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) { Serial.println("Auth Failed"); } 
    else if (error == OTA_BEGIN_ERROR) { Serial.println("Begin Failed"); } 
    else if (error == OTA_CONNECT_ERROR) { Serial.println("Connect Failed"); } 
    else if (error == OTA_RECEIVE_ERROR) { Serial.println("Receive Failed"); } 
    else if (error == OTA_END_ERROR) { Serial.println("End Failed"); }
  });
  ArduinoOTA.begin();

  /* TIC */
  // Set the TIC product
  tic.setProduct(TicProduct::T500);
  tic.setStepMode(TicStepMode::Half);
  tic.setMaxAccel(500000);
  tic.setMaxDecel(500000);
  tic.setMaxSpeed(50000000);
  tic.setCurrentLimit(1092);

  tic.haltAndSetPosition(0);
  tic.exitSafeStart();

  Serial.println(F("Ready..."));

  /* MQTT */
  sprintf(topicMove, "%s/move", ESP_NAME);
  sprintf(topicMoveAndHold, "%s/moveandhold", ESP_NAME);
  client.setServer(broker, 1883);
  client.setCallback(callback);

  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
  ArduinoOTA.handle();
  if (!client.connected())
  {
    reconnect();
  }
  client.loop();

  tic.resetCommandTimeout();

  // run once every 100ms
  currentMillis = millis();
  if (nextRun < currentMillis) {
    nextRun = currentMillis + 100;
    
    // Get current position
    currentPosition = tic.getCurrentPosition();

    // Set target position
    if (setTargetPosition == true) {
      setTargetPosition = false;
      if (tic.getEnergized() == false) {
        tic.energize();
        digitalWrite(LED_BUILTIN, LOW);
        tic.haltAndSetPosition(currentPosition);
      }
      tic.setTargetPosition(targetPosition);
      tic.exitSafeStart();
    }

    // Wait to Denergize if reached target position
    if (currentPosition != lastPosition) {
      // Reset 1000ms timeout
      positionTimeout = currentMillis + 1000;
    } else if (currentPosition == lastPosition && positionTimeout < currentMillis) {
      if (currentPosition == targetPosition && hold == false) {
        if (tic.getEnergized() == true) {
          tic.deenergize();
          digitalWrite(LED_BUILTIN, HIGH);
        }
      }
    }

    lastPosition = currentPosition;
  }
}