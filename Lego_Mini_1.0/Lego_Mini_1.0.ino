
/*Lego Mini App Controlled Lights Project

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY OR FITNESS
FOR A PARTICULAR PURPOSE.  IT MAY INFRINGE THE INTELLECTUAL PROPERTY RIGHTS OF OTHERS
IN WHICH CASE I'D BE IN TROUBLE AS WELL AS YOU BUT I WILL NOT DEFEND OR INDEMNIFY YOU
FOR ANY LOSS YOU MAY INCUR.

WHAT THAT MEANS IS THAT IF THE SOFTWARE DOESN'T WORK, YOU ARE ON YOUR OWN.
SAME IF YOU DIE OR ARE INJURED USING IT, THOUGH I'D BE INTERESTED IN HEARING HOW YOUMANAGED THAT.  
UNLESS YOU STEP ON A LEGO BRICK I SUPPOSE. STILL, THAT WOULDN'T BE MY FAULT, REALLY.

ANY USE OF THIS SOFTWARE MEANS YOU HAVE AGREED TO ALL THAT.  SO THERE.

*/

// ***************************************************************
// Libraries
// ***************************************************************    

#include <ESP8266WiFi.h>
#include <TimeLib.h>
#include <WiFiManager.h>          
#include <FS.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WebSocketsServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <WiFiUdp.h>

// ***************************************************************
// Pin definitions
// ***************************************************************    

#define HL_R     D6
#define HL_L     D4 
#define FL_R   D2
#define FL_L   D3

#define HZ_R   D0
#define MR   D7
#define HZ_L   D5
#define MG   D8
#define MB   D1
#define LDR A0

// ***************************************************************
//  NTP-related definitions and variable
// ***************************************************************

#define SYNC_INTERVAL_SECONDS 3600  // Sync with server every hour
#define NTP_SERVER_NAME "time.nist.gov" // NTP request server
#define NTP_SERVER_PORT  123 // NTP requests are to port 123
#define LOCALPORT       2390 // Local port to listen for UDP packets
#define NTP_PACKET_SIZE   48 // NTP time stamp is in the first 48 bytes of the message
#define RETRIES           20 // Times to try getting NTP time before failing
byte packetBuffer[NTP_PACKET_SIZE];

File fsUploadFile;  //  Temporarily store SPIFFS file

// ***************************************************************
// OTA constants
// ***************************************************************

const char* mdnsName = "LegoMini";  // Domain name for the mDNS responder
const char *OTAName = "ESP8266";      // A name and a password for the OTA service
const char *OTAPassword = "esp8266";

// ***************************************************************
// Misc variables
// ***************************************************************

byte TZoneEEPROMAddress=0; // For time zone storage
bool ledState = LOW;   // ledState used for hazard blinking
bool hazard = false;  // Hazard lights enable
bool rOn = false;     // Right blinkers enable
bool lOn = false;     // Left blinker enable
bool AutoL = false;  // LDR copntrol
bool LDRRead = false; // Limit LDR read to once per minute
bool hourFlash = false;  //Use NTP to flash headlights on the hour
int timeZone;  // Holds timezone
int cycleSpeed = 30;
unsigned long previousMillis = 0; //  For hazards/turn signal blink without delay()
const long interval = 600;      // interval at which to blink (milliseconds)

// ***************************************************************
//  WiFi definitions
// ***************************************************************

#define AP_NAME "LegoMini" // Wifi Manager configurable
#define WIFI_CHK_TIME_SEC 60  // Checks WiFi connection. Resets after this time, if WiFi is not connected
#define WIFI_CHK_TIME_MS  (WIFI_CHK_TIME_SEC * 1000)

// ***************************************************************
//  Instantiate objects
// ***************************************************************

WiFiUDP udp; //For NTP packets
ESP8266WebServer server ( 80 ); 
WebSocketsServer webSocket = WebSocketsServer(81);
WiFiManager wifiManager;


// ***************************************************************
// Setup Functions
// ***************************************************************

