/*
  LightningDetector.pde - AS3935 Franklin Lightning Sensor™ IC by AMS library demo code
  Copyright (c) 2012 Raivis Rengelis (raivis [at] rrkb.lv). All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  2019-12-21 Isolated relay module into separate ESP-01S wifi CPU to contain
  lightning damage. Relay code is ESP01MQTT2RelayOTA.ino 
*/
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <AS3935.h>
#include <ESP8266mDNS.h> // 2019-10-22
#include <WiFiUdp.h> // Needed for NTP as well as mDNSResolver
#include <ArduinoOTA.h>
#include <TimeLib.h>

#include <mDNSResolver.h> // Also needs WiFiUdp.h
#include <PubSubClient.h>
using namespace mDNSResolver;
WiFiUDP mdns;             // 2019-11-04
Resolver resolver(mdns);

#define WLAN_SSID       "MySSID"
#define WLAN_PASS       "MySecretPassword"        
extern "C" {
  #include <user_interface.h>                    // to detect cold starts
}

const int led =LED_BUILTIN; // ESP8266 Pin to which onboard LED is connected
const int nodemcu_led = D0; // GPIO 16
unsigned long previousMillis = 0;  // will store last time LED was updated
const long interval = 500;  // interval at which to blink (milliseconds)
int ledState = LOW;  // ledState used to set the LED

const char *mqtt_servername = "mqttserver.local";
IPAddress mqtt_serveraddress;
WiFiClient espClient;
PubSubClient mqtt_client(espClient);
#define MQTT_PUBLISH "lightning/messages"
#define MQTT_COMMANDS "lightning/commands"

long lastMsg = 0;
char msg[80];

IPAddress dns(8, 8, 8, 8);  //Google dns
IPAddress staticIP(111,222,33,44); // required for dns set 
IPAddress gateway(111,222,33,1);    
IPAddress subnet(255,255,255,0);
// Create an instance of the server
// specify the port to listen on as an argument
WiFiServer server(8080);

// NTP Servers:
static const char ntpServerName[] = "pool.ntp.org"; // 2019-03-24 from us.pool.ntp.org
const int timeZone = +8;     // Malaysia Time Time
WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
time_t getNtpTime();
void digitalClockDisplay();
void digitalClockStr();
void printDigits(int digits);
void sendNTPpacket(IPAddress &address);

void printAS3935Registers();

// Function prototype that provides SPI transfer and is passed to
// AS3935 to be used from within library, it is defined later in main sketch.
// That is up to user to deal with specific implementation of SPI
// Note that AS3935 library requires this function to have exactly this signature
// and it can not be member function of any C++ class, which happens
// to be almost any Arduino library
// Please make sure your implementation of choice does not deal with CS pin,
// library takes care about it on it's own
byte SPItransfer(byte sendByte);

// Iterrupt handler for AS3935 irqs
// and flag variable that indicates interrupt has been triggered
// Variables that get changed in interrupt routines need to be declared volatile
// otherwise compiler can optimize them away, assuming they never get changed
void AS3935Irq();
volatile int AS3935IrqTriggered;

// First parameter - SPI transfer function, second - Arduino pin used for CS
// and finally third argument - Arduino pin used for IRQ
// It is good idea to chose pin that has interrupts attached, that way one can use
// attachInterrupt in sketch to detect interrupt
// Library internally polls this pin when doing calibration, so being an interrupt pin
// is not a requirement

#define IRQpin 4
#define CSpin 15

AS3935 AS3935(SPItransfer,CSpin,IRQpin);

time_t LastDisconnect = now();
int disconnected = 0;

void publish_mqtt(const char *str) {
  snprintf (msg, 80, "%s", str);
  mqtt_client.publish(MQTT_PUBLISH, msg);
  mqtt_client.loop(); // 2019-11-06
}

