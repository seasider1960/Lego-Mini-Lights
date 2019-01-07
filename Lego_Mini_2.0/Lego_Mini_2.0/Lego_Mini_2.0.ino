
/*LEGO MINI LIGHTS AND GAME PROJECT.

  SOME LEGAL MUMBO JUMBO: THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY OR FITNESS
  FOR A PARTICULAR PURPOSE.  IT MAY OR MAY NOT WORK. IT MAY INFRINGE THE INTELLECTUAL PROPERTY RIGHTS OF OTHERS.
  IT MAY CAUSE INJURY OR DEATH -- TO YOU OR SOMEBODY ELSE. YOU MIGHT LOSE SOME OR ALL
  OF YOUR MONEY BY USING THIS SOFTWARE AND/OR ANY ASSOCIATED HARDWARE. TO THE MAXIMUM EXTENT PERMITTED BY LAW,
  PLUS 15%, I WILL NOT DEFEND OR INDEMNIFY YOU OR ANYBODY ELSE FOR ANY OF THESE THINGS. OR ANYTHING ELSE.

  ANY USE OF THIS SOFTWARE MEANS YOU HAVE AGREED TO ALL THAT.  SO THERE.

  Sources:  Websocket and service functions:  https://tttapa.github.io/ESP8266/Chap01%20-%20ESP8266.html
            Elements of the Game functions:  https://www.hackster.io/Jerepondumie/make-an-arduino-memory-game-73f55e
            Wifi Manager:  https://github.com/tzapu/WiFiManager

            Thanks to the authors of those sources, and of course the library authors, for making their work available.
            Also to to the authors of any code snippets I may have used and now forgotten about.

PIN MAPPING OF NODEMCU TO ARDUINO

D0   = 16;
D1   = 5;
D2   = 4;
D3   = 0;
D4   = 2;
D5   = 14;
D6   = 12;
D7   = 13;
D8   = 15;
D9   = 3;
D10  = 1;

*/

// ***************************************************************
// Libraries
// ***************************************************************    

#include <ESP8266WiFi.h>
#include <TimeLib.h>
#include <Time.h>
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

#define HL_R     D4       // Right headlight (as facing car)
#define HL_L     D6       // Left headlight
#define FL_R   D3         // Right foglight 
#define FL_L   D2         // Left foglight
#define HZ_R   D5         // Right turn signal
#define INT_L   D7        // White interior light
#define HZ_L   D0         // Left turn signal
#define M_L   D8          // RGB flashing interior light
#define BUZZ_PIN   D1     // Horn
#define LDR A0            // Light dependent resistor (for auto lights)

// ***************************************************************
//  NTP-related definitions and variable
// ***************************************************************

#define SYNC_INTERVAL_SECONDS 3600      // Sync with server every hour
#define NTP_SERVER_NAME "time.nist.gov" // NTP request server
#define NTP_SERVER_PORT  123            // NTP requests are to port 123
#define LOCALPORT       2390            // Local port to listen for UDP packets
#define NTP_PACKET_SIZE   48            // NTP time stamp is in the first 48 bytes of the message
#define RETRIES           20            // Times to try getting NTP time before failing
byte packetBuffer[NTP_PACKET_SIZE];

// ***************************************************************
// OTA & MDNS constants
// ***************************************************************

const char* mdnsName = "LegoMini";     // Domain name for the mDNS responder
const char *OTAName = "LegoMini";      // A name and a password for the OTA service
const char *OTAPassword = "LetsMotor";

// ***************************************************************
// Game Variables
// ***************************************************************

bool gameMode = false;        // Toggles between light control and game mode
byte sequence[100];           // Storage for the light sequence
byte sequencePos = 0;         // Holds sequence array position
byte answer[100];             // Storage for player submittal
byte answerPos = 0;           // Holds answer array position
byte curLen = 0;              // Current length of the sequence
byte inputCount = 0;          // The number of times that the player has pressed a (correct) button in a given turn 
bool levelSuccess = false;    // Used to indicate to the program that once the player lost
byte noPins = 6;              // Number of LEDs used in game
byte pins[] = {HZ_L, HL_L, FL_L, FL_R, HL_R, HZ_R}; // Array of the pins connected to LEDs                            
bool showSequence = false;     // Controls flow of game (time for player input or Mini to show new light sequence)   
bool soundOn = true;           // Turns game sounds and input sounds on/off
byte highScoreEEPROMAddress=4; // Stores all-time high score
int  highScore = 1;
String highScoreStr = String(highScore); // Holds high score string for Websocket transmission
//String highScoreNameStr = String("MINI COOPER");
char highScoreNameStr[] = "4:MINI COOPER";
byte highScoreNameEEPROMAddress=14;