void startOTA() { // Start the OTA service
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
    digitalWrite(HL_R, 0);    // turn off all lights
    digitalWrite(HL_L, 0);
    digitalWrite(FL_R, 0);
    digitalWrite(FL_L, 0);
    digitalWrite(HZ_R, 0);
    digitalWrite(HZ_L, 0);
    digitalWrite(MR, 0);
    digitalWrite(MG, 0);
    digitalWrite(MB, 0);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\r\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready\r\n");
}

void startSPIFFS() { // Start the SPIFFS and list all contents
  SPIFFS.begin();                             // Start the SPI Flash File System (SPIFFS)
  Serial.println("SPIFFS started. Contents:");
  {
    delay(1000);
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {                      // List the file system contents
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("\tFS File: %s, size: %s\r\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    Serial.printf("\n");
  }
}


void startWebSocket() { // Start a WebSocket server
  webSocket.begin();                          // start the websocket server
  webSocket.onEvent(webSocketEvent);          // if there's an incomming websocket message, go to function 'webSocketEvent'
  Serial.println("WebSocket server started.");
}

void startMDNS() { // Start the mDNS responder
  MDNS.begin(mdnsName);                        // start the multicast domain name server
  Serial.print("mDNS responder started: http://");
  Serial.print(mdnsName);
  Serial.println(".local");
}

void startServer() { // Start a HTTP server with a file read handler and an upload handler
  server.on("/edit.html",  HTTP_POST, []() {  // If a POST request is sent to the /edit.html address,
    server.send(200, "text/plain", ""); 
  }, handleFileUpload);                       // go to 'handleFileUpload'

  server.onNotFound(handleNotFound);          // if someone requests any other file or page, go to function 'handleNotFound'
                                              // and check if the file exists

  server.begin();                             // start the HTTP server
  Serial.println("HTTP server started.");
}

// ***************************************************************
//  Server handlers
// ***************************************************************

void handleNotFound(){ // if the requested file or page doesn't exist, return a 404 not found error
  if(!handleFileRead(server.uri())){          // check if the file exists in the flash memory (SPIFFS), if so, send it
    server.send(404, "text/plain", "404: File Not Found");
  }
}

bool handleFileRead(String path) { // send the right file to the client (if it exists)
  Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "MiniLights.html";          // If a folder is requested, send the index file
  String contentType = getContentType(path);             // Get the MIME type
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
    if (SPIFFS.exists(pathWithGz))                         // If there's a compressed version available
      path += ".gz";                                         // Use the compressed verion
    File file = SPIFFS.open(path, "r");                    // Open the file
    size_t sent = server.streamFile(file, contentType);    // Send it to the client
    file.close();                                          // Close the file again
    Serial.println(String("\tSent file: ") + path);
    return true;
  }
  Serial.println(String("\tFile Not Found: ") + path);   // If the file doesn't exist, return false
  return false;
}

String formatBytes(size_t bytes) { // convert sizes in bytes to KB and MB
  if (bytes < 1024) {
    return String(bytes) + "B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + "KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes / 1024.0 / 1024.0) + "MB";
  }
}

String getContentType(String filename) { // determine the filetype of a given filename, based on the extension
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

void setHue(int hue) { // Set the RGB LED to a given hue (color) (0° = Red, 120° = Green, 240° = Blue)
  hue %= 360;                   // hue is an angle between 0 and 359°
  float radH = hue*3.142/180;   // Convert degrees to radians
  float rf, gf, bf;
  
  if(hue>=0 && hue<120){        // Convert from HSI color space to RGB              
    rf = cos(radH*3/4);
    gf = sin(radH*3/4);
    bf = 0;
  } else if(hue>=120 && hue<240){
    radH -= 2.09439;
    gf = cos(radH*3/4);
    bf = sin(radH*3/4);
    rf = 0;
  } else if(hue>=240 && hue<360){
    radH -= 4.188787;
    bf = cos(radH*3/4);
    rf = sin(radH*3/4);
    gf = 0;
  }
  int r = rf*rf*1023;
  int g = gf*gf*1023;
  int b = bf*bf*1023;
  
  analogWrite(MR,   r);    // Write the right color to the LED output pins
  analogWrite(MG, g);
  analogWrite(MB,  b);
}


void handleFileUpload(){ // upload a new file to the SPIFFS
  HTTPUpload& upload = server.upload();
  String path;
  if(upload.status == UPLOAD_FILE_START){
    path = upload.filename;
    if(!path.startsWith("/")) path = "/"+path;
    if(!path.endsWith(".gz")) {                          // The file server always prefers a compressed version of a file 
      String pathWithGz = path+".gz";                    // So if an uploaded file is not compressed, the existing compressed
      if(SPIFFS.exists(pathWithGz))                      // version of that file must be deleted (if it exists)
         SPIFFS.remove(pathWithGz);
    }
    Serial.print("handleFileUpload Name: "); Serial.println(path);
    fsUploadFile = SPIFFS.open(path, "w");            // Open the file for writing in SPIFFS (create if it doesn't exist)
    path = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile) {                                    // If the file was successfully created
      fsUploadFile.close();                               // Close the file again
      Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
      server.sendHeader("Location","/success.html");      // Redirect the client to the success page
      server.send(303);
    } else {
      server.send(500, "text/plain", "500: couldn't create file");
    }
  }
}



