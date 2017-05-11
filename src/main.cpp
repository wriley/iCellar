/*

NodeMCU connections
4     heater on LED (orange)
5     freeze LED (red)
9     1Wire temp sensors
12    heater resistor (PWM)
14    relay for AC fan

*/

#include <Arduino.h>
#include <Ticker.h>
#include <DallasTemperature.h>
#include <OneWire.h>

const uint8_t LEDHEATERGPIO = 4;
const uint8_t LEDFREEZEGPIO = 5;
const uint8_t ONEWIREGPIO = 9;
const uint8_t HEATERGPIO = 12;
const uint8_t RELAYGPIO = 14;

uint8_t ledState = 0;
uint8_t numDevices = 0;
float roomTemp;
float finsTemp;
Ticker blinkTicker;
Ticker logicTicker;
DeviceAddress roomThermometer, finsThermometer;
OneWire oneWire(ONEWIREGPIO);
DallasTemperature sensors(&oneWire);
bool needToHeatProbe = false;
bool finsAreFrozen = false;
float setPoint = 50.0;
float deadband = 5.0;
uint8_t blowingCounter;
uint8_t fanRunTime = 2;
uint8_t frozenCounter;
uint8_t thawTime = 5;

enum airconState {
  ERROR,
  OFF,
  COOLING,
  BLOWING,
  FROZEN
};

airconState currentState;

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

void setHeaterLED(uint8_t state) {
  digitalWrite(LEDHEATERGPIO, state);
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
  if(v > 0.0) {
    setHeaterLED(1);
  } else {
    setHeaterLED(0);
  }
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
	pinMode(LEDHEATERGPIO, OUTPUT);
  pinMode(LEDFREEZEGPIO, OUTPUT);
  pinMode(RELAYGPIO, OUTPUT);

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

  Serial.println("Done with setup, entering main loop");
}

void loop() {
  blinkTicker.update();
  logicTicker.update();
}
