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
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <BlynkSimpleEsp8266.h>

#define BLYNK_PRINT Serial

#define ONEWIREGPIO 4
#define LEDFREEZEGPIO 5
#define HEATERGPIO 12
#define RELAYGPIO 14

#define UPDATE_PATH         "/firmware"
#define UPDATE_USERNAME     "admin"
#define UPDATE_PASSWORD     "admin"

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
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
File fsUploadFile;

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
    Serial.println("Sending to Blynk...");
    Blynk.virtualWrite(V0, int(roomTemp));
    Blynk.virtualWrite(V1, int(finsTemp));
    Blynk.virtualWrite(V2, currentState);
    Serial.println("Done");
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
    uptimeTicker.start();
  } else {
    wifiConnected = false;
    uptimeTicker.stop();
    Serial.println("wifiManager: failed to connect and hit timeout");
  }
}

String getContentType(String filename) {
	if (httpServer.hasArg("download")) return "application/octet-stream";
	else if (filename.endsWith(".htm")) return "text/html";
	else if (filename.endsWith(".html")) return "text/html";
	else if (filename.endsWith(".css")) return "text/css";
	else if (filename.endsWith(".js")) return "application/javascript";
	else if (filename.endsWith(".json")) return "application/json";
	else if (filename.endsWith(".png")) return "image/png";
	else if (filename.endsWith(".gif")) return "image/gif";
	else if (filename.endsWith(".jpg")) return "image/jpeg";
	else if (filename.endsWith(".ico")) return "image/x-icon";
	else if (filename.endsWith(".xml")) return "text/xml";
	else if (filename.endsWith(".pdf")) return "application/x-pdf";
	else if (filename.endsWith(".zip")) return "application/x-zip";
	else if (filename.endsWith(".gz")) return "application/x-gzip";
	return "text/plain";
}

String formatBytes(size_t bytes){
  if (bytes < 1024){
    return String(bytes)+"B";
  } else if(bytes < (1024 * 1024)){
    return String(bytes/1024.0)+"KB";
  } else if(bytes < (1024 * 1024 * 1024)){
    return String(bytes/1024.0/1024.0)+"MB";
  } else {
    return String(bytes/1024.0/1024.0/1024.0)+"GB";
  }
}

bool handleFileRead(String path) {
	if (path.endsWith("/"))
		path += "index.html";
	String contentType = getContentType(path);
	String pathWithGz = path + ".gz";
	if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
		if (SPIFFS.exists(pathWithGz))
			path += ".gz";
		File file = SPIFFS.open(path, "r");
		size_t sent = httpServer.streamFile(file, contentType);
		file.close();
		return true;
	}
	return false;
}

void handleFileUpload(){
  if(httpServer.uri() != "/edit") return;
  HTTPUpload& upload = httpServer.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile)
      fsUploadFile.close();
  }
}

void handleFileDelete(){
  if(httpServer.args() == 0) return httpServer.send(500, "text/plain", "BAD ARGS");
  String path = httpServer.arg(0);
  if(path == "/")
    return httpServer.send(500, "text/plain", "BAD PATH");
  if(!SPIFFS.exists(path))
    return httpServer.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  httpServer.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate(){
  if(httpServer.args() == 0)
    return httpServer.send(500, "text/plain", "BAD ARGS");
  String path = httpServer.arg(0);
  if(path == "/")
    return httpServer.send(500, "text/plain", "BAD PATH");
  if(SPIFFS.exists(path))
    return httpServer.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if(file)
    file.close();
  else
    return httpServer.send(500, "text/plain", "CREATE FAILED");
  httpServer.send(200, "text/plain", "");
  path = String();
}

void handleFileList() {
  if(!httpServer.hasArg("dir")) {httpServer.send(500, "text/plain", "BAD ARGS"); return;}

  String path = httpServer.arg("dir");
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while(dir.next()){
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir)?"dir":"file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }

  output += "]";
  httpServer.send(200, "text/json", output);
}

void handleNotFound(){
	if(!handleFileRead(httpServer.uri())) {
	  String message = "File Not Found\n\n";
	  message += "URI: ";
	  message += httpServer.uri();
	  message += "\nMethod: ";
	  message += (httpServer.method() == HTTP_GET)?"GET":"POST";
	  message += "\nArguments: ";
	  message += httpServer.args();
	  message += "\n";
	  for (uint8_t i=0; i<httpServer.args(); i++){
	    message += " " + httpServer.argName(i) + ": " + httpServer.arg(i) + "\n";
	  }
	  httpServer.send(404, "text/plain", message);
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

  bool result = SPIFFS.begin();
 	Serial.println("SPIFFS opened: " + result);
	if(result) {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    Serial.printf("\n");

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
  }

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

	httpUpdater.setup(&httpServer, UPDATE_PATH, UPDATE_USERNAME, UPDATE_PASSWORD);

	httpServer.on("/list", HTTP_GET, handleFileList);
	httpServer.on("/edit", HTTP_GET, [](){
    if(!handleFileRead("/edit.html")) httpServer.send(404, "text/plain", "FileNotFound");
  });
	httpServer.on("/edit", HTTP_PUT, handleFileCreate);
	httpServer.on("/edit", HTTP_DELETE, handleFileDelete);
	httpServer.on("/edit", HTTP_POST, [](){ httpServer.send(200, "text/plain", ""); }, handleFileUpload);
	httpServer.onNotFound(handleNotFound);

  httpServer.begin();

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
  httpServer.handleClient();
  uptimeTicker.update();
  Blynk.run();
}