// ***************************************************************
// NTP
// ***************************************************************

time_t _getNTPTime() {

  // Set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);

  // Initialize values needed to form NTP request
  packetBuffer[0]  = 0xE3;  // LI, Version, Mode
  packetBuffer[2]  = 0x06;  // Polling Interval
  packetBuffer[3]  = 0xEC;  // Peer Clock Precision
  packetBuffer[12] = 0x31;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 0x31;
  packetBuffer[15] = 0x34;

  // All NTP fields initialized, now send a packet requesting a timestamp
  udp.beginPacket(NTP_SERVER_NAME, NTP_SERVER_PORT);
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();

  // Wait a second for the response
  delay(1000);

  // Listen for the response
  if (udp.parsePacket() == NTP_PACKET_SIZE) {
    udp.read(packetBuffer, NTP_PACKET_SIZE);  // Read packet into the buffer
    unsigned long secsSince1900;

    // Convert four bytes starting at location 40 to a long integer
    secsSince1900 =  (unsigned long) packetBuffer[40] << 24;
    secsSince1900 |= (unsigned long) packetBuffer[41] << 16;
    secsSince1900 |= (unsigned long) packetBuffer[42] << 8;
    secsSince1900 |= (unsigned long) packetBuffer[43];

    Serial.println("Got NTP time");

    return secsSince1900 - 2208988800UL +(timeZone*3600);
    Serial.println(hour());
  } else  {
    return 0;
  }
}

// Get NTP time with retries on access failure
time_t getNTPTime() {

  unsigned long result;

  for (int i = 0; i < RETRIES; i++) {
    result = _getNTPTime();
    if (result != 0) {
      return result;
    }
    Serial.println("Problem getting NTP time. Retrying");
    delay(300);
  }
  Serial.println("NTP Problem - Try reset");

  while (true) {};
}

// Initialize the NTP code
void initNTP() {

  // Login succeeded so set UDP local port
  udp.begin(LOCALPORT);

  // Set the time provider to NTP
  setSyncProvider(getNTPTime);

  // Set the interval between NTP calls
  setSyncInterval(SYNC_INTERVAL_SECONDS);
}

// ***************************************************************
// Series of functions to set lights
// ***************************************************************
    
void HLoN() {
digitalWrite(HL_R, HIGH);
digitalWrite(HL_L, HIGH);
digitalWrite(HZ_R, HIGH);
digitalWrite(HZ_L, HIGH);
 }

void HLoFF() {
digitalWrite(HL_R, LOW);
digitalWrite(HL_L, LOW);
digitalWrite(HZ_R, LOW);
digitalWrite(HZ_L, LOW);
}

void FLoN() {
digitalWrite(FL_R, HIGH);
digitalWrite(FL_L, HIGH);
}

void FLoFF() {
digitalWrite(FL_R, LOW);
digitalWrite(FL_L, LOW);
}

void HZoFF() {
digitalWrite(HZ_R, LOW);
digitalWrite(HZ_L, LOW);
}

void RLoN() {
digitalWrite(HZ_R, HIGH);
digitalWrite(HZ_L, HIGH);
}

void RLoFF() {
digitalWrite(HZ_R, LOW);
digitalWrite(HZ_L, LOW);
}