// 2019-11-04 PubSubClient. 2019-12-23 not really used as there is no subscribe
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  // Switch on the LED if an 1 was received as first character
  if (strncmp((char *)payload, "modem_on", 8)==0) {
    digitalClockStr("AS3935 setup reads that phone is connected ...");
    Serial.println("AS3935 setup reads that phone is connected ...");
    mqtt_client.loop();
    disconnected = 0;  
    digitalWrite(led, HIGH);  // 2020-04-06
    digitalWrite(nodemcu_led, HIGH);               
  }
  else if (strncmp((char *)payload, "modem_off", 9)==0) 
  {
    digitalClockStr("AS3935 setup reads that phone is disconnected ...");
    Serial.println("AS3935 setup reads that phone is disconnected ...");
    mqtt_client.loop();
    disconnected = 1; 
    LastDisconnect = now();
  }
  mqtt_client.unsubscribe(MQTT_COMMANDS); // 2019-12-27. Important, otherwise endless loop
}

long lastReconnectAttempt = 0; // 2019-11-05 Non-blocking reconnect

boolean reconnect() {
  Serial.print("Attempting MQTT reconnection...");
  // Create a random client ID
  String clientId = "MyAccount"; // 2019-03-23
  clientId += String(random(0xffff), HEX);
  // Attempt to connect
  if (mqtt_client.connect(clientId.c_str())) {
    Serial.println("connected");
    publish_mqtt("AS3935 lightning detector reconnected\n");
    mqtt_client.loop();
  } else {
      Serial.print("failed, rc=");
      Serial.println(mqtt_client.state());
  }
  return mqtt_client.connected();
}

int mqtt_retries = 5; // 2019-12-27 on reset try to connect to mqtt 5 times

