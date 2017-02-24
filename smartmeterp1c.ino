
/*
 * smartmeterp1 - Read P1 telegram from smartmeter and send it to a central system using MQTT protocol over Ethernet
 *
 *  An Ethernet shield is required to send the data.
 *
 *  MQTT topics
 *  * settings/configuration 
 *    - sm1/set/datagrams        - include number of received messages from smart meter
 *    - sm1/set/units            - include units of the sensor values
 *    - sm1/set/p1-telegram      - show P1 telegram on serial interface
 *  * sensor data
 *    - power/cumulative-usage-1 - cumulative electricity usage, tariff 1
 *    - power/cumulative-usage-2 - cumulative electricity usage, tariff 2
 *    - power/actual-usage       - actual electricity usage
 *    - power/tariff             - current electricity tariff
 *    - gas/cumulative-usage     - cumulative gas usage
 *    - sm1/header               - smart meter type 
 *  
 * Interface connections:
 *
 *   Arduino   Smartmeter
 *   D5        2 RTS         (geel)
 *   D8        5 TXD (data)  (zwart)
 *   GND       3 GND         (groen)
 *
 *   D3        LED blue      on during startup, blinks when data is received
 *   D6        nc
 *
 * Open items:
 * - Use DNS to determine broker name (instead of fixed IP address)
 *   Make broker name/addres configurable
 *
 * Revision history
 * Version Date      Decription
 * 0.20    20160115  Added username/passeord authentication (to prevent anonymous logons)
 * 0.21    20160124  Tried to add host name of device (does not work yet) 
 * 
 */

#include <Time.h>
#include <Timezone.h>    // https://github.com/JChristensen/Timezone
#include <SPI.h>
#include <Dns.h>
#include <Ethernet.h>
#include <EthernetClient.h>
#include <EthernetUdp.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>
#include <avr/pgmspace.h>

#define DEBBUG  0
#define MQTT    1

#if DEBUG==1
#define debugln(s)    Serial.println(s)
#deifne debug(s)      Serial.print(s)
#else
#define debugln(s)  
#define debug(s)      
#endif

//#define info(s)     Serial.print(s)
//#define infoln(s)   Serial.println(s)

#define PinLED             6
#define PinRequest         5    // Pin to signal Smartmeter to send P1 telegram (was 4)

#define LineFeed        0x0A
#define MaxNrOfChars     480    // My current P1 telegram is 450 characters

SoftwareSerial smartmeter(8, 9, true ); // RX, TX, inverted
// Ethernet (including SD card): D2, D4, D10..D13, A0, A1 already used

#define TimeZoneOffset  0L   // the offset in seconds to your local time;

#define NTP_UDPPort    123

EthernetUDP ntp;

typedef struct {
    const char *OBISRef;
    const char *topic;
    const int   fmt;
    const bool  next;
} OBISTopic;

#define P1_FmtPowerUsage   1
#define P1_FmtTariff       2
#define P1_FmtGasUsage     3
#define P1_FmtHeader       4
#define P1_FmtPowerActual  5
#define P1_FmtGasTimestamp 6

const char ref1[] PROGMEM  = "1-0:1.8.1";
const char ref2[] PROGMEM  = "1-0:1.8.2";
const char ref3[] PROGMEM  = "1-0:1.7.0";
const char ref4[] PROGMEM  = "0-0:96.14.0";
const char ref5[] PROGMEM  = "0-2:24.3.0";
const char ref6[] PROGMEM  = "/";

const char topic1[] PROGMEM = "power/cumulative-usage-1";
const char topic2[] PROGMEM = "power/cumulative-usage-2";
const char topic3[] PROGMEM = "power/actual-usage";
const char topic4[] PROGMEM = "power/tariff";
const char topic5[] PROGMEM = "gas/cumulative-usage";
const char topic6[] PROGMEM = "sm1/header";

const OBISTopic ots[] = { 
  { ref1, topic1, P1_FmtPowerUsage,  false },
  { ref2, topic2, P1_FmtPowerUsage,  false },
  { ref3, topic3, P1_FmtPowerActual, false },
  { ref4, topic4, P1_FmtTariff,      false },
  { ref5, topic5, P1_FmtGasUsage,    true  },
  { ref6, topic6, P1_FmtHeader,      false }
};

#define NrOfOBISTopics (sizeof(ots) / sizeof(OBISTopic))

boolean   isLedOn = false;

