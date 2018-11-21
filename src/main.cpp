/*===================================================================================================*
 *DESCRIPTION:
 *  This is a continuously running program (on an ESP8266) for interfacing with a Dutch smart meter,
 *  through their P1 port, and publish the data via MQTT.
 *  The telegrams received on the P1 port are parsed and the relevant usage data is sent as a MQTT
 *  message where others can subscribe to (in particular OpenHAB 2).
 *
 *  Currently only tested on a Landys+Gyr 350 using DSMR v4, which produces a P1 telegram every 10s.
 *  An example telegram looks like:
 *      /XMX5LGBBFFB231314239
 *
 *      1-3:0.2.8(42)                                           // DSMR version (4.2)
 *      0-0:1.0.0(180924132132S)                                // Timestamp (yymmddhhmmssS) S=Summer time
 *      0-0:96.1.1(4532323036303137363437393334353135)          // Serial number (ASCII)
 *      1-0:1.8.1(011522.839*kWh)                               // Consumption T1 tariff
 *      1-0:1.8.2(010310.991*kWh)                               // Consumption T2 tariff
 *      1-0:2.8.1(000000.000*kWh)                               // Return T1 tariff
 *      1-0:2.8.2(000000.000*kWh)                               // Return T2 tariff
 *      0-0:96.14.0(0002)                                       // Current tariff (T1 or T2)
 *      1-0:1.7.0(00.503*kW)                                    // Actual consumption all phases
 *      1-0:2.7.0(00.000*kW)                                    // Actual return all phases
 *      0-0:96.7.21(00015)
 *      0-0:96.7.9(00005)                                       // Number of long power failures in any phase
 *      1-0:99.97.0(5)(0-0:96.7.19)(170520130938S)(0000005627*s)(170325044014W)(0043178677*s) [truncated]
 *      1-0:32.32.0(00002)
 *      1-0:52.32.0(00002)
 *      1-0:72.32.0(00002)
 *      1-0:32.36.0(00000)
 *      1-0:52.36.0(00000)
 *      1-0:72.36.0(00000)
 *      0-0:96.13.1()
 *      0-0:96.13.0()
 *      1-0:31.7.0(001*A)
 *      1-0:51.7.0(001*A)
 *      1-0:71.7.0(001*A)
 *      1-0:21.7.0(00.086*kW)                                   // Actual consumption on L1
 *      1-0:41.7.0(00.250*kW)                                   // Actual consumption on L2
 *      1-0:61.7.0(00.166*kW)                                   // Actual consumption on L3
 *      1-0:22.7.0(00.000*kW)                                   // Actual return on L1
 *      1-0:42.7.0(00.000*kW)                                   // Actual return on L2
 *      1-0:62.7.0(00.000*kW)                                   // Actual return on L3
 *      0-1:24.1.0(003)                                         // Slave (Gas meter) device type
 *      0-1:96.1.0(4731303138333430313538383732343334)          // Gas meter serial number
 *      0-1:24.2.1(180924130000S)(04890.857*m3)                 // Gas meter time stamp + value
 *      !FCA6                                                   // CRC16 cehcksum of entire telegram (from / to !)
 * 
 *  More details can be found here: https://electronicsworkbench.io/blog/smartmeter-1.
 * 
 *  The JSON object sent to MQTT has the following specs:
 *  - MQTT topic: sensor/dsmr
 *  - MQTT message:
 *      {
 *        "dsmr": "42",
 *        "power":
 *        {
 *          "time": "180924132132S",
 *          "tariff": "2",
 *          "use":
 *          {
 *            "total":
 *            {
 *              "T1": "11522839",
 *              "T2": "10310991"
 *            },
 *            "actual":
 *            {
 *              "total": "503",
 *              "L1": "86",
 *              "L2": "250",
 *              "L3": "166"
 *            }
 *          },
 *          "return":
 *          {
 *            "total":
 *            {
 *              "T1": "0",
 *              "T2": "0"
 *            },
 *            "actual":
 *            {
 *              "total": "503",
 *              "L1": "86",
 *              "L2": "250",
 *              "L3": "166"
 *            }
 *          }
 *        },
 *        "gas":
 *        {
 *          "time": "180924130000S",
 *          "total": "4890857"
 *        }
 *      }
 * 
 * All power readings are specified in Wh, gas reading is 1/1000 dm3.
 * 
 * The advantage of using a nested JSON structure like above is that we can add
 * elements without affecting existing logic to extract values. 
 *
 * The default MQTT packet size of the used Arduino PubSubClient library is too small for
 * the messages we are sending. Increase it to 256 in the PubSubClient.h file.
 * Be aware that adding a '#define MQTT_MAX_PACKET_SIZE' it to this source file before the include
 * doesn't work because of the order the library headers are processed during pre-compile!
 * See also: https://github.com/knolleary/pubsubclient/issues/431.
 *
 *VERSION HISTORY:
 *	v0.1	Initial test version using HTTP calls to a webservice.
 *  v0.2    Send MQTT messages in stead of making HTTP calls to a webservice.
 *  v0.3    Expanded MQTT message to JSON structure.
 *  v0.4    Adapted to recompiled Arduino PubSubClient library for larger packet size (up to 256).
 *  v0.5    Added extra fields to MQTT message, cleanup of the code and some extra comments.
 *  v0.6    Extended StaticJsonBuffer to fix overflow issue resulting in last value not being sent.
 *
 *COPYRIGHT:
 *	This program comes with ABSOLUTELY NO WARRANTY. Use at your own risk.
 *	This is free software, and you are welcome to redistribute it under certain conditions.
 *	The program and its source code are published under the GNU General Public License (GPL).
 *  See http://www.gnu.org/licenses/gpl-3.0.txt for details.
 *  Some parts are based on other open source code.
 *
 * $File: main.cpp $
 * $Revision: 0.6 $
 * $Date: Sunday, Oct 10, 2018 21:13 UTC $
 *==================================================================================================*/