// ***************************************************************
// Misc variables
// ***************************************************************jm 

byte TZoneEEPROMAddress=0;                // For time zone storage
byte LDRValueEEPROMAddress=8;             // holds adjustable LDR setting for auto lights
bool lightsOn;                            // Whether lights are on so lights can be returned to correct state after flashing the hour
bool ledState = LOW;                      // ledState used for hazard blinking
bool hazard = false;                      // Hazard lights enable
bool rOn = false;                         // Right blinkers enable
bool lOn = false;                         // Left blinker enable
bool AutoL = false;                       // LDR copntrol
bool LDRRead = false;                     // Limit LDR read to once per minute
bool hourFlash = false;                   // Use NTP to flash headlights on the hour
int timeZone = -5;                        // Holds time zone
int LDRReading = 550;                     // Holds initial LDR reading
String LDRReadingStr = String(LDRReading); // Holds LDR Reading for Websocket transmission
int LDRValue = 550;                       // Holds initial LDR "on" threshold
String LDRValueStr = String(LDRValue);    // Holds LDR Value for Websocket transmission
int LDRValueOff = LDRValue + 75;          // Holds LDR "off" threshold (null zone is to stop lights flickering on and off at threshold)     
String LDRValueOffStr = String(LDRValueOff); // LDR off string
unsigned long previousMillis = 0;         // For hazards/turn signal blink without delay()
const long interval = 500;                // For blinker timing

// ***************************************************************
//  WiFi definitions
// ***************************************************************

#define AP_NAME "LegoMini"    // Wifi Manager configurable
#define WIFI_CHK_TIME_SEC 60  // Checks WiFi connection. Resets after this time, if WiFi is not connected
#define WIFI_CHK_TIME_MS  (WIFI_CHK_TIME_SEC * 1000)

// ***************************************************************
//  Instantiate objects
// ***************************************************************

WiFiUDP udp;                      // For NTP packets
ESP8266WebServer server ( 80 ); 
WebSocketsServer webSocket = WebSocketsServer(81);
WiFiManager wifiManager;
File fsUploadFile;                 // Temporarily store SPIFFS file

// ***************************************************************
// Setup Functions
// ***************************************************************