char inputBuffer[MaxNrOfChars];
bool messageStartReceived, messageEndReceived;
bool updateNrOfDatagrams = false;
//bool sendChangesOnly = false;
bool sendP1Telegram = false;
bool sendUnits = false;
int  writeIndex;
int  lineStartIndex;

long nrOfDatagrams = 0;

#if MQTT==1
byte myMac[6] = { 0x90, 0xA2, 0xDA, 0x0d, 0x5c, 0x21 }; 

IPAddress myIPaddress(192,168,178,70);
IPAddress myDNS(208, 67, 222, 222);
IPAddress myGateway(192,168,178, 1);
IPAddress mySubnet(255, 255, 255, 0);

IPAddress ntpIP(72, 251, 251, 11);        // This is the default fall back. Will try to lookup IP adress from pool.ntp.org

//byte mqttBroker[] = {192, 168, 178, 242};
const char mqttServer[] PROGMEM = "mqttbroker";

#define MQTT_Port       1883
#define UDP_LocalPort   8888
#define NTP_PacketSize    48 // NTP time stamp is in the first 48 bytes of the message

byte packetBuffer[NTP_PacketSize]; //buffer to hold incoming and outgoing packets 

void cbReceiveMessage(char *, byte *, unsigned int);

EthernetClient ethClient; 
//PubSubClient   mqttClient(mqttBroker, MQTT_Port, cbReceiveMessage, ethClient);
// #$#new
PubSubClient   mqttClient(ethClient);
#endif

int freeRam () {
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}

void printFreeRAM() {
   Serial.print(F("Free RAM: "));
   Serial.println(freeRam());
}

void setup() {
   pinMode(PinLED, OUTPUT);
   ledOn();
   
   // initialize (software) serial interface for reading P1 telegram
   smartmeter.begin(9600);
   delay(1);
   
   // Use serial port for logging informational and debug messages
   Serial.begin(9600);
   Serial.println(F("P1 reader v0.21 20160124\n"));
   printFreeRAM();

   // Set pin 4 high to enable the SD Card
   pinMode(4, OUTPUT);
   digitalWrite(4, HIGH);

#if MQTT==1
   // initialize ethernet interface for publishing data
   Ethernet.begin(myMac, myIPaddress, myDNS, myGateway, mySubnet);
//   Ethernet.hostName("smr1");    not a generic function
   Serial.println(F("Ethernet initialized."));
   Serial.print(F("IP address: ")); Serial.println(Ethernet.localIP());

   mqttClient.setServer(mqttServer, MQTT_Port);
   mqttClient.setCallback(cbReceiveMessage);

   DNSClient dns;
   dns.begin(Ethernet.dnsServerIP());
   if (dns.getHostByName("pool.ntp.org",ntpIP) == 1) {
      Serial.print(F("NTP IP: "));
      Serial.println(ntpIP);
   }
   else Serial.println(F("DNS lookup failed"));

   ntp.begin(UDP_LocalPort);  // is this correct?
   setSyncProvider(getNtpTime);
   while(timeStatus()== timeNotSet)   
     ; // wait until the time is set by the sync provider
#endif

   messageStartReceived = false;
   messageEndReceived = false;
   writeIndex = 0;
   lineStartIndex = 0;

   // Set request pin high to enable smartmeter to send data.
   pinMode(PinRequest, OUTPUT);
   digitalWrite(PinRequest, HIGH);

   printFreeRAM();
   Serial.println(F("init done"));
   ledOff();
}

void ledBlink() {
  if (isLedOn) ledOff(); else ledOn();
}

void ledOn() {
  digitalWrite(PinLED, HIGH);    // turn the LED off by making the voltage LOW
  isLedOn = true;
}

void ledOff() {
  digitalWrite(PinLED, LOW);    // turn the LED on by making the voltage HIGH
  isLedOn = false;
}

