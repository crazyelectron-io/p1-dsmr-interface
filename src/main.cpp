/*=================================================================================================
 *DESCRIPTION:
 *  This is a program for interfacing with Dutch smart meters, through their P1 port, and publish
 *  the data via MQTT.
 *  The telegrams received on the P1 port are parsed and the usage data is sent as a MQTT message
 *  where others can subscribe to.
 *
 *VERSION HISTORY:
 *	v0.1	Initial test version.
 *
 *COPYRIGHT:
 *	(c)2017 by Ron Moerman, https://electronicsworkbench.io, ron@electronicsworkbench.io.
 *	This program comes with ABSOLUTELY NO WARRANTY. Use at your own risk.
 *	This is free software, and you are welcome to redistribute it under certain
 *  conditions.
 *	The program and its source code are published under the GNU General Public License
 *  (GPL). See http://www.gnu.org/licenses/gpl-3.0.txt for details.
 *
 * $File: main.cpp $
 * $Revision: 0.1 $
 * $Date: Sunday, Sep 10, 2017 21:24:48 UTC $
 *================================================================================================*/

/*================================================================================================
 *                              I N C L U D E   H E A D E R S
 *================================================================================================*/

#include <TimeLib.h>
#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include "CRC16.h"


/*================================================================================================
 *                             G L O B A L   C O N S T A N T S
 *================================================================================================*/

/*======vvv Adapt values below vvv======*/

/*--- WiFi connection parameters (hard-coded for now) ---*/
const char* WIFI_SSID = "moerman-IoT";
const char* WIFI_PWD = "w!3Lingen8";

/*--- MQTT connection parameters ---*/
const PROGMEM char* MQTT_CLIENT_ID = "dsmr4";               //MQTT Client ID
const PROGMEM char* MQTT_SERVER = "192.168.1.30";           //HASS MQTT server
const PROGMEM unsigned int MQTT_SERVER_PORT = 1883;         //Port# on the HASS MQTT server
const PROGMEM char* MQTT_USER = "homeassistant";            //Auth for the HASS embedded MQTT server
const PROGMEM char* MQTT_PWD = "Wielingen8";
const PROGMEM char* MQTT_TOPIC = "sensor/dsmr";             //MQTT topic to create and publish to

/*--- Define input pins ---*/
#define SERIAL_RX D5                                        //P1 serial input pin

#define OTA_PORT 8266                                       //This is the default port for Arduino OTA library.

/*--- Debug/trace settings ---*/
//#define P1_DEBUG                                            //Debug the P1 telegram handling
#define MQTT_DEBUG                                          //Debug the MQTT handling

/*======^^^ Change values above ^^^======*/

#define SENSOR_VERSION "0.0.1"                              //Sensor client software version

const int cnMaxLineLength = 250;                            //Longest normal line is 178 char (+3 for \r\n\0)

#define MQTT_VERSION MQTT_VERSION_3_1_1


/*================================================================================================
 *                           G L O B A L   V A R I A B L E S
 *================================================================================================*/

/*--- Variables to store the relevant meter readings ---*/
long lElecLow = 0;                                          //Electricity consumption low tariff
long lElecHigh = 0;                                         //Electricity consumption high tariff
long lReturnLow = 0;                                        //Electricity return low tariff
long lReturnHigh = 0;                                       //Electricity return high tariff
long lElecActual = 0;                                       //Electricity actual consumption
long lReturnActual = 0;                                     //Electricity actual return
long lGasMeter = 0;                                         //Gas meter reading

/*--- Buffer for storing and processing a line of the telegram --- */
char achTelegram[cnMaxLineLength];

/*--- Define the P1 serial interface ---*/
SoftwareSerial hP1Serial(SERIAL_RX, -1, true, cnMaxLineLength); //(RX, TX, inverted, buffer size)

/*--- Cummulated CRC16 value ---*/
unsigned int nCurrentCrc = 0;

/*--- WiFi connection handle/instance ---*/
WiFiClient hEspClient;
/*--- MQTT PubSub client handle/instance ---*/
PubSubClient hMqttClient;


/*================================================================================================
 *                                     F U N C T I O N S
 *================================================================================================*/