/*==================================================================================================*
 *                                 I N C L U D E   H E A D E R S                                    *
 *==================================================================================================*/

#include <Arduino.h>
#include <ArduinoOTA.h>

#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>

#include <TimeLib.h>
#include <PubSubClient.h> //With increased MQTT packet size to 512
#include <ArduinoJson.h>
#include <ctype.h>

#include "CRC16.h"
#include "secrets.h" //Contains all the super secret stuff!

/*==================================================================================================*
 *                               G L O B A L   C O N S T A N T S                                    *
 *==================================================================================================*/

/*##########VVV ADAPT VALUES BELOW TO YOUR CONFIGURATION VVV##########*/

/*--- WiFi connection parameters (hard-coded for now) ---*/
const char *WIFI_SSID = SECRET_WIFI_SSID;
const char *WIFI_PWD = SECRET_WIFI_PWD;

/*--- MQTT connection parameters ---*/
const PROGMEM char *MQTT_CLIENT_ID = "dsmrv4";      //MQTT Client ID
const PROGMEM char *MQTT_SERVER = "192.168.1.2";    //MQTT Server (Mosquitto)
const PROGMEM unsigned int MQTT_SERVER_PORT = 1883; //Port# on the MQTT Server
const PROGMEM char *MQTT_USER = SECRET_MQTT_USER;   //Authentication for the MQTT Server
const PROGMEM char *MQTT_PWD = SECRET_MQTT_PWD;
const PROGMEM char *MQTT_TOPIC = "sensor/dsmr"; //MQTT topic to create and publish to

/*--- Define serial input ---*/
#define SERIAL_RX D5    //P1 serial input pin
#define BAUDRATE 115200 //DSMRv4 runs P1 port at 115,200 baud (8N1)

/*--- Define OTA port ---*/
#define OTA_PORT 8266 //This is the default port for Arduino OTA library.

