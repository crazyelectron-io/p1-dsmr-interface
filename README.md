# README.md for P1 DSMR interface programme

This is a continuously running program (on an ESP8266) for interfacing with a Dutch smart meter, through their P1 port, and publish the data via MQTT.
The telegrams received on the P1 port are parsed and the relevant usage data is sent as a MQTT message where others can subscribe to (in particular OpenHAB 2).

Currently only tested on a Landys+Gyr 350 using DSMR v4, which produces a P1 telegram every 10s.
An example telegram looks like:

```ini
    /XMX5LGBBFFB231314239

    1-3:0.2.8(42)                                           // DSMR version (4.2)
    0-0:1.0.0(180924132132S)                                // Timestamp (yymmddhhmmssS) S=Summer time
    0-0:96.1.1(4532323036303137363437393334353135)          // Serial number (ASCII)
    1-0:1.8.1(011522.839*kWh)                               // Consumption T1 tariff
    1-0:1.8.2(010310.991*kWh)                               // Consumption T2 tariff
    1-0:2.8.1(000000.000*kWh)                               // Return T1 tariff
    1-0:2.8.2(000000.000*kWh)                               // Return T2 tariff
    0-0:96.14.0(0002)                                       // Current tariff (T1 or T2)
    1-0:1.7.0(00.503*kW)                                    // Actual consumption all phases
    1-0:2.7.0(00.000*kW)                                    // Actual return all phases
    0-0:96.7.21(00015)
    0-0:96.7.9(00005)                                       // Number of long power failures in any phase
    1-0:99.97.0(5)(0-0:96.7.19)(170520130938S)(0000005627*s)(170325044014W)(0043178677*s) [truncated]
    1-0:32.32.0(00002)
    1-0:52.32.0(00002)
    1-0:72.32.0(00002)
    1-0:32.36.0(00000)
    1-0:52.36.0(00000)
    1-0:72.36.0(00000)
    0-0:96.13.1()
    0-0:96.13.0()
    1-0:31.7.0(001*A)
    1-0:51.7.0(001*A)
    1-0:71.7.0(001*A)
    1-0:21.7.0(00.086*kW)                                   // Actual consumption on L1
    1-0:41.7.0(00.250*kW)                                   // Actual consumption on L2
    1-0:61.7.0(00.166*kW)                                   // Actual consumption on L3
    1-0:22.7.0(00.000*kW)                                   // Actual return on L1
    1-0:42.7.0(00.000*kW)                                   // Actual return on L2
    1-0:62.7.0(00.000*kW)                                   // Actual return on L3
    0-1:24.1.0(003)                                         // Slave (Gas meter) device type
    0-1:96.1.0(4731303138333430313538383732343334)          // Gas meter serial number
    0-1:24.2.1(180924130000S)(04890.857*m3)                 // Gas meter time stamp + value
    !FCA6                                                   // CRC16 cehcksum of entire telegram (from / to !)
```

More details can be found [here](https://electronicsworkbench.io/blog/smartmeter-1).

The JSON object sent to MQTT has the following specs:

```json
- MQTT topic: sensor/dsmr
- MQTT message:
    {
      "dsmr": "42",
      "power":
      {
        "time": "180924132132S",
        "tariff": "2",
        "use":
        {
          "total":
          {
            "T1": "11522839",
            "T2": "10310991"
          },
          "actual":
          {
            "total": "503",
            "L1": "86",
            "L2": "250",
            "L3": "166"
          }
        },
        "return":
        {
          "total":
          {
            "T1": "0",
            "T2": "0"
          },
          "actual":
          {
            "total": "503",
            "L1": "86",
            "L2": "250",
            "L3": "166"
          }
        }
      },
      "gas":
      {
        "time": "180924130000S",
        "total": "4890857"
      }
    }
```

All power readings are specified in Wh, gas reading is 1/1000 m3.

The advantage of using a nested JSON structure like above is that we can add elements without affecting existing logic to extract values.

The default MQTT packet size of the used Arduino PubSubClient library is too small for the messages we are sending. Increase it to 512 in the PubSubClient.h file. Be aware that adding a '#define MQTT_MAX_PACKET_SIZE' it to this source file before the include doesn't work because of the order the library headers are processed during pre-compile!
See also: https://github.com/knolleary/pubsubclient/issues/431.

**VERSION HISTORY:**
  v0.1    Initial test version using HTTP calls to a webservice.
  v0.2    Send MQTT messages in stead of making HTTP calls to a webservice.
  v0.3    Expanded MQTT message to JSON structure.
  v0.4    Adapted to recompiled Arduino PubSubClient library for larger packet size (up to 256).
  v0.5    Added extra fields to MQTT message, cleanup of the code and some extra comments.
  v0.6    Extended StaticJsonBuffer to fix overflow issue resulting in last value not being sent.

**COPYRIGHT:**
This program comes with ABSOLUTELY NO WARRANTY. Use at your own risk. This is free software, and you are welcome to redistribute it under certain conditions. The program and its source code are published under the GNU General Public License (GPL).  See http://www.gnu.org/licenses/gpl-3.0.txt for details. Some parts are based on other open source code.
