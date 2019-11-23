/*

NodeMCU connections
4     1Wire temp sensors
5     red LED
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
#include "config.h"

#define BLYNK_PRINT Serial

#define ONEWIREGPIO 4
#define LEDREDGPIO 5
#define RELAYGPIO 14

#define UPDATE_PATH         "/firmware"
#define UPDATE_USERNAME     "admin"
// change password for production use
#define UPDATE_PASSWORD     "admin"

uint8_t ledState = 0;
uint8_t numDevices = 0;
float temp1;
float temp2;
Ticker blinkTicker;
Ticker logicTicker;
Ticker uptimeTicker;
DeviceAddress temp1Thermometer;
DeviceAddress temp2Thermometer;
OneWire oneWire(ONEWIREGPIO);
DallasTemperature sensors(&oneWire);
WiFiManager wifiManager;
float setPoint = 90.0;
float hysteresis = 2.0;
bool wifiConnected = false;

ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
File fsUploadFile;
long uptimeSeconds = 0;
String resetReason = ESP.getResetReason();

enum heaterState {
    ERROR=-1,
    OFF,
    HEATING
};

heaterState currentState;

void setRedLED(uint8_t state) {
    digitalWrite(LEDREDGPIO, state);
}

void setRelay(uint8_t state) {
    digitalWrite(RELAYGPIO, state);
}

void uptimeCallback() {
    uptimeSeconds++;
    switch(currentState) {
        case ERROR:
            setRelay(0);
            setRedLED(0);
            break;
        case OFF:
            setRelay(0);
            setRedLED(0);
            break;
        case HEATING:
            setRelay(1);
            setRedLED(1);
            break;
    }
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
    temp1 = DallasTemperature::toFahrenheit(sensors.getTempC(temp1Thermometer));
    temp2 = DallasTemperature::toFahrenheit(sensors.getTempC(temp2Thermometer));
}

void logicCallback() {
    readSensors();

    Serial.println("In logicCallback");
    Serial.print("temp1: ");
    Serial.print(temp1);
    Serial.print("  temp2: ");
    Serial.print(temp2);
    Serial.print("  currentState: ");
    Serial.println(currentState);

    if(wifiConnected) {
        Serial.println("Sending to Blynk...");
        Blynk.virtualWrite(V0, int(temp1));
        Blynk.virtualWrite(V1, int(temp2));
        Blynk.virtualWrite(V2, currentState);
        Blynk.virtualWrite(V5, uptimeSeconds);
        Serial.println("Done");
    }

    if(temp1 <= (setPoint - hysteresis)) {
        currentState = HEATING;
    }

    if (temp1 >= (setPoint + hysteresis)) {
      currentState = OFF;
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
        if(sent > 0) {
          return true;
        } else {
          return false;
        }
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

void handleStatus() {
    StaticJsonDocument<400> doc;
    JsonObject root = doc.to<JsonObject>();
    root["uptime"] = uptimeSeconds;
    root["freeHeap"] = ESP.getFreeHeap();
    root["temp1"] = temp1;
    root["temp2"] = temp2;
    int currentStateVal = currentState;
    root["currentState"] = currentStateVal;
    root["resetReason"] = resetReason;
    char msg[400];
    //char *firstChar = msg;
    //root.printTo(firstChar, sizeof(msg) - strlen(msg));
    serializeJson(doc, msg);
    httpServer.send(200, "text/json", msg);
}


// main setup
void setup() {
    Serial.begin(9600);
    delay(100);
    Serial.println("");
    Serial.println("Beginning setup");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, ledState);

    // setup GPIO inputs

    // setup GPIO outputs
    pinMode(LEDREDGPIO, OUTPUT);
    digitalWrite(LEDREDGPIO, 0);
    pinMode(RELAYGPIO, OUTPUT);
    digitalWrite(RELAYGPIO, 0);

    // setup watchdog
    ESP.wdtDisable();
    ESP.wdtEnable(WDTO_8S);

    // test outputs

    Serial.println("Testing outputs...");
    delay(500);

    Serial.println("Red LED ON");
    setRedLED(1);
    delay(1000);
    setRedLED(0);
    Serial.println("Red LED OFF");

    Serial.println("Relay ON");
    setRelay(1);
    delay(1000);
    setRelay(0);
    Serial.println("Relay OFF");

    delay(1000);

    // end tests


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

                StaticJsonDocument<64> doc;
                DeserializationError error = deserializeJson(doc, configFile);
                if(error) {
                  Serial.println("failed to load json config");
                }

                strcpy(blynk_token, doc["blynk_token"]);
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
    if (!sensors.getAddress(temp1Thermometer, 0)) Serial.println("Unable to find address for Device 0");
    if (!sensors.getAddress(temp2Thermometer, 1)) Serial.println("Unable to find address for Device 1");

    if(numDevices == 2) {
        currentState = OFF;
    } else {
        currentState = ERROR;
    }

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
    httpServer.on("/status", HTTP_GET, handleStatus);
    httpServer.onNotFound(handleNotFound);

    httpServer.begin();

    Serial.println("Done with setup, entering main loop");

    blinkTicker.attach(0.5, blinkCallback);
    logicTicker.attach(30, logicCallback);
    uptimeTicker.attach(1, uptimeCallback);

    logicCallback();
}

void loop() {
    if(!wifiConnected) {
        wifiConnect();
    }
    httpServer.handleClient();
    Blynk.run();
    ESP.wdtFeed();
}