/*------------------------------------------------------------------------------------------------*
 * SetupWiFi: Setup the WiFi connection to the IoT network.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Connect to the IoT WiFi network, for now with hard-coded WiFi parameters, by setting the
 *  ESP8266 in STA-mode. In case of failure, retry for 60 seconds and if it still fails, reboot
 *  the system to try allover again.
 *INPUT:
 *	None.
 *OUTPUT:
 *	None. Returns only after succesful connect.
 *NOTES:
 *	For now the WiFi parameters are hard-coded in the program.
 *------------------------------------------------------------------------------------------------*/
void SetupWiFi()
{
     delay(10);

     /*--- Initialize the WiFi connection ---*/
     Serial.print("Connecting to ");
     Serial.print(WIFI_SSID);

     WiFi.mode(WIFI_STA);
     WiFi.begin(WIFI_SSID, WIFI_PWD);

     /*--- Wait up to 60 seconds for the connection ---*/
     int nWait =  60;
     while (WiFi.status() != WL_CONNECTED)
     {
         delay(500);
         Serial.print(".");
         if (!--nWait)
         {
             /*--- Restart and retry if still not connected ---*/
             Serial.println("");
             Serial.println("Connection Failed! Rebooting...");
             ESP.restart();
         }
     }

     /*--- WiFi connection established, show DHCP-provided IP address on serial console ---*/
     Serial.print(" WiFi connected with IP address: ");
     Serial.println(WiFi.localIP());
}


/*------------------------------------------------------------------------------------------------*
 * SetupOTA: Setup for OTA updates.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Defines  callback routines for the Arduino Framework OTA library routines; only to show the
 *  status. All actual OTA processing is handled by the library.
 *INPUT:
 *	None.
 *OUTPUT:
 *	None.
 *------------------------------------------------------------------------------------------------*/
 void SetupOTA(void)
 {
     ArduinoOTA.setPort(OTA_PORT);                           //Port defaults to 8266
     ArduinoOTA.setHostname(MQTT_CLIENT_ID);                 //Defaults to esp8266-[ChipID]
     //ArduinoOTA.setPassword((const char *)"123");          //No authentication by default

     ArduinoOTA.onStart([]() { Serial.println("OTA Start"); });
     ArduinoOTA.onEnd([]() { Serial.println("\nOTA End"); });
     ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
     {
         Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
     });
     ArduinoOTA.onError([](ota_error_t error)
     {
         Serial.printf("OTA Error[%u]: ", error);
         if (error == OTA_AUTH_ERROR)
             Serial.println("Auth Failed");
         else if (error == OTA_BEGIN_ERROR)
             Serial.println("Begin Failed");
         else if (error == OTA_CONNECT_ERROR)
             Serial.println("Connect Failed");
         else if (error == OTA_RECEIVE_ERROR)
             Serial.println("Receive Failed");
         else if (error == OTA_END_ERROR)
             Serial.println("End Failed");
     });

     ArduinoOTA.begin();
 }


/*------------------------------------------------------------------------------------------------*
 * ConnectMqtt: (Re)connect to the MQTT broker.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Check if there is an active MQTT connection. If not try to re-establish a connection for 10
 *  seconds (retry 5 times, every other second), before quiting (to prevent stalling).
 *INPUT:
 *	None.
 *OUTPUT:
 *	(bool) true if a connection is established (within 10s), false if it failed.
 *------------------------------------------------------------------------------------------------*/
bool ConnectMqtt(void)
{
    if (!hMqttClient.connected())
    {
        Serial.print("Setup MQTT...");
        /*--- Loop until we're (re)connected for 10 seconds ---*/
        for (int nLoop = 0; nLoop<5; ++nLoop)
        {
            /*--- Attempt to connect ---*/
            if (hMqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PWD))
            {
                Serial.print("connected as Publish client with topic ");
                Serial.println(MQTT_TOPIC);
                return true;                                //We're done!
            }
            else
            {
#ifdef MQTT_DEBUG
                Serial.print("failed, rc=");
                Serial.print(hMqttClient.state());
                Serial.println("");
#else
                Serial.print(".");
#endif
                yield();
                delay(2000);                                //Wait 2s before retrying
            }
        }
        /*--- MQTT connection failed ---*/
#ifndef MQTT_DEBUG
        Serial.print("failed, rc=");
        Serial.print(hMqttClient.state());
        Serial.println("");
#endif
        return false;
    }
    return true;                                            //We were already connected
}