void cbReceiveMessage(char *topic, byte *payload, unsigned int len) {
      // topics contains commands to control the smartmeter readout
      // /smartmeter1/set/p1telegram on | off
  Serial.print(topic); Serial.print(": "); // Serial.print(payload);
  // can be optimized? 
  if (!strcmp_P(topic, PSTR("sm1/set/p1-telegram"))) {
     if (!strncmp_P((char *)payload, PSTR("on"), 2)) {  // Limit to max string size
        // publish complete P1 telegram when it is received.
        sendP1Telegram = true;
     } else {
        sendP1Telegram = false;
     }
  } else if (!strcmp_P(topic, PSTR("sm1/set/datagrams"))) {
      if (!strncmp_P((char *)payload, PSTR("on"), 2)) {  // Limit to max string size
        // publish complete P1 telegram when it is received.
        updateNrOfDatagrams = true;
     } else {
        updateNrOfDatagrams = false;
     }
  } else if (!strcmp_P(topic, PSTR("sm1/set/units"))) {
      if (!strncmp_P((char *)payload, PSTR("on"), 2)) {  // Limit to max string size
        // publish also units when value is received
        sendUnits = true;
     } else {
        sendUnits = false;
     }
  }
}

char c2h(char c){
  return "0123456789ABCDEF"[0x0F & (unsigned char)c];
}

void readSmartMeter() {
      // Read smartmeter data and store all data until the p1 telegram is complete
   char ch;
   if (smartmeter.available()) {
      ch = smartmeter.read() & 0x7f;
      if (!messageStartReceived) {
         if (ch == '/') {
            messageStartReceived = true;
            Serial.println(); Serial.print(F("/"));
            writeIndex = 0;
            lineStartIndex = 0;
            inputBuffer[writeIndex++] = ch;
         }
      } else {
         inputBuffer[writeIndex++] = ch;
         if (ch == LineFeed) {
            Serial.print(F("."));
            // A complete line is received, check if the first character is an exclamation mark
            // to signal the end of the telegram
            if (lineStartIndex != 0) {
               if (inputBuffer[lineStartIndex] == '!') {
                  Serial.println(F("!"));
                  messageEndReceived = true;
                  nrOfDatagrams++;
                  ledOff();
               }
            }
            lineStartIndex = writeIndex;
         }
      }

       if (writeIndex > MaxNrOfChars) {
          // Ignore the rest of the characters.
          messageStartReceived = false;
          writeIndex = 0;    // Overwrite already received characters.
          ledOff();
       }
       // Blink LED every 40 characters
       if (!messageEndReceived && (writeIndex % 40) == 0) ledBlink();
    }
}

void processData() {
      // Process the complete block of data from the smartmeter, line by line.
   char  *s;
   int    i;
   char   payload[20], payloadunit[6], timestamp[13];
   char   scratch[35];
   time_t utc, t;

   if (messageEndReceived) {
      // Wait until a complete p1 telegram from the smartmeter is received 
      // to prevent that data is lost.
      if (messageStartReceived) {

         //Central European Time (Frankfurt, Paris)
         TimeChangeRule CEST = {"CEST", Last, Sun, Mar, 2, 120};     //Central European Summer Time
         TimeChangeRule CET = {"CET ", Last, Sun, Oct, 3, 60};       //Central European Standard Time
         Timezone CE(CEST, CET);
          
         utc = now();    //current time from the Time Library
         t = CE.toLocal(utc);
         sprintf_P(scratch, PSTR("%04d%02d%02d %02d%02d%02d"), year(t), month(t), day(t), hour(t), minute(t), second(t));
         Serial.print(F("sm1/timestamp ")); Serial.println(scratch);
         mqttClient.publish("sm1/timestamp", scratch);
         // process data in message
         debugln(F("Message received"));
         debugln();
         debugln(inputBuffer);
#if MQTT==1
         if (updateNrOfDatagrams) { 
            ltoa(nrOfDatagrams, scratch, 10);
            mqttClient.publish("sm1/datagrams", scratch);
         }
         if (sendP1Telegram) {
             // the line below crashes the client, instead display p1 telegram to the screen.
            //mqttClient.publish("sm1/p1-telegram", inputBuffer);
           Serial.println();
           Serial.println(inputBuffer);
         }
#endif
         // Process message by searching for OBIS references
         for (i = 0; i < NrOfOBISTopics; i++) {
            debug(F("Topic: ")); debug(ots[i].OBISRef);
            if (s = strstr_P(inputBuffer, ots[i].OBISRef)) {
              debugln(F(" found"));
              memset(payload, 0, sizeof(payload));
              memset(payloadunit, 0, sizeof(payloadunit));
              memset(timestamp, 0, sizeof(timestamp));
              memset(scratch, 0, sizeof(scratch));
              if (ots[i].next) { // value is on next line
                  s = strchr(s, '('); 
                  sscanf(s, "(%12s)", timestamp);   // first get the timestamp
                  s = strchr(s, LineFeed); s++;
                  sscanf(s, "(%9s)", payload);      // then the actual value
              } else {
                 switch (ots[i].fmt) {
                    case P1_FmtHeader     : sscanf(s, "/%s", payload);                                break;
                    case P1_FmtPowerUsage : sscanf(s, "%9s(%9s*%3s)", scratch, payload, payloadunit); break;
                    case P1_FmtTariff     : sscanf(s, "%11s(%4s)", scratch, payload);                 break;
                    case P1_FmtPowerActual: sscanf(s, "%9s(%7s*%2s)", scratch, payload, payloadunit); break;
                    default: debugln(F("Unknown OBIS Ref rcv (2)"));
                 }
              }
              strcpy_P(scratch, ots[i].topic);
              Serial.print(scratch);  // topic
              Serial.print(F("  ")); 
              Serial.print(payload);
              if (strlen(payloadunit) >= 1) {
                  Serial.print(F(" ")); 
                  Serial.print(payloadunit);
              }
              if (strlen(timestamp) >= 1) {
                  Serial.print(F(" time: ")); 
                  Serial.print(timestamp);
              }
              Serial.println();
#if MQTT==1
              mqttClient.publish(scratch, payload);
              if (sendUnits && strlen(payloadunit) >= 1) {
                 strcat(scratch, "/unit");
                 mqttClient.publish(scratch, payloadunit);
              }
              if (strlen(timestamp) >= 1) {
                 strcpy_P(scratch, ots[i].topic);
                 strcat(scratch, "/timestamp");
                 mqttClient.publish(scratch, timestamp);
              }
#endif
              ledOff();
           } else {
              debugln(F(" not found"));
           }
         }
      }
      messageStartReceived = false;
      messageEndReceived = false;
      writeIndex = 0;
      printFreeRAM();
   }
}