void setup()
{ // 2019-11-12 Try not to mqtt_publish() as it may not be set up yet

  pinMode(led, OUTPUT); // 2020-04-06
  pinMode(nodemcu_led, OUTPUT);
  
  // Serial.begin(9600);
  Serial.begin(115200); // 2019-10-19

  Serial.print("Connecting to ");
  
  Serial.println(WLAN_SSID);

  WiFi.mode(WIFI_OFF); // 2018-12-11 workaround from 2018-10-09
  WiFi.mode(WIFI_STA); // 2018-11-27 remove rogue AP
  WiFi.setSleepMode(WIFI_NONE_SLEEP); // 2020-03-18 nessun dorma. none shall sleep
  WiFi.config(staticIP, dns, gateway, subnet);
  WiFi.begin(WLAN_SSID, WLAN_PASS); // 2018-12-11 changed to after WIFI_STA
  while (WiFi.waitForConnectResult() != WL_CONNECTED) { // 2019-10-17
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  randomSeed(micros()); // 2019-11-04
  
  Serial.println();

  Serial.println("WiFi connected");
  Serial.println("IP address: "); 
  Serial.println(WiFi.localIP());

   // Start the server
  server.begin();

  rst_info *rinfo;
  rinfo = ESP.getResetInfoPtr();
  Serial.println(String("ResetInfo.reason = ") + (*rinfo).reason);
  // if ((*rinfo).reason == 0)  // power up    
  // There is a problem: 'int disconnected' needs to be saved as well. Until this is done do
  // not apply cold-start code
  // It would be nice to read the relays states from the 2nd cpu  

  
  // OTA
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname("AS3935MQTTOTA");

  // No authentication by default
  ArduinoOTA.setPassword("MyIoTpassword");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

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
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP()); 
    
  
  // first begin, then set parameters
  SPI.begin();
  // NB! chip uses SPI MODE1
  SPI.setDataMode(SPI_MODE1);
  // NB! max SPI clock speed that chip supports is 2MHz,
  // but never use 500kHz, because that will cause interference
  // to lightning detection circuit
  SPI.setClockDivider(SPI_CLOCK_DIV16);
  // and chip is MSB first
  SPI.setBitOrder(MSBFIRST);
  // reset all internal register values to defaults
  AS3935.reset();
  // and run calibration
  // if lightning detector can not tune tank circuit to required tolerance,
  // calibration function will return false
  
  
  if(!AS3935.calibrate())
    Serial.println("Tuning out of range, check your wiring, your sensor and make sure physics laws have not changed!");  

  outputCalibrationValues();
  recalibrate();

  AS3935.setNoiseFloor(7); // 2020-04-19 settings are slightly over sensitive
  AS3935.setSpikeRejection(5); 
  AS3935.setWatchdogThreshold(2); // Need ferrite bead, otherwise 4
  
  outputCalibrationValues();
  recalibrate();

  // since this is demo code, we just go on minding our own business and ignore the fact that someone divided by zero

  // first let's turn on disturber indication and print some register values from AS3935
  // tell AS3935 we are indoors, for outdoors use setOutdoors() function
  AS3935.setIndoors();
  //AS3935.setOutdoors();
  // turn on indication of distrubers, once you have AS3935 all tuned, you can turn those off with disableDisturbers()
  AS3935.enableDisturbers();
  //AS3935.disableDisturbers();
  printAS3935Registers();
  AS3935IrqTriggered = 0; 
  // Using interrupts means you do not have to check for pin being set continiously, chip does that for you and
  // notifies your code
  // demo is written and tested on ChipKit MAX32, irq pin is connected to max32 pin 2, that corresponds to interrupt 1
  // look up what pins can be used as interrupts on your specific board and how pins map to int numbers

  // ChipKit Max32 - irq connected to pin 2
  // attachInterrupt(1,AS3935Irq,RISING);
  // uncomment line below and comment out line above for Arduino Mega 2560, irq still connected to pin 2
  attachInterrupt(digitalPinToInterrupt(IRQpin),AS3935Irq,RISING);

  Udp.begin(localPort);
  Serial.print("Local port: ");
  Serial.println(Udp.localPort());
  Serial.println("waiting for sync");
  setSyncProvider(getNtpTime);
  setSyncInterval(300);

  Serial.print("Resolving ");
  Serial.println(mqtt_servername);
  resolver.setLocalIP(WiFi.localIP());

  mqtt_serveraddress = resolver.search(mqtt_servername);

  if(mqtt_serveraddress != INADDR_NONE) {
    Serial.print("Resolved: ");
    Serial.println(mqtt_serveraddress);
  } 
  else {
    Serial.println("Not resolved");
    Serial.println("Connection Failed! Rebooting in 5s ...");
    delay(5000);
    ESP.restart();
  }

  mqtt_client.setServer(mqtt_serveraddress, 1883); // Use mdns name
  mqtt_client.setCallback(callback);
  lastReconnectAttempt = 0; // 2019-11-05

  // 2019-12-27 blocking reconnects
  while (!mqtt_client.connected() && mqtt_retries--) 
  { 
    long mqtt_now = millis();
    if (mqtt_now - lastReconnectAttempt > 5000) 
    {
      lastReconnectAttempt = mqtt_now;
      Serial.print("setup() mqtt_client.connected() returns false ...");
      Serial.print("setup() Retrying mqtt connect after 5s ...");
      if (reconnect())
      {
        lastReconnectAttempt = 0;
        Serial.println(" setup() mqtt connected!");
      }
    }
    mqtt_client.loop();
    ArduinoOTA.handle(); // 2020-03-24
    delay(3000);  
  }
  if ( mqtt_client.connected() )
  {
    mqtt_client.subscribe(MQTT_COMMANDS); // callback() will unsubscribe after receiving last message
  }
}

static int dots = 0;
static int val = 0; 
static int commas = 0;