void hazardBlink() {
  if (hazard == true) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      if (ledState == LOW) {   // if the LED is off turn it on and vice-versa:
        ledState = HIGH;
        Serial.println("Hazards On");
      } else {
          ledState = LOW;
          Serial.println("Hazards Off");
        }
      digitalWrite(HZ_R, ledState);   // set the LED with the ledState of the variable:
      digitalWrite(HZ_L, ledState);   // digitalWrite(ledPin, ledState);  
    }
  }
}

void leftBlink()  {
  if (lOn == true) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      if (ledState == LOW) {
        ledState = HIGH;
        Serial.println("L On");
      } else {
          ledState = LOW;
          Serial.println("L Off");
        }
      digitalWrite(HZ_L, ledState);
    }
  }
}

void rightBlink() {
  if (rOn == true) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      if (ledState == LOW) {
        ledState = HIGH;
        Serial.println("R On");
      } else {
          ledState = LOW;
          Serial.println("R Off");
        }
      digitalWrite(HZ_R, ledState);
    }
  }
}

void flashHour() {  // For using lights to flash time on the hour
  if ((minute() == 0) && (hourFlash == false)) {
    Serial.print("HourFlash");
    for (int i = 1; i <= hourFormat12(); i++) {
      HLoN();
      FLoN();
      RLoN();
      delay(800);
      HLoFF();
      FLoFF();
      RLoFF();
      delay(500);
      Serial.print("Hour Flashed:  ");
      Serial.println(hourFormat12());
      hourFlash = true;
    }
  }
  else if (minute() == 1) {
    hourFlash = false;
  }
}

void ldrRead()  {
  if ((second() == 0) && (LDRRead == false)) {
    Serial.print("Reading LDR");
    int v = analogRead(LDR);
    //Serial.print("LDR =  ");
    //Serial.println(v);
   LDRRead = true;
   if ((v < 500) && (AutoL == true)) {
      HLoN();
   }
   else if ((v > 575) && (AutoL == true)) {
      HLoFF();
      Serial.println("Auto Lights off");
   }
 }
  else if (second() == 1) {
    LDRRead = false;
  }
}

// ***************************************************************
// Program Setup
// ***************************************************************

void setup() {
  Serial.println("WiFi setup");
  WiFiManager wifiManager;
  Serial.begin(115200);
  delay(1000);
  Serial.println("\r\n");
  wifiManager.autoConnect("LegoMini"); 
  Serial.println("Connected");
  startOTA();                  // Start the OTA service
  startSPIFFS();               // Start the SPIFFS and list all contents
  startWebSocket();            // Start a WebSocket server
  startMDNS(); 
  startServer();               // Start a HTTP server with a file read handler and an upload handler
  server.onNotFound(handleNotFound);          // if someone requests any other file or page, go to function 'handleNotFound'
                                              // and check if the file exists
  pinMode(HL_R, OUTPUT);    // Set pin modes
  pinMode(HL_L, OUTPUT);
  pinMode(FL_R, OUTPUT);
  pinMode(FL_L, OUTPUT);
  pinMode(HZ_R, OUTPUT);
  pinMode(HZ_L, OUTPUT);
  pinMode(MR, OUTPUT);
  pinMode(MG, OUTPUT);
  pinMode(MB, OUTPUT);
  pinMode(LDR, INPUT); 
  delay(100);
  initNTP();  // Initialize NTP code
  digitalWrite(HL_R, LOW);    // Turn off the Mini lights
  digitalWrite(HL_L, LOW);
  digitalWrite(FL_R, LOW);
  digitalWrite(FL_L, LOW);
  digitalWrite(HZ_R, LOW);
  digitalWrite(HZ_L, LOW);
  digitalWrite(MG, LOW);
  digitalWrite(MR, LOW);
  digitalWrite(MB, LOW);
  AutoL = false;
  EEPROM.begin(512);
  EEPROM.get(TZoneEEPROMAddress, timeZone);
  Serial.print("Time Zone is: ");
  Serial.println(timeZone);
  Serial.println("\nReady!\n");
}

// ***************************************************************
// Program Loop
// ***************************************************************