/*--- Debug/trace settings ---*/
//#define P1_DEBUG                                                //Debug the P1 telegram handling
//#define MQTT_DEBUG                                              //Debug the MQTT handling

/*##########^^^ ADAPT VALUES ABOVE TO YOUR CONFIGURATION ^^^##########*/

#define SENSOR_VERSION "0.6" //Sensor client software version

/*--- DSMR definitions ---*/
#define DSMR_VERSION "1-3:0.2.8"       //DSMR version
#define DSMR_PWR_TIMESTAMP "0-0:1.0.0" //P1 telegram timestamp
#define DSMR_PWR_LOW "1-0:1.8.1"       //Power consumption meter (low tariff)
#define DSMR_PWR_HIGH "1-0:1.8.2"      //Power consumption meter (high tariff)
#define DSMR_RET_LOW "1-0:2.8.1"       //Power return meter (low tariff)
#define DSMR_RET_HIGH "1-0:2.8.2"      //Power return meter (high tariff)
#define DSMR_PWR_ACTUAL "1-0:1.7.0"    //Power consumption actual
#define DSMR_PWR_L1 "1-0:21.7.0"       //Power consumption L1 actual
#define DSMR_PWR_L2 "1-0:41.7.0"       //Power consumption L2 actual
#define DSMR_PWR_L3 "1-0:61.7.0"       //Power consumption L3 actual
#define DSMR_RET_L1 "1-0:22.7.0"       //Power return L1 actual
#define DSMR_RET_L2 "1-0:42.7.0"       //Power return L2 actual
#define DSMR_RET_L3 "1-0:62.7.0"       //Power return L3 actual
#define DSMR_RET_ACTUAL "1-0:2.7.0"    //Power return actual
#define DSMR_PWR_TARIFF "0-0:96.14.0"  //Power current tariff (1=Low,2=High)
#define DSMR_GAS_METER "0-1:24.2.1"    //Gas on Kaifa MA105 + Landis+Gyr 350 meters

const int cnLineLen = 200; //Longest normal line is 178 char (+3 for \r\n\0)

#define MQTT_VERSION MQTT_VERSION_3_1_1 //The MQTT version we use

/*==================================================================================================*
 *                           G L O B A L   V A R I A B L E S                                        *
 *==================================================================================================*/

/*--- Variables to store the relevant meter readings ---*/
long lDsmrVersion = 0;  //DSMR telegram version number
char achPwrTime[16];    //Timestamp of power reading
long lPwrLow = 0;       //Power consumption low tariff
long lPwrHigh = 0;      //Power consumption high tariff
long lPwrActual = 0;    //Power actual consumption
long lPwrL1 = 0;        //Power actual L1 consumption
long lPwrL2 = 0;        //Power actual L2 consumption
long lPwrL3 = 0;        //Power actual L3 consumption
long lReturnLow = 0;    //Power return low tariff (solar panels)
long lReturnHigh = 0;   //Power return high tariff (solar panels)
long lReturnActual = 0; //Power actual return (solar panels)
long lReturnL1 = 0;     //Power actual L1 return
long lReturnL2 = 0;     //Power actual L2 return
long lReturnL3 = 0;     //Power actual L3 return
long lPwrTariff = 0;    //Active power tariff (T1 or T2)
char achGasTime[16];    //Timestamp of gas reading
long lGasMeter = 0;     //Gas meter reading (~hourly updated)

/*--- Buffer for storing and processing a line of the P1 telegram --- */
char achTelegram[cnLineLen];

/*--- Define the P1 serial interface ---*/
SoftwareSerial hP1Serial(SERIAL_RX, -1, true, cnLineLen); //(RX, TX, inverted, buffer size)

/*--- Cummulated CRC16 value ---*/
unsigned int nCurrentCrc = 0;

/*--- WiFi connection handle/instance ---*/
WiFiClient hEspClient;

/*--- MQTT PubSub client handle/instance ---*/
PubSubClient hMqttClient;