void loop()
{
   ArduinoOTA.handle(); // 2019-10-22
  
  // here we go into loop checking if interrupt has been triggered, which kind of defeats
  // the whole purpose of interrupts, but in real life you could put your chip to sleep
  // and lower power consumption or do other nifty things
  if(AS3935IrqTriggered)  
  {
    // reset the flag
    AS3935IrqTriggered = 0;
    // wait 2 ms before reading register (according to datasheet?)
    delay(2);
    // first step is to find out what caused interrupt
    // as soon as we read interrupt cause register, irq pin goes low
    int irqSource = AS3935.interruptSource();
    // returned value is bitmap field, bit 0 - noise level too high, bit 2 - disturber detected, and finally bit 3 - lightning!
    if (irqSource & 0b0001)
    {
      Serial.println("Noise level too high, try adjusting noise floor");
      digitalClockStr("Noise level too high, try adjusting noise floor");
    }  
    if (irqSource & 0b0100)
    {
      digitalClockDisplay();
      Serial.println("Disturber detected");
      digitalClockStr("Disturber detected");
    }
    if (irqSource & 0b1000)
    {
      // need to find how far that lightning stroke, function returns approximate distance in kilometers,
      // where value 1 represents storm in detector's near victinity, and 63 - very distant, out of range stroke
      // everything in between is just distance in kilometers
      int strokeDistance = AS3935.lightningDistanceKm();
      if (strokeDistance == 1)
      {
        digitalClockStr("Storm overhead, watch out! Disconnecting the phone lines ..........");
        delay(100);
        LastDisconnect = now();
        disconnected = 1;
        digitalClockDisplay();
        Serial.println("Storm overhead, watch out!");
        Serial.println("AS3935 disconnecting the phone lines ...");
        digitalClockStr("AS3935 disconnecting the phone lines ...");
        snprintf (msg, 80, "%s", "modem_off"); // 2019-12-21
        mqtt_client.publish(MQTT_COMMANDS, msg, true);
        mqtt_client.loop();
      }  
      if (strokeDistance == 63)
      {
        Serial.println("Out of range lightning detected.");
        digitalClockStr("Out of range lightning detected.");
      }  
      if (strokeDistance < 63 && strokeDistance > 1)
      {
        digitalClockDisplay();
        Serial.print("Lightning detected ");
        Serial.print(strokeDistance,DEC);
        Serial.println(" kilometers away.");

        char publishstr[80];
        sprintf(publishstr, "Lightning detected %d km away", strokeDistance);
        digitalClockStr(publishstr);
        
        if (disconnected && strokeDistance < 5) // do not be in a hurry to reconnect
          LastDisconnect = now();
      }
    }
  }

  if (disconnected) // 2020-04-06 blink leds alternately to indicate disconnect 
  {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) 
    {
      // save the last time you blinked the LED
      previousMillis = currentMillis;
      // if the LED is off turn it on and vice-versa:
      ledState = not(ledState);
      // set the LED with the ledState of the variable:
      Serial.print("Writing to LED: ");
      Serial.println(ledState); 
      digitalWrite(led,  ledState);
      digitalWrite(nodemcu_led,  not(ledState));      
    }  
  }
    
  if (disconnected && ((now() - LastDisconnect) > 1800))
  {
    disconnected = 0;
    digitalWrite(led, HIGH);  // 2020-04-06
    digitalWrite(nodemcu_led, HIGH);               
    
    digitalClockDisplay();
    Serial.println("AS3935 reconnecting the phone lines ...");
    digitalClockStr("AS3935 reconnecting the phone lines ...");
    snprintf (msg, 80, "%s", "modem_on"); // 2019-12-21
    mqtt_client.publish(MQTT_COMMANDS, msg, true);
    mqtt_client.loop();
  }
  
  /*
  if(!AS3935.calibrate()) 
  {
    Serial.println("Tuning out of range, check your wiring, your sensor and make sure physics laws have not changed!");
  } 
  */
  // 2019-02-24 Check for broken wifi links
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi! Initiating reset ...");
    delay(5000);  // 2019-10-14
    ESP.restart();
  }

  // 2019-11-05 nonblocking reconnects
  if (!mqtt_client.connected()) { 
    long mqtt_now = millis();
    if (mqtt_now - lastReconnectAttempt > 5000) 
    {
      lastReconnectAttempt = mqtt_now;
      Serial.print("mqtt_client.connected() returns false ...");
      Serial.print("Retrying after 5s ...");
      if (reconnect())
      {
        lastReconnectAttempt = 0;
        Serial.println(" reconnected!");
      }
    }
  }
  else 
  {
    long mqtt_now = millis();
    if (mqtt_now - lastMsg > 3600000)
    {
      lastMsg = mqtt_now;
      

      //snprintf(publishstr, 80, "MQTT loop time is %ld\n", mqtt_now);
      //publish_mqtt(publishstr);

      //mqtt_client.publish(MQTT_PUBLISH, publishstr);
      
      //snprintf(publishstr, 80, "%02d:%02d:%02d %02d.%02d.%02d %s\n", hour(), minute(), second(), day(), month(), year(), "Woo hoo!");
      //publish_mqtt(publishstr);

      digitalClockStr("MQTT loop time");
      // publish_mqtt(publishstr);
    }  
  }
  mqtt_client.loop();

  ArduinoOTA.handle();   
  // Check if a client has connected
  WiFiClient client = server.available();
  if ( ! client ) {
    return;
  }

  //Wait until the client sends some data
  while ( ! client.available () )
  {
    delay (10); // 2019-02-28 reduced from 100
    Serial.print(".");
    if (commas++ > 5) {
      commas = 0;
      Serial.println("Terminating client connecting without reading");
      delay(20);
      client.stop();
      return;
    }
    mqtt_client.loop(); // 2019-12-22
    ArduinoOTA.handle(); // 2019-12-22      
  }

  Serial.println("new client connect, waiting for data ");
  
  // Read the first line of the request
  String req = client.readStringUntil ('\r');
  client.flush ();
  
  // Match the request
  String s = "";
  // Prepare the response
  if (req.indexOf ("/modem/on") != -1) 
  {
    Serial.println("AS3935 reconnecting the phone lines ...");
    digitalClockStr("AS3935 reconnecting the phone lines ...");
    snprintf (msg, 80, "%s", "modem_on"); // 2019-12-21
    mqtt_client.publish(MQTT_COMMANDS, msg, true);
    mqtt_client.loop();

    disconnected = 0; 
    digitalWrite(led, HIGH);  // 2020-04-06
    digitalWrite(nodemcu_led, HIGH);               
    
    LastDisconnect = now();
    digitalClockDisplay();
    Serial.println("Modem remotely connected");
    digitalClockStr("Modem remotely connected");
    val |= 0x02; // if you want feedback see below
    s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>\r\nModem Status is ";
    //s += (val)?"on":"off";
    s += val; // 2018-10-25
    s += "</html>\n";
  } else if (req.indexOf ("/modem/off") != -1) {
      Serial.println("AS3935 disconnecting the phone lines ...");
      digitalClockStr("AS3935 disconnecting the phone lines ...");
      snprintf (msg, 80, "%s", "modem_off"); // 2019-12-21
      mqtt_client.publish(MQTT_COMMANDS, msg, true);
      mqtt_client.loop();
      ArduinoOTA.handle(); // 2019-12-22

      disconnected = 1; 
      LastDisconnect = now();
      digitalClockDisplay();
      Serial.println("Modem remotely disconnected");
      digitalClockStr("Modem remotely disconnected");
      val |= 0x02; // if you want feedback see below
      s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>\r\nModem Status is ";
      //s += (val)?"on":"off";
      s += val; // 2018-10-25
      s += "</html>\n";
  } else if (req.indexOf("/index.html") != -1) {
    /*
      s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>\r\nStatus is ";
      //s += (val)?"on":"off";
      s += val; // 2018-10-25
      s += " Firmware version: "; // 2019-10-14 
      s += ESP.getSdkVersion();
      s += "</HTML>\n";
      s += "<HTML><body><h1>It works!</h1></body></HTML>";
      */
      s = "HTTP/1.1 200 OK\r\n";
      s += "Content-Type: text/html\r\n";
      s += "Connection: close\r\n";
      s += "\r\n<!DOCTYPE HTML><html>";
      s += "Firmware version:  ";
      s += ESP.getSdkVersion();
      s += "</html>\r\n";
      Serial.println("responded to index.html");
  }    
  else {
    Serial.println("invalid request");
    s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>\r\nInvalid request</html>\n";
  }

  client.flush ();
  
  // Send the response to the client
  client.print (s);
  // delay (10);
  client.stop(); // 2019-02-24
  Serial.println("Client disonnected");
  // delay(10);   
}

