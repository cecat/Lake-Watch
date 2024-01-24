/*
  Monitor cabin power
    C. Catlett Apr 2019
    2020-Oct
      - added MQTT to connect to remote Home Assistant
      - added ds18b20 temperature sensor to monitor crawlspace
    2021-Jan
      - added connection check to MQTT just to be safe                       
 */

#include <Particle.h>
#include <MQTT.h>
#include "secrets.h" 
#include <OneWire.h>
#include <DS18B20.h>
// include topics for mqtt
#include "topics.h"
// include misc variables
#include "vars.h"

FuelGauge fuel;                   // lipo battery
DS18B20  sensor(D1, true);        // DS18B20 temperature sensor (needs libraries OneWire and DS18B20)

// MQTT 

#define MQTT_KEEPALIVE 35 * 60              // 60s is std default
void timer_callback_send_mqqt_data();    
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
     char p[length + 1];
     memcpy(p, payload, length);
     p[length] = 0; 
     Particle.publish("mqtt recvd", p, 3600, PRIVATE);
 }
MQTT client(MY_SERVER, 1883, MQTT_KEEPALIVE, mqtt_callback);
int MQTT_CODE = 0;
Timer checkTimer(FIVE_MIN, checkPower);
Timer reportTimer(REPORT, reportPower);
bool  TimeToCheck     = TRUE;
bool  TimeToReport    = TRUE;

// Application watchdog - sometimes MQTT wedges things
int DOGTIME = 120000;           // wait 2 minutes before pulling the ripcord
retained bool REBORN  = FALSE;  // did app watchdog restart us?
ApplicationWatchdog *wd;
void watchdogHandler() {
  REBORN = TRUE;
  System.reset(RESET_NO_WAIT);
}
retained bool SELF_RESTART = FALSE;
int fails = 0;
int GIVE_UP = 3;

void setup() {
    Time.zone (-5);
    Particle.syncTime();

    wd = new ApplicationWatchdog(DOGTIME, watchdogHandler, 1536); // restart after DOGTIME sec no pulse
    if (REBORN) {
      Particle.publish("****WEDGED****", "app watchdog restart", 3600, PRIVATE);
      REBORN = FALSE;
    }
    if (SELF_RESTART) {
      Particle.publish("----STUCK----", "self-reboot after 4 mqtt fails", 3600, PRIVATE);
      SELF_RESTART = FALSE;
    }

    fuelPercent = fuel.getSoC();
    Particle.publish("mqtt_startup", "Attempting to connect to HA", 3600, PRIVATE);
    client.connect(CLIENT_NAME, HA_USR, HA_PWD);
    // check MQTT 
    if (client.isConnected()) {
        Particle.publish("mqtt_startup", "Connected to HA", 3600, PRIVATE);
        //client.disconnect();
      } else {
        Particle.publish("mqtt_startup", "Fail connect HA - check secrets.h", 3600, PRIVATE);
    }
    //client.disconnect();
    checkTimer.start();
    reportTimer.start();
}

void loop() {

// check everything when timer fires; notify only state changes
    if (TimeToCheck) {
        TimeToCheck = FALSE;
        fuelPercent = fuel.getSoC(); 
        powerSource = System.powerSource();
        if (powerSource == LINE_PWR) {
          if (!PowerIsOn) {
            tellHASS(TOPIC_B, String(powerSource));
            Particle.publish("POWER-start ON", String(powerSource), PRIVATE);
            reportTimer.changePeriod(REPORT);
          }
          PowerIsOn = TRUE;
        } else {
          if (PowerIsOn) {
            tellHASS(TOPIC_C, String(powerSource));
            Particle.publish("POWER OUT", String(powerSource), PRIVATE);
            reportTimer.changePeriod(FIVE_MIN);
          }
          PowerIsOn = FALSE;
        }
        // check crawlspace
        crawlTemp = getTemp();
        if (crawlTemp > allGood) {
          if (inDanger) {
            tellHASS(TOPIC_E, String(crawlTemp));
            inDanger=FALSE;
          }
        }
        if (crawlTemp < danger)    { 
          tellHASS(TOPIC_F, String(crawlTemp)); 
          if (crawlTemp < Freezing)  { 
            tellHASS(TOPIC_G, String(crawlTemp)); 
            Particle.publish("CRAWLSPACE DANGER", String(powerSource), PRIVATE);
            inDanger=TRUE;
          }
        }
    }

    if (TimeToReport) {
      TimeToReport = FALSE;
      wd->checkin(); // poke app watchdog we're going in...

      //client.disconnect();
      if (client.isConnected()) {
        //Particle.publish("mqtt", "connected ok", 3600, PRIVATE); delay(100);
      } else {  
        Particle.publish("mqtt", "reconnecting", 3600, PRIVATE); delay(100);
        client.connect(CLIENT_NAME, HA_USR,HA_PWD);
        delay(2000);
      }

      if (client.isConnected()){
        fails=0;
        tellHASS(TOPIC_A, String(fuelPercent));
        if (PowerIsOn) {  tellHASS(TOPIC_B, String(fuelPercent));
            } else {  tellHASS(TOPIC_C, String(fuelPercent)); }
        tellHASS(TOPIC_D, String(crawlTemp));
        if (inDanger) {
          if (crawlTemp < Freezing) {
            tellHASS(TOPIC_G, String(crawlTemp));
         } else tellHASS(TOPIC_F, String(crawlTemp));
        }
        tellHASS(TOPIC_H, String(mqttCt));
        tellHASS(TOPIC_I, String(mqttFails));
        //client.disconnect();
      } else {
        Particle.publish("mqtt", "Failed to connect", 3600, PRIVATE);
        mqttFails++;
        fails++;
        if (fails > GIVE_UP) {
          SELF_RESTART=TRUE;
          Particle.publish("-STUCK-", "Too many fails- restarting", 3600, PRIVATE);
          System.reset(RESET_NO_WAIT);
        }
      }
      Particle.publish("MQTT", String("Fail rate " + String(mqttFails) + "/" + String(mqttCt)),3600, PRIVATE);
      void myWatchdogHandler(void); // reset the dog
    }
} 
/************************************/
/***         FUNCTIONS       ***/
/************************************/

// Checking timer interrupt handler
void checkPower() {  TimeToCheck = TRUE;  }

// Reporting timer interrupt handler
void reportPower() {  TimeToReport = TRUE;  }

// Report to HASS via MQTT
void tellHASS (const char *ha_topic, String ha_payload) {  

  delay(100); // bit of delay in between successive messages
  if (client.isConnected()){
    client.publish(ha_topic, ha_payload);
    mqttCt++;
  } else {
    mqttFails++;
    Particle.publish("mqtt", "Connection dropped", 3600, PRIVATE);
  }
}

//  Check the crawlspace on the DS18B20 sensor (code from Lib examples)

double getTemp() {  
  float _temp;
  float fahrenheit = 0;
  int   i = 0;

  do {  _temp = sensor.getTemperature();
  } while (!sensor.crcCheck() && MAXRETRY > i++);
  if (i < MAXRETRY) {  
    fahrenheit = sensor.convertToFahrenheit(_temp);
  } else {
    Particle.publish("ERROR", "Invalid reading", PRIVATE);
  }
  return (fahrenheit); 
}