void startOTA() { // Start the OTA service
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
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

void startSPIFFS() {                          // Start the SPIFFS and list all contents
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

void startWebSocket() {                       // Start a WebSocket server
  webSocket.begin();                         
  webSocket.onEvent(webSocketEvent);          // if there's an incomming websocket message, go to function 'webSocketEvent'
  Serial.println("WebSocket server started.");
}

void startMDNS() { // Start the mDNS responder
  MDNS.begin(mdnsName);                        // Start the multicast domain name server
  Serial.print("mDNS responder started: http://");
  Serial.print(mdnsName);
  Serial.println(".local");
}

void startServer() {                          // Start a HTTP server with a file read handler and an upload handler
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

void handleNotFound(){                                    // if the requested file or page doesn't exist, return a 404 not found error
  if(!handleFileRead(server.uri())){                      // check if the file exists in the flash memory (SPIFFS), if so, send it
    server.send(404, "text/plain", "404: File Not Found");
  }
}

bool handleFileRead(String path) {                        // send the right file to the client (if it exists)
  Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "MiniLights.html";
  else
{
  if ( path.indexOf('.') == -1 ) path += ".html";         // page1 becomes page1.html -- easy way to create multiple web pages
}
  
  // If a folder is requested, send the index file
  String contentType = getContentType(path);              // Get the MIME type
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
    if (SPIFFS.exists(pathWithGz))                        // If there's a compressed version available
      path += ".gz";                                      // Use the compressed verion
    File file = SPIFFS.open(path, "r");                   // Open the file
    size_t sent = server.streamFile(file, contentType);   // Send it to the client
    file.close();                                         // Close the file again
    Serial.println(String("\tSent file: ") + path);
    return true;
  }
  Serial.println(String("\tFile Not Found: ") + path);    // If the file doesn't exist, return false
  return false;
}

String formatBytes(size_t bytes) {                        // convert sizes in bytes to KB and MB
  if (bytes < 1024) {
    return String(bytes) + "B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + "KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes / 1024.0 / 1024.0) + "MB";
  }
}

String getContentType(String filename) {                    // determine the filetype of a given filename, based on the extension
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
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

    return secsSince1900 - 2208988800UL + (timeZone*3600);
    Serial.print("Hour time is: ");
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
// Game functions
// ***************************************************************

// Flashes all the LEDs together
// freq is the blink speed - small number -> fast | big number -> slow
void flash(short freq){
  for(int i = 0; i < 2; i++){
    writeAllPins(HIGH);
    //beep();
    delay(freq);
    writeAllPins(LOW);
    delay(freq);
  }
}

///This function resets all the game variables to their default values

void Reset(){
  flash(500);
  curLen = 0;
  inputCount = 0;
  answerPos = 0;
  sequencePos = 0; 
  showSequence = false;
}

void beep(byte freq){
  tone(BUZZ_PIN, 500, 200);
  delay(freq);
  tone(BUZZ_PIN, 500,200);
  delay(freq);
}

void Lose() {   // For immediate indication that player has pressed incorrect button
  flash(50);
  if (soundOn == true){
  tone(BUZZ_PIN, 800, 500);
  delay(1000);
  tone(BUZZ_PIN, 600, 500);
  delay(1000);
  tone(BUZZ_PIN, 300, 1500);
  }
}

void Win() {    // If player completes current level successfully
  
  if (soundOn == true){
  tone(BUZZ_PIN, 500, 200);
  delay(300);
  tone(BUZZ_PIN, 700, 200);
  delay(300);
  tone(BUZZ_PIN, 800, 1000);
  }
}

void playSequence() { // Plays new randomw sequence for player to remember
  for(int i = 0; i < curLen; i++){
      Serial.print("Seq: ");
      Serial.print(i);
      Serial.print("Pin: ");
      Serial.println(sequence[i]);
      digitalWrite(sequence[i], HIGH);
      delay(500);
      digitalWrite(sequence[i], LOW);
      delay(250);
    } 
}

void DoLoseProcess(){
  Lose();             // Flash all the LEDS quickly (see Lose function)
  delay(1000);
  playSequence();     // Shows the user the last sequence
  delay(1000);
}

void highScoreProcess() { //  A little celebration if new high score reached
  if (soundOn == true)  {
  digitalWrite(M_L, HIGH);
  tone(BUZZ_PIN, 500, 200);
  delay(300);
  tone(BUZZ_PIN, 700, 200);
  delay(300);
  tone(BUZZ_PIN, 800, 200);
  delay(300);
  tone(BUZZ_PIN, 1000, 200);
  delay(300);
  tone(BUZZ_PIN, 1200, 200);
  delay(300);
  tone(BUZZ_PIN, 1400, 2000);
  delay(300);
  digitalWrite(M_L, LOW);
  } 
}

void newHighScore() {
  if (curLen > highScore)  {                      // Function to record new high score
        highScore = curLen;
        highScoreStr = String(highScore);
        webSocket.broadcastTXT("3:" + highScoreStr);
        EEPROM.put(highScoreEEPROMAddress, highScore);
        EEPROM.commit();
        webSocket.broadcastTXT("5:");
        highScoreProcess();
  }
}
  
// ***************************************************************
// Series of functions to set lights
// ***************************************************************


//send the same value to all the LED pins
void writeAllPins(byte val){
  for(byte i = 0; i < noPins; i++){
    digitalWrite(pins[i], val); 
  }
}
    
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
      HLoFF();  // start with lights off
      FLoFF();
      RLoFF();
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

void ldrRead()  {    // Reads LDR and sets lights accordingly
  if ((second() % 5 == 0) && (LDRRead == false)) { // reads once every 5s
    LDRReading = analogRead(LDR);
    Serial.print("LDR Reading =  ");
    Serial.println(LDRReading);
    LDRReadingStr = String(LDRReading);
    webSocket.broadcastTXT("6:" + LDRReadingStr);
    LDRRead = true;                                // stops multiple readings
    if ((LDRReading < LDRValue) && (AutoL == true)) {
      HLoN();
    }
    else if ((LDRReading > LDRValueOff) && (AutoL == true)) {
      HLoFF();
      Serial.println("Auto Lights off");
   }
 }
  else if (second() %5 != 0) {
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
  pinMode(HL_R, OUTPUT);      // Set pin modes
  pinMode(HL_L, OUTPUT);
  pinMode(FL_R, OUTPUT);
  pinMode(FL_L, OUTPUT);
  pinMode(HZ_R, OUTPUT);
  pinMode(HZ_L, OUTPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  pinMode(INT_L, OUTPUT);
  pinMode(M_L, OUTPUT);
  pinMode(LDR, INPUT); 
  delay(100);
  initNTP();  // Initialize NTP code
  digitalWrite(HL_R, LOW);    // Turn off the Mini lights
  digitalWrite(HL_L, LOW);
  digitalWrite(FL_R, LOW);
  digitalWrite(FL_L, LOW);
  digitalWrite(HZ_R, LOW);
  digitalWrite(HZ_L, LOW);
  digitalWrite(M_L, LOW);
  digitalWrite(INT_L, LOW);
  digitalWrite(BUZZ_PIN, LOW);
  AutoL = false;
  Reset();
  EEPROM.begin(512);
  EEPROM.get(TZoneEEPROMAddress, timeZone);
  if (timeZone > 12) {
    timeZone = timeZone - 25;  //So EEPROM doesn't need to handle negative numbers
  }
  Serial.print("Time Zone is: ");
  Serial.println(timeZone);
  Serial.print("Hour time is: ");
  Serial.println (hourFormat12());
  Serial.println(hour());
  Serial.println(hourFormat12()+timeZone);
  EEPROM.get(highScoreEEPROMAddress, highScore);
  highScoreStr = String(highScore);
  Serial.print("High Score is: ");
  Serial.println(highScore);
  EEPROM.get(LDRValueEEPROMAddress, LDRValue);
  Serial.print("New LDR value in EEPROM is: ");
  Serial.print(LDRValue);
  if (LDRValue < 200) {
    LDRValue = 800;
  }
  Serial.print("LDR Threshold is ");
  Serial.println (LDRValue);
  EEPROM.get(12, highScoreNameStr[0]);  //  Not the most efficient but it works!
  EEPROM.get(13, highScoreNameStr[1]);
  EEPROM.get(14, highScoreNameStr[2]);
  EEPROM.get(15, highScoreNameStr[3]);
  EEPROM.get(16, highScoreNameStr[4]);
  EEPROM.get(17, highScoreNameStr[5]);
  EEPROM.get(18, highScoreNameStr[6]);
  EEPROM.get(19, highScoreNameStr[7]);
  EEPROM.get(20, highScoreNameStr[8]);
  EEPROM.get(21, highScoreNameStr[9]);
  EEPROM.get(22, highScoreNameStr[10]);
  EEPROM.get(23, highScoreNameStr[11]);
  EEPROM.get(24, highScoreNameStr[12]);
  Serial.print("High Score Name is: ");
  Serial.println(highScoreNameStr);
  Serial.println("\nReady!\n");
}

// ***************************************************************
// Program Loop
// ***************************************************************

unsigned long nextConnectionCheckTime = 0;
unsigned long prevMillis = millis();

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

  if (gameMode == false) {  // These functions are disabled  when Mini is in game mode
    flashHour();
    hazardBlink();
    rightBlink();
    leftBlink();
    ldrRead();
  }
   if (gameMode == true) {                            // If in game mode...
    if(showSequence == true) {                        // This is wheat happens when the player presses "Show New Light Sequence"
      randomSeed(millis());
      sequence[curLen] = pins[random(0,noPins)];      // Put a new random value in the next position in the sequence
      for (int i= 0; i< curLen; i++) {                // Shuffle the sequence so make things a bit harder!
         int pos = random(curLen);
         int t = sequence[i];   
         sequence[i] = sequence[pos];
         sequence[pos] = t;
       }
      curLen++;                                       // Set the length of the new sequence
      sequencePos = 0;                                // Set sequence position at 0
      answerPos = 0;                                  // Set answer position at 0
      inputCount = 0;                                 // Set input count at 0
      delay(1000);                                    // 1 second for the player to compose themselves
      Serial.print("Current Length is: ");
      Serial.println(curLen);
      Serial.println("Sequence is: ");
      playSequence();                                 // Show the sequence to the player
      if (soundOn == true) {                          // Make a beep before sequence plays
        beep(50);
      }
      showSequence = false;                           // Now it's the player's turn
      levelSuccess = false;                           // This stops player advancing to new level before completing current level
      }
   }    
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
        delay(500);
        webSocket.broadcastTXT("3:" + highScoreStr);  // Populates high score box on new connection
        webSocket.broadcastTXT(highScoreNameStr);// Populates high score name box on new connection
        webSocket.broadcastTXT("6:" + LDRReadingStr);
        webSocket.broadcastTXT("7:" + LDRValueStr);
        webSocket.broadcastTXT("8:" + LDRValueOffStr);
        
      }
      break;
    case WStype_TEXT:                     // if new text data is received
      Serial.printf("[%u] get Text: %s\n", num, payload);
      
       if (payload[0] == 'Z') {                             // Browser is sending time zone data                    
          if (soundOn == true) {
          tone(BUZZ_PIN, 500, 500);
        }
          timeZone = atoi((const char *) &payload[1]);
          if (timeZone < 0) {
          timeZone = timeZone + 25;
        }   
          EEPROM.put(TZoneEEPROMAddress, timeZone);
          EEPROM.commit();
          Serial.print("Adjusted time zone value in EEPROM is ");
        EEPROM.get(TZoneEEPROMAddress, timeZone);
        Serial.println(timeZone); // for validation of correct number in EEPROM
          if (timeZone > 12) {
          timeZone = timeZone - 25;
        }
        Serial.print("Time Zone now in RAM is ");
        Serial.println(timeZone);
        flashHour(); //                                     // Confirms correct time zone set by flashing current hour
      }  else if (payload[0] == 'A') {                      // Browser sends A to turn on interior light                   
          digitalWrite(INT_L, HIGH);
      } else if (payload[0] == 'B') {                       // Browser sends B to turn off interior light
          digitalWrite(INT_L, LOW); 
      }  else if (payload[0] == 'C') {                      // Browser sends C to turn on flashing RGB interior light
          digitalWrite(M_L, HIGH);
      } else if (payload[0] == 'D') {                       // Browser sends D to turn off flashing RGB interior light
          digitalWrite(M_L, LOW); 
      } else if (payload[0] == 'E') {                       // Browser sends a E to turn headlights on
          HLoN();
      } else if (payload[0] == 'F') {                       // Browser sends a F to turn headlights off
          HLoFF();
      } else if (payload[0] == 'G') {                       // Browser sends G to turn foglights on
          FLoN();
      } else if (payload[0] == 'H') {                       // Browser sends H to turn Foglights off
          FLoFF();
      } else if (payload[0] == 'I') {                       // Browser sends I to turn hazards on
          hazard = true;   
      } else if (payload[0] == 'J') {                       // Browser sends an J to turn hazards off
          hazard = false;
          HZoFF();
      } else if (payload[0] == 'K') {                       // Browser sends K to turn left indicator on/right indicator off
          lOn = true;
          rOn = false;
      } else if (payload[0] == 'L') {                       // Browser sends L to turn left blinker off
          lOn = false;
          HZoFF();
      } else if (payload[0] == 'M') {                       // Browser sends M to turn right inidicator on/left indicator off
          rOn = true;
          lOn = false;
      } else if (payload[0] == 'N') {                       // Browser sends N to turn right indicator off
          rOn = false;
          HZoFF();
       } else if (payload[0] == 'O') {                      // Browser sends O to enable autolights
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          AutoL = true;
         
      } else if (payload[0] == 'P') {                       // Browser sends P to disable autolights
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          AutoL = false;
      } else if (payload[0] == 'Q') {                       // Browser sends Q to turn sounds on
            tone(BUZZ_PIN, 500, 200);
          soundOn = true;
      }
      else if (payload[0] == 'R') {                         // Browser sends R to turn sounds off
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          soundOn = false;
  
      } else if (payload[0] == 'S') {                       // Browser sends S to enter game mode
          Reset();
          gameMode = true;
          Serial.print("Mode is: ");
          Serial.println(gameMode);
          Serial.print("Show Sequence is: ");
          Serial.println(showSequence);
          AutoL = false;
          digitalWrite(HL_R, LOW);                          // Start with lights off
          digitalWrite(HL_L, LOW);
          digitalWrite(FL_R, LOW);
          digitalWrite(FL_L, LOW);
          digitalWrite(HZ_R, LOW);
          digitalWrite(HZ_L, LOW);
          digitalWrite(INT_L, LOW);
          digitalWrite(M_L, LOW);
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
      }  else if (payload[0] == 'T') {                      // Browser sends T to quit game mode
          gameMode = false;
          Serial.print("Mode is: ");
          Serial.println(gameMode);
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
      }   else if (payload[0] == 'U') {                     // Browser sends U to "blow" horn
          tone(BUZZ_PIN, 200, 1500);
      }   else if (payload[0] == 'V') {                     // Browser sends V to adjust LDR threshold
          if (soundOn == true)  {
          tone(BUZZ_PIN, 500, 200);
          }
          LDRValue = LDRValue + 25;
          EEPROM.put(LDRValueEEPROMAddress, LDRValue);
          EEPROM.commit();
          EEPROM.get(LDRValueEEPROMAddress, LDRValue);
          Serial.print("New LDR value is: ");
          Serial.print(LDRValue);
          LDRValueStr = String(LDRValue);
          LDRValueOff = LDRValue + 75;
          LDRValueOffStr = String(LDRValueOff);
          webSocket.broadcastTXT("7:" + LDRValueStr);
          webSocket.broadcastTXT("8:" + LDRValueOffStr);
      }   else if (payload[0] == 'W') {                     //  Browser sends W to adjust the LDR threshold the other way
          if (soundOn == true)  {
          tone(BUZZ_PIN, 500, 200);
          }
          LDRValue = LDRValue - 25;
          EEPROM.put(LDRValueEEPROMAddress, LDRValue);
          EEPROM.commit();
          EEPROM.get(LDRValueEEPROMAddress, LDRValue);
          Serial.print("New LDR value is: ");
          Serial.print(LDRValue);
          LDRValueStr = String(LDRValue);
          LDRValueOff = LDRValue + 75;
          LDRValueOffStr = String(LDRValueOff);
          webSocket.broadcastTXT("7:" + LDRValueStr);
          webSocket.broadcastTXT("8:" + LDRValueOffStr);
      } else if (payload[0] == 'a') {                       // Browser sends a to make next player submittal the left headlight
            if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          inputCount++;                                     // Increment input count
          Serial.print("Input count is: ");
          Serial.println(inputCount);
          digitalWrite(HL_L, HIGH);                         // Turn headlight on to confirm player submission
          answer[answerPos] = 12;                           // Place player submission in next answer array position
          delay(250);
          if (answer[answerPos] != sequence[sequencePos]) { // Check whether player submission is correct and if not:
            Serial.println("Wrong!");                       
            webSocket.broadcastTXT("2:");
            DoLoseProcess();
            newHighScore();                                 // Function to record new high score and high scorer       
          }
          else if (answer[answerPos] == sequence[sequencePos] && inputCount == curLen)  {  // If answer correct check if sequence completed..
            levelSuccess = true;
            Serial.println("Read Sequence complete");       
            Win();                                            // Level sucessfuly completed
            webSocket.broadcastTXT("1:");
          } 
          else {                                              // If not, mocw to next sequence and answer array position to await next player input 
            answerPos++;
            sequencePos++;
            Serial.println("Correct!");
          } 
       } else if (payload[0] == 'b') {                        //  Browser sends b to make next player submittal the left foglight
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          inputCount++;
          Serial.print("Input count is: ");
          Serial.println(inputCount);
          digitalWrite(FL_L, HIGH);
          answer[answerPos] = 4;
          delay(250);
          if (answer[answerPos] != sequence[sequencePos]) {
            Serial.println("Wrong!");
            webSocket.broadcastTXT("2:");
            DoLoseProcess();
            newHighScore();     
          }
          else if (answer[answerPos] == sequence[sequencePos] && inputCount == curLen)  {
            levelSuccess = true;
            Serial.println("Read Sequence complete");                 
            Win();                                            
            webSocket.broadcastTXT("1:");
          } 
          else {
            answerPos++;
            sequencePos++;
            Serial.println("Correct!");
          }
      } else if (payload[0] == 'c') {                         // Browser sends c to make next player submittal the right foglight
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          inputCount++;
          Serial.print("Input count is: ");
          Serial.println(inputCount);
          digitalWrite(FL_R, HIGH);
          answer[answerPos] = 0;
          delay(250);
          if (answer[answerPos] != sequence[sequencePos]) {
            Serial.println("Wrong!");
            webSocket.broadcastTXT("2:");
            DoLoseProcess();
            newHighScore();         
          }
          else if (answer[answerPos] == sequence[sequencePos] && inputCount == curLen)  {
            levelSuccess = true;
            Serial.println("Read Sequence complete");                   
            Win();                                        
            webSocket.broadcastTXT("1:");
          } 
          else  {
            answerPos++;
            sequencePos++;
            Serial.println("Correct!");
          }
      }  else if (payload[0] == 'd') {                        // Browser sends d to make next player submittal the right headlight
            if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
           inputCount++;
           Serial.print("Input count is: ");
           Serial.println(inputCount);
           digitalWrite(HL_R, HIGH);
           answer[answerPos] = 2;
           delay(250);
           if (answer[answerPos] != sequence[sequencePos]) {
             Serial.println("Wrong!");
             webSocket.broadcastTXT("2:");
             DoLoseProcess();
             newHighScore();         
          }
          else if (answer[answerPos] == sequence[sequencePos] && inputCount == curLen)  {
            levelSuccess = true;
            Serial.println("Read Sequence complete");               
            Win();                                        
            webSocket.broadcastTXT("1:");
          } 
          else {
            answerPos++;
            sequencePos++;
            Serial.println("Correct!");
          }
      } else if (payload[0] == 'e') {                      //  Browser sends e to make next player submittal the right indicator
            if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          inputCount++;
          Serial.print("Input count is: ");
          Serial.println(inputCount);
          digitalWrite(HZ_R, HIGH);
          answer[answerPos] = 14;
          delay(250);
          if (answer[answerPos] != sequence[sequencePos]) {
             Serial.println("Wrong!");
             webSocket.broadcastTXT("2:");
             DoLoseProcess();
             newHighScore();         
          }
          else if (answer[answerPos] == sequence[sequencePos] && inputCount == curLen)  {
            levelSuccess = true;
            Serial.println("Read Sequence complete");                   
            Win();                                        
            webSocket.broadcastTXT("1:");
          } 
          else {
            answerPos++;
            sequencePos++;
            Serial.println("Correct!");
          }
      } else if (payload[0] == 'f') {                      //  Browser sends f to make next player submittal the leftindicator
            if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          inputCount++;
          Serial.print("Input count is: ");
          Serial.println(inputCount);
          digitalWrite(HZ_L, HIGH);
          answer[answerPos] = 16;
          delay(250);
          if (answer[answerPos] != sequence[sequencePos]) {
             Serial.println("Wrong!");
             webSocket.broadcastTXT("2:");
             DoLoseProcess();
             newHighScore();         
          }
          else if (answer[answerPos] == sequence[sequencePos] && inputCount == curLen)  {
            //readSequence = false;
            levelSuccess = true;
            Serial.println("Read Sequence complete");                  
            Win();                                        
            webSocket.broadcastTXT("1:");
          } 
          else {
            answerPos++;
            sequencePos++;
            Serial.println("Correct!");
          }
      }  else if (payload[0] == 'g') {                       // Browser sends g to indicate player has pressed "Show New Light Sequence" button
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          answerPos = 0;                                    // Reset answer array position
          sequencePos = 0;                                  // Reset sequence array position
          inputCount = 0;                                   // Reset input count
          if (levelSuccess == true || curLen == 0) {        // Stops player moving to next level if already lost = must press new game
            showSequence = true;
          } else  {
            Lose();
          }
      } else if (payload[0] == 'h') {                      // Browser sends h to start new game
          Reset();
          HLoFF();  
          FLoFF();
          RLoFF();
          if (soundOn == true)  {
            tone(BUZZ_PIN, 500, 200);
          }
          Serial.print("Current Length: ");
          Serial.println(curLen);
          delay(100);
      }  else if (payload[0] == 'j') {                      //  Browser sends j to reset lights after indicating player submittal (timer controlled by Javascript function)
            HLoFF();  
            FLoFF();
            RLoFF();
      } else if (payload[0] == 'l') {                      //  Browser sends l if player waits more than 10s between button presses (timer controlled by Javascript function)
           webSocket.broadcastTXT("2:");
           DoLoseProcess();
           Reset();
           HLoFF();  
           FLoFF();
           RLoFF(); 
      } else if (payload[0] == 'p') {                      //  Browser sends p to record new high scorer name
          highScoreNameStr[0] = '4';
          EEPROM.put(12,'4');
          highScoreNameStr[1] = ':';
          EEPROM.put(13,':');
          highScoreNameStr[2] = payload[1];
          EEPROM.put(14,highScoreNameStr[2]);
          highScoreNameStr[3] = payload[2];
          EEPROM.put(15,highScoreNameStr[3]);
          highScoreNameStr[4] = payload[3];
          EEPROM.put(16,highScoreNameStr[4]);
          highScoreNameStr[5] = payload[4];
          EEPROM.put(17,highScoreNameStr[5]);
          highScoreNameStr[6] = payload[5];
          EEPROM.put(18,highScoreNameStr[6]);
          highScoreNameStr[7] = payload[6];
          EEPROM.put(19,highScoreNameStr[7]);
          highScoreNameStr[8] = payload[7];
          EEPROM.put(20,highScoreNameStr[8]);
          highScoreNameStr[9] = payload[8];
          EEPROM.put(21,highScoreNameStr[9]);
          highScoreNameStr[10] = payload[9];
          EEPROM.put(22,highScoreNameStr[10]);
          highScoreNameStr[11] = payload[10];
          EEPROM.put(23,highScoreNameStr[11]);
          highScoreNameStr[12] = payload[11];
          EEPROM.put(24,highScoreNameStr[12]);
          EEPROM.commit();
          Serial.print("New high score name is: ");
          Serial.println(highScoreNameStr);
          Serial.print("New high score name in EEPROM is: ");
          EEPROM.get(12,highScoreNameStr);                  //  Retrieves the whole high score name string
          Serial.println(highScoreNameStr);
       } else if (payload[0] == 'q') {                      //  Browser sends q to reset high score and high score name
          highScoreNameStr[0] = '4';                        // Write the whole string to EEPROM including the Websocket prefix
          EEPROM.put(12,'4');
          highScoreNameStr[1] = ':';
          EEPROM.put(13,':');
          highScoreNameStr[2] = 'M';
          EEPROM.put(14,'M');
          highScoreNameStr[3] = 'I';
          EEPROM.put(15,'I');
          highScoreNameStr[4] = 'N';
          EEPROM.put(16,'N');
          highScoreNameStr[5] = 'I';
          EEPROM.put(17,'I');
          highScoreNameStr[6] = ' ';
          EEPROM.put(18,' ');
          highScoreNameStr[7] = 'C';
          EEPROM.put(19,'C');
          highScoreNameStr[8] = 'O';
          EEPROM.put(20,'O');
          highScoreNameStr[9] = 'O';
          EEPROM.put(21,'O');
          highScoreNameStr[10] = 'P';
          EEPROM.put(22,'P');
          highScoreNameStr[11] = 'E';
          EEPROM.put(23,'E');
          highScoreNameStr[12] = 'R';
          EEPROM.put(24,'R');
          highScore = 0;
          highScoreStr = String(highScore);
          EEPROM.put(highScoreEEPROMAddress, 0);
          EEPROM.commit(); 
       }
      break;
  }
}