void printAS3935Registers()
{
  int noiseFloor = AS3935.getNoiseFloor();
  int spikeRejection = AS3935.getSpikeRejection();
  int watchdogThreshold = AS3935.getWatchdogThreshold();
  int minLightning = AS3935.getMinimumLightnings();
  Serial.print("Noise floor is: ");
  Serial.println(noiseFloor,DEC);
  Serial.print("Spike rejection is: ");
  Serial.println(spikeRejection,DEC);
  Serial.print("Watchdog threshold is: ");
  Serial.println(watchdogThreshold,DEC); 
  Serial.print("Minimum Lightning is: ");
  Serial.println(minLightning,DEC);   
}

// this is implementation of SPI transfer that gets passed to AS3935
// you can (hopefully) wrap any SPI implementation in this
byte SPItransfer(byte sendByte)
{
  return SPI.transfer(sendByte);
}

// this is irq handler for AS3935 interrupts, has to return void and take no arguments
// always make code in interrupt handlers fast and short
void ICACHE_RAM_ATTR AS3935Irq()
{
  AS3935IrqTriggered = 1;
}


void recalibrate() {
  delay(50);
  Serial.println();
  int calCap = AS3935.getBestTune();
  Serial.print("antenna calibration picks value:\t ");
  Serial.println(calCap);
  delay(50);
}

