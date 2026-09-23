#ifndef CONFIG_H
#define CONFIG_H

#include "Arduino.h"

#ifdef DEBUG
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#else
#define debug(x)
#define debugln(x)
#endif

// Captive portal redirection
#define REDIRECT_URL "http://192.168.4.1/"
static constexpr const char *NTP_SERVER = "pool.ntp.org";

#define  BATTERY_VOLTAGE_PIN 4
#define  BREWING_SIM_PIN 15  // GPIO 15 for brewing simulation mode (LOW = brewing, HIGH = normal)

#define USER_INACTIVITY_TIMEOUT_MS  (360UL * 60UL * 1000UL)
#define MACHINE_INACTIVITY_TIMEOUT_MS  (360UL * 60UL * 1000UL)
#define USER_DIM_TIMEOUT_MS  (10UL * 60UL * 1000UL)
#define MACHINE_DIM_TIMEOUT_MS  (10UL * 60UL * 1000UL)
#define DISPLAY_BRIGHTNESS_ACTIVE  180
#define DISPLAY_BRIGHTNESS_DIM  30
#define DISPLAY_ROTATION  2

// How often the sign in at startup is retried before giving up, and the pause
// between attempts. Only transport errors are retried; a rejected credential
// is final.
#define AUTH_ATTEMPTS_AT_STARTUP  3
#define AUTH_RETRY_DELAY_MS  (3UL * 1000UL)

#define uS_TO_S_FACTOR 1000000ULL

#endif