/*------------------------------------------------------------------------------------------------*
 * PublishToTopic: Publish the meter values to MQTT topic.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Create a JSON object with all the current smart meter values (including gas) and publish it to
 *  the defined topic.
 *INPUT:
 *	None. Values are in global variables.
 *OUTPUT:
 *	(bool) true if succeeded, false if failed (either connection lost or message too large).
 *------------------------------------------------------------------------------------------------*/
bool PublishToTopic(void)
{
    /*--- create JSON object and fill with meter values
        see https://github.com/bblanchon/ArduinoJson/wiki/API%0Referemce ---*/
    StaticJsonBuffer<250> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["current_power"] = (String)lElecActual;
    root["low_power"] = (String)lElecLow;
    root["high_power"] = (String)lElecHigh;
    root["gas_usage"] = (String)lGasMeter;
#ifdef MQTT_DEBUG
    root.prettyPrintTo(Serial);
    Serial.println("");
#endif

    /*--- Publish the JSON data to the MQTT topic ---*/
    char achData[250];                                         //Temporary data buffer
    root.printTo(achData, root.measureLength()+1);
    return hMqttClient.publish(MQTT_TOPIC, achData, true);
}


/*------------------------------------------------------------------------------------------------*
 * IsNumber: Check if the passed character is a valid digit or decimal point.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Test the passed character for a valid digit or decimal point.
 *INPUT:
 *	char chNum - character to test
 *OUTPUT:
 *	(bool) true if the character is a valid number character, false otherwise.
 *------------------------------------------------------------------------------------------------*/
bool IsNumber(char chNum)
{
    return (isdigit(chNum) || chNum == '.' || chNum == 0);
}


/*------------------------------------------------------------------------------------------------*
 * FindLastChar: Find the position of the last character specified.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Look for the specified characters in the past string, starting at the passed position and
 *  going backwards to the beginning.
 *INPUT:
 *	char achBuffer[] - character array to search through
 *  char chSearch - character to look for
 *  int nLen - position in string to start looking backward
 *OUTPUT:
 *	(int) position of the last character specified, or -1 if not found.
 *------------------------------------------------------------------------------------------------*/
int FindLastChar(char achBuffer[], char chSearch, int nLen)
{
    for (int i = nLen - 1; i >= 0; i--)
    {
        if (achBuffer[i] == chSearch)
            return i;
    }
    return -1;
}


/*------------------------------------------------------------------------------------------------*
 * GetValue: Get usage value from the string passed
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Retreive the number from the end of the passed string and multiply by 1000 to remove the decimal
 *  point from the value sent by the P1 interface for usage values in the DSMR standard telegram.
 *  Numbers are surrounded by brackets, like '(0123.456)'.
 *INPUT:
 *	char * pchBuffer - string to read the last number from (looking from end of string)
 *  int nMaxLen - length of string to consider
 *OUTPUT:
 *	(long) value retreived from string (char array) or 0 if no valid number found
 *------------------------------------------------------------------------------------------------*/
long GetValue(char* pchBuffer, int nMaxLen)
{
    /*--- Find start of the value by looking at the corresponding opening bracket '(' ---*/
    int nStart = FindLastChar(pchBuffer, '(', nMaxLen - 2);
    /*--- Do some sanity checks ---*/
    if (nStart < 8)
        return 0;                                           //Numbers are at least 8 characters long
    if (nStart > 32)
        nStart = 32;                                        //...and maximum 32 characters

    /*--- Look for the '*', separating the value from the unit (e.g. kWh)---*/
    int nLen = FindLastChar(pchBuffer, '*', nMaxLen - 2) - nStart - 1;
    /*--- Do some sanity checks; values should have 4 to 12 digits ---*/
    if (nLen < 4 || nLen > 12)
        return 0;

    /*--- Check if it is a valid number and return its value (or 0) ---*/
    char *pchValue = pchBuffer + nStart + 1;                //Point at start of value string
    for (int i = 0; i < nLen; i++)
    {
        if (!IsNumber(*(pchValue+i)))
        {
#ifdef P1_DEBUG
            Serial.println("ERROR: Value parsed is not a valid number!");
#endif
            return 0;
        }
    }
    return 1000*atof(pchValue);
}