void outputCalibrationValues() {
   // output the frequencies that the different capacitor values set:
  delay(50);
  Serial.println();
  for (byte i = 0; i <= 0x0F; i++) {
    int frequency = AS3935.tuneAntenna(i);
    Serial.print("tune antenna to capacitor ");
    Serial.print(i);
    Serial.print("\t gives frequency: ");
    Serial.print(frequency);
    Serial.print(" = ");
    long fullFreq = (long) frequency*160;  // multiply with clock-divider, and 10 (because measurement is for 100ms)
    Serial.print(fullFreq,DEC);
    Serial.println(" Hz");
    delay(10);
  }
}

int digitalClockStr(const char *message)
{
  char clockstr[80];

  // output digital clock time to char buffer
  snprintf(clockstr, 80, "%02d:%02d:%02d %02d.%02d.%02d %s\n", hour(), minute(), second(), year(), month(), day(), message);
  publish_mqtt(clockstr);
  mqtt_client.loop();
}

void digitalClockDisplay()
{
  // digital clock display of the time
  Serial.print(hour());
  printDigits(minute());
  printDigits(second());
  Serial.print(" ");
  Serial.print(day());
  Serial.print(".");
  Serial.print(month());
  Serial.print(".");
  Serial.print(year());
  // Serial.println();
  // debugV("%d:%d:%d", hour(), minute(), second());
  Serial.print(" ");
}


void printDigits(int digits)
{
  // utility for digital clock display: prints preceding colon and leading 0
  Serial.print(":");
  if (digits < 10)
    Serial.print('0');
  Serial.print(digits);
}

/*-------- NTP code ----------*/

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t getNtpTime()
{
  IPAddress ntpServerIP; // NTP server's ip address

  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  Serial.println("Transmit NTP Request");
  // get a random server from the pool
  WiFi.hostByName(ntpServerName, ntpServerIP);
  Serial.print(ntpServerName);
  Serial.print(": ");
  Serial.println(ntpServerIP);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("Receive NTP Response");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      unsigned long secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
    }
  }
  Serial.println("No NTP Response :-(");
  return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}