bool rainbow = false;
unsigned long nextConnectionCheckTime = 0;
unsigned long prevMillis = millis();
int hue = 0;

void loop() {
  webSocket.loop();
  server.handleClient();
  ArduinoOTA.handle();   
  if (millis() > nextConnectionCheckTime) {
    Serial.print("\n\nChecking WiFi... ");
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost. Resetting...");
      ESP.reset();
    } else {
      Serial.println("OK");
    }
    nextConnectionCheckTime = millis() + WIFI_CHK_TIME_MS;
  }
  if  (rainbow) {                               // if the rainbow effect is turned on
    if  (millis() > prevMillis + 10) {          
      if (++hue == 360)                        // Cycle through the color wheel (increment by one degree every 32 ms)
        hue = 0;
      setHue(hue);                            // Set the RGB LED to the right color
      prevMillis = millis();
    }
  }
  flashHour();
  hazardBlink();
  rightBlink();
  leftBlink();
  ldrRead();
}

// ***************************************************************
// End Program Loop
// ***************************************************************

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) { // When a WebSocket message is received
  switch (type) {
    case WStype_DISCONNECTED:             // if the websocket is disconnected
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED: {              // if a new websocket connection is established
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
        rainbow = false;                  // Turn rainbow off when a new connection is established
      }
      break;
    case WStype_TEXT:                     // if new text data is received
      Serial.printf("[%u] get Text: %s\n", num, payload);
      if (payload[0] == '#') {            // we get RGB data
        uint32_t rgb = (uint32_t) strtol((const char *) &payload[1], NULL, 16);   // decode rgb data
        int r = ((rgb >> 20) & 0x3FF);                     // 10 bits per color, so R: bits 20-29
        int g = ((rgb >> 10) & 0x3FF);                     // G: bits 10-19
        int b =          rgb & 0x3FF;                      // B: bits  0-9

        analogWrite(MR,   r);                         // write it to the LED output pins
        analogWrite(MG, g);
        analogWrite(MB,  b);
      } else if (payload[0] == 'O') {                      
          timeZone = atoi((const char *) &payload[1]);   
          EEPROM.begin(512);
          EEPROM.put(TZoneEEPROMAddress, timeZone);
          EEPROM.commit();
          Serial.print("Time Zone written to EEPROM. New TZ is ");
          Serial.println(timeZone);
    
     } else if (payload[0] == 'V') {                      
        int s = atoi((const char *) &payload[1]);
       
        Serial.println(s);
         cycleSpeed = s;
      }  else if (payload[0] == 'A') {                      // the browser sends an A when the rainbow effect is enabled
          rainbow = true;
      } else if (payload[0] == 'B') {                      // the browser sends B when the rainbow effect is disabled
          rainbow = false;
      } else if (payload[0] == 'C') {                      // the browser sends a C to turn the headlights on
          HLoN();
      } else if (payload[0] == 'D') {                      // the browser sends a D to turn the headlights off
          HLoFF();
      } else if (payload[0] == 'E') {                     // the browser sends an E to turn the fog lights on
          FLoN();
      } else if (payload[0] == 'F') {                     // the browser sends an F to turn the fog lights off
          FLoFF();
      } else if (payload[0] == 'G') {                     // the browser sends a G to turn the hazards on
          hazard = true;   
      } else if (payload[0] == 'H') {                      // the browser sends an H to turn the hazards off
          hazard = false;
          HZoFF();
      } else if (payload[0] == 'K') {                      // the browser sends a K to turn the left blinker on
          lOn = true;
          rOn = false;
      } else if (payload[0] == 'L') {                      // the browser sends a L to turn the left blinker off
          lOn = false;
          HZoFF();
      } else if (payload[0] == 'M') {                      // the browser sends an M to turn the right blinker on
          rOn = true;
          lOn = false;
      } else if (payload[0] == 'N') {                     // the browser sends an N to turn the right blinker off
          rOn = false;
          HZoFF();
       } else if (payload[0] == 'R') {                      //  the browser sends an R to enable the autolights
          AutoL = true;
         
      } else if (payload[0] == 'S') {                      // the browser sends an S to disable the autolights
          AutoL = false;
      }
      break;
  }
}


