#include <ESP8266WiFi.h>
#include <SPI.h>

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

const char *mqtt_servername = "mqttserver.local";
IPAddress mqtt_serveraddress;
WiFiClient espClient;
PubSubClient mqtt_client(espClient);
#define MQTT_PUBLISH "lightning/messages"
#define MQTT_SUBSCRIBE "lightning/commands"

long lastMsg = 0;
char msg[80];

IPAddress dns(8, 8, 8, 8);  //Google dns
IPAddress staticIP(111,222,33,55); // required for dns set 
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



// 2019-10-21
//Hex command to send to serial for close relay
byte relON[]  = {0xA0, 0x01, 0x01, 0xA2};
//Hex command to send to serial for open relay
byte relOFF[] = {0xA0, 0x01, 0x00, 0xA1};
//Hex command to send to serial for close relay
byte rel2ON[]  = {0xA0, 0x02, 0x01, 0xA3};
//Hex command to send to serial for open relay
byte rel2OFF[] = {0xA0, 0x02, 0x00, 0xA2};

time_t LastDisconnect = now();
int disconnected = 0;

void publish_mqtt(const char *str) {
  snprintf (msg, 80, "%s", str);
  mqtt_client.publish(MQTT_PUBLISH, msg);
  mqtt_client.loop(); // 2019-11-06
}

// 2019-11-04 PubSubClient
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  // Switch on the modem on receiving command.

  if (strncmp((char *)payload, "modem_on", 8)==0) {
    digitalClockStr("Relay module reconnecting the phone lines ...");
    Serial.println("Reconnecting the phone lines ...");
    Serial.write (relON, sizeof(relON)); 
    delay(100); // give relay cpu some time before the next command
    Serial.write (rel2ON, sizeof(rel2ON)); 
    delay(100); 
    disconnected = 0;           
  }
  else if (strncmp((char*)payload, "modem_off", 9)==0) 
  {
    digitalClockStr("Relay module disconnecting the phone lines ...");
    Serial.println("Disconnecting the phone lines ...");
    Serial.write (relOFF, sizeof(relOFF));
    delay(100);     
    Serial.write (rel2OFF, sizeof(rel2OFF));
    delay(100); 
    disconnected = 1; 
    LastDisconnect = now();
  }
  //else if (length>0)
  //{
    //digitalClockStr((char *)payload);
  //}
}

long lastReconnectAttempt = 0; // 2019-11-05 Non-blocking reconnect

boolean reconnect() {
  Serial.print("Attempting MQTT reconnection...");
  // Create a random client ID
  String clientId = "heong"; // 2019-03-23
  clientId += String(random(0xffff), HEX);
  // Attempt to connect
  if (mqtt_client.connect(clientId.c_str())) {
    Serial.println("connected");
    // Once connected, publish an announcement...
    // client.publish("outTopic", "hello esp8266 mqtt world");
    digitalClockStr("Lightning detector relay module reconnected");
    // publish_mqtt("Lightning detector relay module reconnected\n");
    mqtt_client.subscribe(MQTT_SUBSCRIBE);
    mqtt_client.loop();
  } else {
      Serial.print("failed, rc=");
      Serial.println(mqtt_client.state());
  }
  return mqtt_client.connected();
}

void setup()
{ // 2019-11-12 Try not to mqtt_publish() as it may not be set up yet
  // Serial.begin(9600);
  Serial.begin(115200); // 2019-10-19

  Serial.print("Connecting to ");
  
  Serial.println(WLAN_SSID);

  WiFi.mode(WIFI_OFF); // 2018-12-11 workaround from 2018-10-09
  WiFi.mode(WIFI_STA); // 2018-11-27 remove rogue AP
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
  delay(100);

  // OTA
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname("MQTT4RelayOTA");

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

  //Serial.write (relON, sizeof(relON)); // Connect phone line 
  //delay(100); // give relay cpu some time before the next command
  //Serial.write (rel2ON, sizeof(rel2ON));
  //delay(100);   
}

static int dots = 0;
static int val = 0; 
static int commas = 0;

void loop()
{
   ArduinoOTA.handle(); // 2019-10-22
  
   // 2019-02-24 Check for broken wifi links
   if (WiFi.status() != WL_CONNECTED) {
     Serial.println("No WiFi! Initiating reset ...");
     // Serial.write(relON, sizeof(relON)); // 2019-10-16 
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
      
      digitalClockStr("MQTT loop time");
    }  
  }
  mqtt_client.loop();

    
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
  if (req.indexOf ("/1/on") != -1)
  {
    Serial.write (relON, sizeof(relON)); 
    val |= 0x01; // if you want feedback see below
    s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>\r\nStatus is ";
    //s += (val)?"on":"off";
    s += val; // 2018-10-25
    s += "</html>\n";
  } else if (req.indexOf ("/1/off") != -1) {
      Serial.write (relOFF, sizeof(relOFF)); 
      val &= 0xfe; // if you want feedback
      s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>\r\nStatus is ";
      //s += (val)?"on":"off";
      s += val; // 2018-10-25
      s += "</html>\n";
  } else if (req.indexOf ("/2/on") != -1) {
      Serial.write (rel2ON, sizeof(rel2ON)); 
      val |= 0x02; // if you want feedback see below
      s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>\r\nStatus is ";
      //s += (val)?"on":"off";
      s += val; // 2018-10-25
      s += "</html>\n";
  } else if (req.indexOf ("/2/off") != -1) {
      Serial.write (rel2OFF, sizeof(rel2OFF)); 
      val &= 0xfd; // if you want feedback
      s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE HTML>\r\n<html>\r\nStatus is ";
      //s += (val)?"on":"off";
      s += val; // 2018-10-25
      s += "</html>\n";
  } else if (req.indexOf ("/modem/on") != -1) {
      Serial.write (relON, sizeof(relON));
      delay(100);     
      Serial.write (rel2ON, sizeof(rel2ON));
      disconnected = 0; 
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
      Serial.write (relOFF, sizeof(relOFF));
      delay(100);     
      Serial.write (rel2OFF, sizeof(rel2OFF));
      delay(100);
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
  delay (10);
  client.stop(); // 2019-02-24
  Serial.println("Client disonnected");
  delay(10);   
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
