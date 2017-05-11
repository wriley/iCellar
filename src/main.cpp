/*

NodeMCU connections
4     1Wire temp sensors
5     freeze LED (red)
12    heater resistor (software PWM)
14    relay for AC fan

*/

#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <Arduino.h>
#include <Ticker.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <BlynkSimpleEsp8266.h>

#define BLYNK_PRINT Serial

#define ONEWIREGPIO 4
#define LEDFREEZEGPIO 5
#define HEATERGPIO 12
#define RELAYGPIO 14

uint8_t ledState = 0;
uint8_t numDevices = 0;
float roomTemp;
float finsTemp;
Ticker blinkTicker;
Ticker logicTicker;
Ticker uptimeTicker;
DeviceAddress roomThermometer, finsThermometer;
OneWire oneWire(ONEWIREGPIO);
DallasTemperature sensors(&oneWire);
WiFiManager wifiManager;
bool needToHeatProbe = false;
bool finsAreFrozen = false;
float setPoint = 50.0;
float deadband = 5.0;
uint8_t blowingCounter;
uint8_t fanRunTime = 2;
uint8_t frozenCounter;
uint8_t thawTime = 5;
bool wifiConnected = false;

char blynk_token[32] = "YOUR_BLYNK_TOKEN";

enum airconState {
  ERROR=-1,
  OFF,
  COOLING,
  BLOWING,
  FROZEN
};

airconState currentState;

void uptimeCallback() {
  Blynk.virtualWrite(V5, millis() / 1000);
}

void blinkCallback() {
  if(ledState == 0) {
    ledState = 1;
  } else {
    ledState = 0;
  }
  digitalWrite(LED_BUILTIN, ledState);
}

void readSensors() {
  Serial.println("Requesting temperatures...");
  sensors.requestTemperatures();
  Serial.println("Done");
  roomTemp = DallasTemperature::toFahrenheit(sensors.getTempC(roomThermometer));
  finsTemp = DallasTemperature::toFahrenheit(sensors.getTempC(finsThermometer));
}

void setFreezeLED(uint8_t state) {
  digitalWrite(LEDFREEZEGPIO, state);
}

void setRelay(uint8_t state) {
  digitalWrite(RELAYGPIO, state);
}

void setHeater(float v) {
  int iv = int(v * 255);
  Serial.print("Setting PWM: ");
  Serial.print(v);
  Serial.print("  analog value ");
  Serial.println(iv);
  analogWrite(HEATERGPIO, iv);
}

void logicCallback() {
  readSensors();

  Serial.println("In logicCallback");
  Serial.print("roomTemp: ");
  Serial.print(roomTemp);
  Serial.print("  finsTemp: ");
  Serial.print(finsTemp);
  Serial.print("  currentState: ");
  Serial.println(currentState);

  if(wifiConnected) {
    Blynk.virtualWrite(V0, int(roomTemp));
    Blynk.virtualWrite(V1, int(finsTemp));
    Blynk.virtualWrite(V2, currentState);
  }

  switch(currentState) {
    case ERROR:
      setHeater(0.0);
      setRelay(0);
      break;
    case OFF:
      setHeater(0.0);
      setRelay(0);
      if(roomTemp <= 68.0 && roomTemp > setPoint) {
        currentState = COOLING;
      }
      break;
    case COOLING:
      setHeater(0.75);
      setRelay(1);
      if(roomTemp <= setPoint || finsTemp <= 32.0) {
        currentState = BLOWING;
      }
      break;
    case BLOWING:
      setHeater(0.0);
      setRelay(1);
      if(blowingCounter++ > fanRunTime) {
        currentState = OFF;
      }
      break;
    case FROZEN:
      setHeater(0.0);
      setRelay(1);
      if(frozenCounter++ > thawTime) {
        currentState = OFF;
      }
      break;
  }
}

void wifiConnect() {
  if (wifiManager.autoConnect()) {
    wifiConnected = true;
    Blynk.config(blynk_token);
    Blynk.connect();
  } else {
    wifiConnected = false;
    Serial.println("wifiManager: failed to connect and hit timeout");
  }
}

// main setup
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("");
  Serial.println("Beginning setup");

	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, ledState);

	// setup GPIO inputs

	// setup GPIO outputs
	pinMode(LEDFREEZEGPIO, OUTPUT);
  pinMode(RELAYGPIO, OUTPUT);

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          if(json.containsKey("blynk_token")) {
            strcpy(blynk_token, json["blynk_token"]);
          }
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read

  Serial.print("Blynk token: ");
  Serial.println(blynk_token);

  // setup 1Wire
  sensors.begin();
  Serial.println("Locating 1Wire devices...");
  Serial.print("Found ");
  numDevices = sensors.getDeviceCount();
  Serial.print(numDevices, DEC);
  Serial.println(" devices.");
  if(numDevices != 2) {
    Serial.print("!!!ERROR!!! Expected 2 1Wire devices but found ");
    Serial.println(numDevices, DEC);
  }
  if (!sensors.getAddress(roomThermometer, 0)) Serial.println("Unable to find address for Device 0");
  if (!sensors.getAddress(finsThermometer, 1)) Serial.println("Unable to find address for Device 1");

  if(numDevices == 2) {
    currentState = OFF;
  } else {
    currentState = ERROR;
  }

  // setup tickers
  blinkTicker.setCallback(blinkCallback);
  blinkTicker.setInterval(500);
  blinkTicker.start();

  logicTicker.setCallback(logicCallback);
  logicTicker.setInterval(60000);
  logicTicker.start();

  uptimeTicker.setCallback(uptimeCallback);
  uptimeTicker.setInterval(1000);

  wifiManager.setTimeout(180);
  wifiConnect();

  Serial.println("Done with setup, entering main loop");

  // call once at start instead of waiting 1 minute for first Ticker fire
  logicCallback();
}

void loop() {
  blinkTicker.update();
  logicTicker.update();

  if(!wifiConnected) {
    wifiConnect();
  }
  uptimeTicker.update();
  Blynk.run();
}