/*------------------------------------------------------------------------------------------------*
 * DecodeTelegram: Decode the current telegram line.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Decode the telegram line stored in the buffer and extract any meter values we want to use.
 *INPUT:
 *	int len - length of telegram line to decode, including trailing zero
 *  (the telegram line is stored in the global variable 'achTelegram')
 *OUTPUT:
 *	(bool) true if decoded a valid message and CRC, false otherwise.
 *------------------------------------------------------------------------------------------------*/
bool DecodeTelegram(int nLen)
{
    /*--- Check for Message Start or Checksum Start character on this line --- */
    int nStartChar = FindLastChar(achTelegram, '/', nLen);
    int nEndChar = FindLastChar(achTelegram, '!', nLen);
    bool bValidCrcFound = false;

    /*--- Is this the start of telegram line? ---*/
    if (nStartChar >= 0)
    {
        /*--- Start telegram character found ('/'); restart CRC16 calculation ---*/
        nCurrentCrc = Crc16(0x0000, (unsigned char *)achTelegram+nStartChar, nLen-nStartChar);
#ifdef P1_DEBUG
        /*--- Send the telegram also through the serial debug port ---*/
        for (int cnt=nStartChar; cnt<nLen-nStartChar; cnt++)
            Serial.print(achTelegram[cnt]);
#endif
    }
    /*--- Is this the end of telegram line? ---*/
    else if (nEndChar >= 0)
    {
        /*--- Add to CRC16 calculation ---*/
        nCurrentCrc = Crc16(nCurrentCrc, (unsigned char*)achTelegram+nEndChar, 1);
        char achMessageCrc[4];                              //Buffer for the CRC16 characters
        strncpy(achMessageCrc, achTelegram+nEndChar+1, 4);
#if P1_DEBUG
        for (int cnt=0; cnt<nLen; cnt++)
            Serial.print(achTelegram[cnt]);
#endif
        bValidCrcFound = (strtol(achMessageCrc, NULL, 16) == nCurrentCrc);
        if (bValidCrcFound)
            Serial.println("\nINFO: VALID CRC FOUND!");
        else
            Serial.println("\nERROR: INVALID CRC FOUND!");
        nCurrentCrc = 0;
    }
    else
    {
        nCurrentCrc = Crc16(nCurrentCrc, (unsigned char*)achTelegram, nLen);
#if P1_DEBUG
        for (int cnt=0; cnt<nLen;cnt++)
            Serial.print(achTelegram[cnt]);
#endif
    }

    // 1-0:1.8.1(000992.992*kWh)
    // 1-0:1.8.1 = Electricity consumption low tariff (DSMR v4.0)
    if (strncmp(achTelegram, "1-0:1.8.1", strlen("1-0:1.8.1")) == 0)
        lElecLow =  GetValue(achTelegram, nLen);

    // 1-0:1.8.2(000560.157*kWh)
    // 1-0:1.8.2 = Electricity consumption high tariff (DSMR v4.0)
    if (strncmp(achTelegram, "1-0:1.8.2", strlen("1-0:1.8.2")) == 0)
        lElecHigh = GetValue(achTelegram, nLen);

    // 1-0:2.8.1(000348.890*kWh)
    // 1-0:2.8.1 = Electricity return low tariff (DSMR v4.0)
    if (strncmp(achTelegram, "1-0:2.8.1", strlen("1-0:2.8.1")) == 0)
        lReturnLow = GetValue(achTelegram, nLen);

    // 1-0:2.8.2(000859.885*kWh)
    // 1-0:2.8.2 = Electricity return high tariff (DSMR v4.0)
    if (strncmp(achTelegram, "1-0:2.8.2", strlen("1-0:2.8.2")) == 0)
        lReturnHigh = GetValue(achTelegram, nLen);

    // 1-0:1.7.0(00.424*kW) Actual consumption
    // 1-0:2.7.0(00.000*kW) Actual teruglevering
    // 1-0:1.7.x = Electricity consumption actual usage (DSMR v4.0)
    if (strncmp(achTelegram, "1-0:1.7.0", strlen("1-0:1.7.0")) == 0)
        lElecActual = GetValue(achTelegram, nLen);

    if (strncmp(achTelegram, "1-0:2.7.0", strlen("1-0:2.7.0")) == 0)
        lReturnActual = GetValue(achTelegram, nLen);

    // 0-1:24.2.1(150531200000S)(00811.923*m3)
    // 0-1:24.2.1 = Gas (DSMR v4.0) on Kaifa MA105 and Landis+Gyr 350 meter
    if (strncmp(achTelegram, "0-1:24.2.1", strlen("0-1:24.2.1")) == 0)
        lGasMeter = GetValue(achTelegram, nLen);

    return bValidCrcFound;
}