//const char* password = ;

void mqttInitialize() {
#if MQTT==1
   char s[20];
//   mqttClient.connect("sm1");
   mqttClient.connect("sm1", "sm1", "TrunkDida885");
   delay(5);

   // Subscribe to settings that can be set remotely
   mqttClient.subscribe("sm1/set/p1-telegram");
   mqttClient.subscribe("sm1/set/datagrams");
   mqttClient.subscribe("sm1/set/units");
//   mqttClient.publish("");

   ltoa(now(),s,10);
   mqttClient.publish("sm1/timestamp", s);
#endif
}

void loop() {
   if (!mqttClient.connected()) {
      mqttInitialize();
   }
   readSmartMeter();
   processData();
#if MQTT==1
   if (!messageStartReceived) 
      // to prevent characters from the serial interface are missed, 
      mqttClient.loop();
#endif   
   // Also loop for UDP (NTP) packet?
}

unsigned long getNtpTime() {
  int iBytes;
  
  sendNTPpacket(ntpIP); // send an NTP packet to a time server

  // wait for a reply / timeout after 10 seconds
  iBytes = 0;
  while (ntp.parsePacket() != 48 && iBytes < 2000) {
    iBytes++;
    delay(5);
  }

  if (ntp.available() == 48)  {
    Serial.print(F("time synced in "));
    Serial.print(iBytes*5);
    Serial.println(F(" ms"));

    // Ntp.read(packetBuffer,8);                // dump header
    ntp.read(packetBuffer,NTP_PacketSize);  // read the packet into the buffer

    // The timestamp starts at byte 40 of the received packet and is four bytes,
    // or two words, long. First, esxtract the two words:

    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);  
    // combine the four bytes (two words) into a long integer
    // this is NTP time (seconds since Jan 1 1900):
    unsigned long secsSince1900 = highWord << 16 | lowWord;
    const unsigned long seventyYears = 2208988800UL - TimeZoneOffset;  
    unsigned long epoch = secsSince1900 - seventyYears;
    return epoch; 
  } else {
    debugln(F(" - No NTP packet received"));
    return 0;
  }
}

void sendNTPpacket(IPAddress ntpServer) {
      // send an NTP request to the NTP time server at the given address 
  memset(packetBuffer, 0, NTP_PacketSize); 
  // Initialize values needed to form NTP request (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;            // Stratum, or type of clock
  packetBuffer[2] = 6;            // Polling Interval
  packetBuffer[3] = 0xEC;         // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49; 
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp: 
  ntp.beginPacket(ntpServer, NTP_UDPPort);   	   
  ntp.write(packetBuffer,NTP_PacketSize); 
  ntp.endPacket();
}

