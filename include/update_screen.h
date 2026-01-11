#pragma once

#include <stdint.h>

bool updateDateTime(void);
void updateBatteryImages(void);
void updateWiFiImages(void);
void updateStatusImages(void);
void updateShotCounters(uint32_t coffee_count, uint32_t flush_count);

// Error handling and screen redirection
void showNoConnectionScreen(const char* errorMessage);
void checkWiFiConnection(void);

// Error types enum for predefined messages
enum ErrorType {
    ERROR_WIFI_DISCONNECTED,
    ERROR_WIFI_FAILED,
    ERROR_AUTH_FAILED,
    ERROR_API_FAILED,
    ERROR_CUSTOM  // Use this to set your own custom message
};