/*------------------------------------------------------------------------------------------------*
 * DoTelegramLines: Read and decode lines of the P1 telegram
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Keep reading lines from the P1 serial input, decode them one-by-one, and pubish the resulting
 *  smart meter values to the sensor MQTT topic (in JSON format).
 *INPUT:
 *	None. The message is stored in the global buffer 'telegram'.
 *OUTPUT:
 *	None. Returns when no more P1 serial input available.
 *------------------------------------------------------------------------------------------------*/
void DoTelegramLines(void)
{
    int nLen;
    bool bNew = false;                                      //Indicates when new meter data is parsed

    if (hP1Serial.available())                              //Any serial data available?
    {
        memset(achTelegram, 0, sizeof(achTelegram));        //Clear the telegram receive buffer

        /*--- Keep reading and decoding telegram lines while available on the P1 interface ---*/
        while (hP1Serial.available())
        {
            nLen = hP1Serial.readBytesUntil('\n', achTelegram, cnMaxLineLength); //Read a line from the telegram
            achTelegram[nLen] = '\n';                       //Terminate the line
            achTelegram[nLen+1] = 0;
            yield();
            if (DecodeTelegram(nLen+1))                     //Decode the value(s) on this telegram line, if any
                bNew = true;
        }

        /*--- Send any updated smart meter values to MQTT broker ---*/
        if (bNew)
            PublishToTopic();                           //Send updated data from this line (if any)
    }
}


/*------------------------------------------------------------------------------------------------*
 * setup: The standar Arduino Framework one-time initialization routine.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Called from the Arduino Framework at boot. Initializes the serial console (for status and
 *  debugging purposes), the MQTT connection , the OTA services and the WiFi connection.
 *INPUT:
 *	None.
 *OUTPUT:
 *	None. Returns only when the WiFi and MQTT connections are succesful.
 *NOTES:
 *  During boot of the ESP8266 there will be some garbage characters on the serial debug output.
 *------------------------------------------------------------------------------------------------*/
 void setup(void)
 {
     Serial.begin(115200);                                   //Setup the serial console (USB) @115200 baud

     Serial.print("\r\nBooting DSMR P1 MQTT Sensor version ");
     Serial.println(SENSOR_VERSION);                         //Send welcome message to console

     SetupWiFi();                                            //Setup the WiFi connection

     hP1Serial.begin(115200);                                //Initialize the P1 serial interface

     SetupOTA();                                             //Setup OTA update service

     hMqttClient.setClient(hEspClient);                      //Initialzie the MQTT interface
     hMqttClient.setServer(MQTT_SERVER, MQTT_SERVER_PORT);
     (void)ConnectMqtt();                                    //Setup MQTT connection

     Serial.println("READY");
 }


/*------------------------------------------------------------------------------------------------*
 * loop: The main program loop
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Called from the Arduino Framework after the setup routine. It will be called in a loop until
 *  the device is reset.
 *INPUT:
 *	None.
 *OUTPUT:
 *	None.
 *------------------------------------------------------------------------------------------------*/
void loop(void)
{
    /*--- Make sure we have an MQTT connection ---*/
    if (!hMqttClient.loop())                                //Keep the MQTT connection alive
        (void)ConnectMqtt();

    /*--- Read, decode and send meter values ---*/
    DoTelegramLines();

    /*--- Check for OTA updates ---*/
    ArduinoOTA.handle();
}