/*==================================================================================================*
 *                                     F U N C T I O N S                                            *
 *==================================================================================================*/

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
 *	For now the WiFi parameters are hard-coded in the program (*to be fixed*).
 *------------------------------------------------------------------------------------------------*/
void SetupWiFi()
{
    delay(10); //Let the SoC WiFi stabilize

    /*--- Initialize the WiFi connection ---*/
    Serial.print("Connecting to "); //Tell the world we are connecting
    Serial.print(WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PWD);

    /*--- Wait up to 60 seconds for the connection ---*/
    int nWait = 60;
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(1000);
        Serial.print(".");
        if (!--nWait)
        {
            /*--- Restart and retry if still not connected ---*/
            Serial.println("");
            Serial.println("Connection Failed! Rebooting...");
            ESP.restart();
        }
    }

    /*--- WiFi connection established ---*/
    Serial.print(" WiFi connected with IP address: "); //Show DHCP-provided IP address on console
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
    ArduinoOTA.setPort(OTA_PORT);           //Port defaults to 8266
    ArduinoOTA.setHostname(MQTT_CLIENT_ID); //Defaults to esp8266-[ChipID]
    //ArduinoOTA.setPassword((const char *)"123456");           //No authentication by default

    ArduinoOTA.onStart([]() {
        Serial.println("OTA Start");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA End");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
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

        /*--- Loop until we're (re)connected for 5 seconds ---*/
        for (int nLoop = 0; nLoop < 5; ++nLoop)
        {
            /*--- Attempt to connect ---*/
            if (hMqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PWD))
            {
                Serial.print("connected as Publish client with topic ");
                Serial.println(MQTT_TOPIC);
                return true; //We're done!
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
                delay(1000); //Wait 1s before retrying
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
#ifdef MQTT_DEBUG
    Serial.println("MQTT connecting alive");
#endif
    return true; //We were already connected
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
        see https://github.com/bblanchon/ArduinoJson/wiki/API%20Reference ---*/
    StaticJsonBuffer<800> jsonBuffer; //Be generous: too small will drop last entrie(s)
    JsonObject &root = jsonBuffer.createObject();
    root["dsmr"] = (String)lDsmrVersion;
    JsonObject &jPwr = root.createNestedObject("power");
    jPwr["time"] = (String)achPwrTime;   //Power reading timestamp + Summer/Winter time
    jPwr["tariff"] = (String)lPwrTariff; //Active power tariff (T1 or T2)
    /*--- Create Gas meter entries ---*/
    JsonObject &jGas = root.createNestedObject("gas");
    jGas["time"] = (String)achGasTime; //Gas reading timestamp + Summer/Winter time
    jGas["total"] = (String)lGasMeter; //Gas meter reading (~hourly updated)
    /*---  Create power consumption entries ---*/
    JsonObject &jUse = jPwr.createNestedObject("use");
    JsonObject &jTotalUse = jUse.createNestedObject("total");
    jTotalUse["T1"] = (String)lPwrLow;  //Power consumption low tariff
    jTotalUse["T2"] = (String)lPwrHigh; //Power consumption high tariff
    JsonObject &jActualUse = jUse.createNestedObject("actual");
    jActualUse["total"] = (String)lPwrActual; //Power actual consumption
    jActualUse["L1"] = (String)lPwrL1;        //Power actual L1 consumption
    jActualUse["L2"] = (String)lPwrL2;        //Power actual L2 consumption
    jActualUse["L3"] = (String)lPwrL3;        //Power actual L3 consumption
    /*--- Create power return entries ---*/
    JsonObject &jReturn = jPwr.createNestedObject("return");
    JsonObject &jTotalReturn = jReturn.createNestedObject("total");
    jTotalReturn["T1"] = (String)lReturnLow;  //Power return low tariff (solar panels)
    jTotalReturn["T2"] = (String)lReturnHigh; //Power return high tariff (solar panels)
    JsonObject &jActualReturn = jReturn.createNestedObject("actual");
    jActualReturn["total"] = (String)lReturnActual; //Power actual return (solar panels)
    jActualReturn["L1"] = (String)lReturnL1;        //Power actual L1 return (solar panels)
    jActualReturn["L2"] = (String)lReturnL2;        //Power actual L2 return (solar panels)
    jActualReturn["L3"] = (String)lReturnL3;        //Power actual L3 return (solar panels)

    /*--- Publish the JSON data to the MQTT topic ---*/
    char achData[600]; //Temporary packet buffer
    root.printTo(achData, root.measureLength() + 1);
    achData[root.measureLength() + 1] = 0;
#ifdef MQTT_DEBUG
    Serial.print("MQTT topic: ");
    Serial.println(MQTT_TOPIC);
    Serial.print("MQTT message: ");
    Serial.println(achData);
#endif
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
 * FindFirstChar: Find the position of the first character specified.
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Look for the specified characters in the past string, starting at the first position and
 *  going forwards to the end.
 *INPUT:
 *	char achBuffer[] - character array to search through
 *  char chSearch - character to look for
 *  int nLen - Length of string passed
 *OUTPUT:
 *	(int) position of the First character specified, or -1 if not found.
 *------------------------------------------------------------------------------------------------*/
int FindFirstChar(char achBuffer[], char chSearch, int nLen)
{
    for (int i = 0; i <= nLen; i++)
    {
        if (achBuffer[i] == 0)
            break; //Double check: end of string reached?
        if (achBuffer[i] == chSearch)
            return i; //Found character
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
 *  bool bMultiply - Multiply by 1000 to get rid of decimal
 *OUTPUT:
 *	(long) value retreived from string (char array) or 0 if no valid number found
 *------------------------------------------------------------------------------------------------*/
long GetValue(char *pchBuffer, int nMaxLen, bool bMultiply = true)
{
    /*--- Find start of the value by looking at the corresponding opening bracket '(' ---*/
    int nStart = FindLastChar(pchBuffer, '(', nMaxLen - 1);
    /*--- Do some sanity checks ---*/
    if (nStart < 8)
    {
#ifdef P1_DEBUG
        Serial.println("ERROR[0]");
#endif
        return 0;
    }
    if (nStart > 32)
    {
#ifdef P1_DEBUG
        Serial.println("ERROR[1]");
#endif
        return 0;
    }

    /*--- Look for the '*', separating the value from the unit (e.g. kWh)---*/
    int nLen = FindLastChar(pchBuffer, '*', nMaxLen - 1) - nStart - 1;
    /*--- Some number strings have no *, so check for that too ---*/
    if (nLen < 0)
        nLen = FindLastChar(pchBuffer, ')', nMaxLen - 1) - nStart - 1;

    /*--- Sanity check: values should have between 1 and 12 digits ---*/
    if (nLen < 1 || nLen > 12)
    {
#ifdef P1_DEBUG
        Serial.println("ERROR[5]");
#endif
        return 0;
    }

    /*--- Check if it is a valid number and return its value (or 0) ---*/
    char *pchValue = pchBuffer + nStart + 1; //Point at start of value string
    for (int i = 0; i < nLen; i++)
    {
        if (!IsNumber(*(pchValue + i)))
        {
#ifdef P1_DEBUG
            Serial.println("ERROR[6]");
#endif
            return 0;
        }
    }

    /*--- Return value without decimal point ---*/
    if (bMultiply)
        return 1000 * atof(pchValue);
    else
        return atof(pchValue);
}

/*------------------------------------------------------------------------------------------------*
 * GetLastText: Get last text parameter from the string passed (between brackets)
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Retreive the text from the end of the passed string.
 *  Text is surrounded by brackets, like '(180924132132S)'.
 *INPUT:
 *	char* pchBuffer - string to read the last text from (looking from end of string)
 *  int nMaxLen - length of string to consider
 *  char* pchText - Pointer to start of the parsed text (max 32)
 *OUTPUT:
 *	(int) length of parsed text, 0 if no text found and pchText is empty string.
 *------------------------------------------------------------------------------------------------*/
int GetLastText(char *pchBuffer, int nMaxLen, char *pchText)
{
    pchText[0] = 0;

    /*--- Find start of the text by looking at the corresponding opening bracket '(' ---*/
    int nStart = FindLastChar(pchBuffer, '(', nMaxLen - 1); //9
    if (nStart < 8 || nStart > 39)
    { //Do some sanity checks
#ifdef P1_DEBUG
        Serial.println("ERROR[1]");
#endif
        return 0;
    }

    /*--- Look for the ')', terminating the text ---*/
    int nLen = FindLastChar(pchBuffer, ')', nMaxLen - 1) - nStart - 1; //13
    if (nLen < 1 || nLen > 31)
    { //Do some more sanity checks
#ifdef P1_DEBUG
        Serial.println("ERROR[2]");
#endif
        return 0;
    }

    /*--- Copy the text to the output buffer and terminate it with '\0x00' ---*/
    strncpy(pchText, pchBuffer + nStart + 1, nLen);
    pchText[nLen] = 0;

    return nLen;
}

/*------------------------------------------------------------------------------------------------*
 * GetFirstText: Get first text parameter from the string passed (between brackets)
 *------------------------------------------------------------------------------------------------*
 *DESCRIPTION:
 *	Retreive the text from the beginning of the passed string.
 *  Text is surrounded by brackets, like '(180924132132S)'.
 *INPUT:
 *	char* pchBuffer - string to read the first text from (looking from start of string)
 *  int nMaxLen - length of string to consider
 *  char* pchText - Pointer to start of the parsed text (max 32)
 *OUTPUT:
 *	(int) length of parsed text, 0 if no text found and pchText is empty string.
 *------------------------------------------------------------------------------------------------*/
bool GetFirstText(char *pchBuffer, int nMaxLen, char *pchText)
{
    pchText[0] = 0;

    /*--- Find start of the text by looking at the corresponding opening bracket '(' ---*/
    int nStart = FindFirstChar(pchBuffer, '(', nMaxLen - 2);
    if (nStart < 8 || nStart > 12)
    { //Do some sanity checks
#ifdef P1_DEBUG
        Serial.println("ERROR[3]");
#endif
        return 0;
    }

    /*--- Look for the ')', terminating the text ---*/
    int nLen = FindFirstChar(pchBuffer, ')', nMaxLen) - nStart;
    if (nLen < 1 || nLen > 31)
    { //Do some more sanity checks
#ifdef P1_DEBUG
        Serial.println("ERROR[4]");
#endif
        return 0;
    }

    /*--- Copy the text to the output buffer and terminate it with '\0x00' ---*/
    strncpy(pchText, pchBuffer + nStart + 1, nLen - 1);
    pchText[nLen - 1] = 0;

    return nLen;
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

    /*--- Is this the start of P1 telegram line? ---*/
    if (nStartChar >= 0)
    {
        /*--- Start of telegram character found ('/'); restart CRC16 calculation ---*/
        nCurrentCrc = Crc16(0x0000, (unsigned char *)achTelegram + nStartChar, nLen - nStartChar);
#ifdef P1_DEBUG
        /*--- Send the telegram also through the serial debug port ---*/
        for (int cnt = nStartChar; cnt < nLen - nStartChar; cnt++)
            Serial.print(achTelegram[cnt]);
#endif
    }
    /*--- Is this the end of telegram line? ---*/
    else if (nEndChar >= 0)
    {
        /*--- Add to CRC16 calculation ---*/
        nCurrentCrc = Crc16(nCurrentCrc, (unsigned char *)achTelegram + nEndChar, 1);
        char achMessageCrc[4]; //Buffer for the CRC16 characters
        strncpy(achMessageCrc, achTelegram + nEndChar + 1, 4);
#ifdef P1_DEBUG
        for (int cnt = 0; cnt < nLen; cnt++)
            Serial.print(achTelegram[cnt]);
#endif
        /*--- Compare the CRC16 calculated with the CRC16 value received ---*/
        bValidCrcFound = (strtoul(achMessageCrc, NULL, 16) == nCurrentCrc);
        if (bValidCrcFound)
            Serial.println("\nINFO: VALID CRC FOUND!");
        else
            Serial.println("\nERROR: INVALID CRC FOUND!");
        nCurrentCrc = 0;
    }
    else
    {
        /*--- This is a data line, update CRC16 ---*/
        nCurrentCrc = Crc16(nCurrentCrc, (unsigned char *)achTelegram, nLen);
#ifdef P1_DEBUG
        for (int cnt = 0; cnt < nLen; cnt++)
            Serial.print(achTelegram[cnt]);
#endif
    }

    /*--- Done processing CRC16, parse relevant data ---*/

    // DSMR version
    // Example: 1-3:0.2.8(42)
    if (strncmp(achTelegram, DSMR_VERSION, strlen(DSMR_VERSION)) == 0)
        lDsmrVersion = GetValue(achTelegram, nLen, false);

    // Power reading timestamp (DSMR v4.0)
    // Example: 0-0:1.0.0(180924132132S)
    if (strncmp(achTelegram, DSMR_PWR_TIMESTAMP, strlen(DSMR_PWR_TIMESTAMP)) == 0)
    {
        nLen = GetLastText(achTelegram, nLen, achPwrTime);
    }

    // Power consumption low tariff (DSMR v4.0)
    // Example: 1-0:1.8.1(000992.992*kWh)
    if (strncmp(achTelegram, DSMR_PWR_LOW, strlen(DSMR_PWR_LOW)) == 0)
        lPwrLow = GetValue(achTelegram, nLen);

    // Power consumption high tariff (DSMR v4.0)
    // Example: 1-0:1.8.2(000560.157*kWh)
    if (strncmp(achTelegram, DSMR_PWR_HIGH, strlen(DSMR_PWR_HIGH)) == 0)
        lPwrHigh = GetValue(achTelegram, nLen);

    // Power return low tariff (DSMR v4.0)
    // Example: 1-0:2.8.1(000348.890*kWh)
    if (strncmp(achTelegram, DSMR_RET_LOW, strlen(DSMR_RET_LOW)) == 0)
        lReturnLow = GetValue(achTelegram, nLen);

    // Power return high tariff (DSMR v4.0)
    // Example: 1-0:2.8.2(000859.885*kWh)
    if (strncmp(achTelegram, DSMR_RET_HIGH, strlen(DSMR_RET_HIGH)) == 0)
        lReturnHigh = GetValue(achTelegram, nLen);

    // Power consumption actual total (DSMR v4.0)
    // Example: 1-0:1.7.0(00.424*kW)
    if (strncmp(achTelegram, DSMR_PWR_ACTUAL, strlen(DSMR_PWR_ACTUAL)) == 0)
        lPwrActual = GetValue(achTelegram, nLen);

    // Power consumption actual L1 (DSMR v4.0)
    // Example: 1-0:21.7.0(00.086*kW)
    if (strncmp(achTelegram, DSMR_PWR_L1, strlen(DSMR_PWR_L1)) == 0)
        lPwrL1 = GetValue(achTelegram, nLen);

    // Power consumption actual L2 (DSMR v4.0)
    // Example: 1-0:41.7.0(00.086*kW)
    if (strncmp(achTelegram, DSMR_PWR_L2, strlen(DSMR_PWR_L2)) == 0)
        lPwrL2 = GetValue(achTelegram, nLen);

    // Power consumption actual L3 (DSMR v4.0)
    // Example: 1-0:61.7.0(00.086*kW)
    if (strncmp(achTelegram, DSMR_PWR_L3, strlen(DSMR_PWR_L3)) == 0)
        lPwrL3 = GetValue(achTelegram, nLen);

    // Power return actual total (DSMR v4.0)
    // Example: 1-0:2.7.0(00.000*kW)
    if (strncmp(achTelegram, DSMR_RET_ACTUAL, strlen(DSMR_RET_ACTUAL)) == 0)
        lReturnActual = GetValue(achTelegram, nLen);

    // Power return actual L1 (DSMR v4.0)
    // Example: 1-0:22.7.0(00.086*kW)
    if (strncmp(achTelegram, DSMR_RET_L1, strlen(DSMR_RET_L1)) == 0)
        lReturnL1 = GetValue(achTelegram, nLen);

    // Power return actual L2 (DSMR v4.0)
    // Example: 1-0:42.7.0(00.086*kW)
    if (strncmp(achTelegram, DSMR_RET_L2, strlen(DSMR_RET_L2)) == 0)
        lReturnL2 = GetValue(achTelegram, nLen);

    // Power return actual L3 (DSMR v4.0)
    // Example: 1-0:62.7.0(00.086*kW)
    if (strncmp(achTelegram, DSMR_RET_L3, strlen(DSMR_RET_L3)) == 0)
        lReturnL3 = GetValue(achTelegram, nLen);

    // Power current tariff (DSMR v4.0)
    // Example: 0-0:96.14.0(0002)
    if (strncmp(achTelegram, DSMR_PWR_TARIFF, strlen(DSMR_PWR_TARIFF)) == 0)
        lPwrTariff = GetValue(achTelegram, nLen, false);

    // Gas (DSMR v4.0) on Kaifa MA105 and Landis+Gyr 350 meter
    // Example: 0-1:24.2.1(150531200000S)(00811.923*m3)
    if (strncmp(achTelegram, DSMR_GAS_METER, strlen(DSMR_GAS_METER)) == 0)
    {
        lGasMeter = GetValue(achTelegram, nLen);
        achGasTime[0] = 0;
        (void)GetFirstText(achTelegram, nLen, achGasTime);
    }

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
    bool bNew = false; //Indicates when new meter data is parsed

    if (hP1Serial.available()) //Any serial data available?
    {
        memset(achTelegram, 0, sizeof(achTelegram)); //Clear the telegram receive buffer

        /*--- Keep reading and decoding telegram lines while available on the P1 interface ---*/
        while (hP1Serial.available())
        {
            nLen = hP1Serial.readBytesUntil('\n', achTelegram, cnLineLen); //Read a line from the P1 telegram
            achTelegram[nLen] = '\n';                                      //Terminate the line
            achTelegram[nLen + 1] = 0;
            yield();
            if (DecodeTelegram(nLen + 1)) //Decode the value(s) on this telegram line, if any
                bNew = true;
        }

        /*--- Send any updated smart meter values to MQTT broker ---*/
        if (bNew)
            if (!PublishToTopic()) //Send updated data from this line
                Serial.println(" MQTT Publish failed");
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
void setup()
{
    Serial.begin(BAUDRATE); //Setup the serial console (USB) @115,200 baud

    Serial.print("\r\n \r\nBooting DSMR P1 MQTT Sensor, version ");
    Serial.println(SENSOR_VERSION); //Send our welcome message to console

    SetupWiFi(); //Setup the WiFi connection

    hP1Serial.begin(BAUDRATE); //Initialize the P1 serial interface

    SetupOTA(); //Setup OTA update service

    hMqttClient.setClient(hEspClient); //Initialzie the MQTT interface
    hMqttClient.setServer(MQTT_SERVER, MQTT_SERVER_PORT);
    (void)ConnectMqtt(); //Setup MQTT connection

    Serial.println("READY\r\n");
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
 *NOTES:
 *  None.
 *------------------------------------------------------------------------------------------------*/
void loop()
{
    /*--- Make sure we have an MQTT connection ---*/
    if (!hMqttClient.loop()) //Keep the MQTT connection alive
        (void)ConnectMqtt();

    /*--- Read, decode and send smartmeter values ---*/
    DoTelegramLines();

    /*--- Check for OTA updates ---*/
    ArduinoOTA.handle();
}
