#include "update_screen.h"
#include "Arduino.h"
#include "time.h"
#include <ui/ui.h>
#include "WiFi.h"
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

unsigned long timeUpdate = 0;
unsigned long statusUpdate = 0;
bool statusImagesInitialized = false;

// External mutex from main.cpp
extern SemaphoreHandle_t gui_mutex;


bool updateDateTime(void)
{
    if ( WiFi.isConnected() && (millis() - timeUpdate <= TIME_UPDATE))
        return false;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        log_e("Failed to obtain time");
        return false;
    }

    char timeStr[32];

    snprintf(timeStr, sizeof(timeStr), "%02d : %02d",
             timeinfo.tm_hour,
             timeinfo.tm_min);

    // Update label with mutex protection
    if (gui_mutex && xSemaphoreTake(gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lv_label_set_text(ui_timeLabel, timeStr);
        xSemaphoreGive(gui_mutex);
    }

    timeUpdate = millis();
    return true;
}

// Get battery level (0-3) based on voltage
// ESP32 ADC reads 0-4095 for 0-3.3V range
// Typical LiPo voltage: 3.0V (empty) to 4.2V (full)
// With voltage divider, we need to scale appropriately
int getBatteryLevel(void)
{
    // Read analog value from battery pin
    int rawValue = analogRead(BATTERY_VOLTAGE_PIN);
    
    // Convert to voltage (assuming 3.3V reference and potential voltage divider)
    // For many ESP32 boards with battery, there's a 2:1 divider
    float voltage = (rawValue / 4095.0) * 3.3 * 2.0;

    Serial.print("Battery voltage: ");
    Serial.println(voltage);
    // Map voltage to battery level (0-3)
    // 3.0V = empty (0), 3.5V = low (1), 3.8V = medium (2), 4.0V+ = full (3)
    if (voltage >= 4.0) return 3;      // Full - battery3
    else if (voltage >= 3.8) return 2; // Medium - battery2
    else if (voltage >= 3.5) return 1; // Low - battery1
    else return 0;                     // Empty - battery0
}

// Get WiFi signal level (0-3) based on RSSI
int getWiFiLevel(void)
{
    if (!WiFi.isConnected()) {
        return 0; // No connection - wifi0
    }
    
    int32_t rssi = WiFi.RSSI();
    
    // Map RSSI to WiFi level (0-3)
    // -90 dBm or less = no signal (0)
    // -80 to -67 = weak (1)
    // -66 to -51 = medium (2)
    // -50 or better = strong (3)
    if (rssi >= -50) return 3;        // Strong - wifi3
    else if (rssi >= -66) return 2;   // Medium - wifi2
    else if (rssi >= -80) return 1;   // Weak - wifi1
    else return 0;                    // Very weak - wifi0
}

void updateBatteryImages(void)
{
    int batteryLevel = getBatteryLevel();
    
    // Select appropriate battery image
    const void* batteryImg = &ui_img_battery0_png;
    switch(batteryLevel) {
        case 3: batteryImg = &ui_img_battery3_png; break;
        case 2: batteryImg = &ui_img_battery2_png; break;
        case 1: batteryImg = &ui_img_battery1_png; break;
        default: batteryImg = &ui_img_battery0_png; break;
    }
    
    // Update all battery images on all screens with mutex protection
    if (gui_mutex && xSemaphoreTake(gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // NoConnectionScreen - BatImage
        if (ui_BatImage) {
            lv_img_set_src(ui_BatImage, batteryImg);
            lv_obj_clear_flag(ui_BatImage, LV_OBJ_FLAG_HIDDEN);
        }
        
        // setupWifiScreen - BatImage1
        if (ui_BatImage1) {
            lv_img_set_src(ui_BatImage1, batteryImg);
            lv_obj_clear_flag(ui_BatImage1, LV_OBJ_FLAG_HIDDEN);
        }
        
        // mainScreen - BatImage2
        if (ui_BatImage2) {
            lv_img_set_src(ui_BatImage2, batteryImg);
            lv_obj_clear_flag(ui_BatImage2, LV_OBJ_FLAG_HIDDEN);
        }
        
        xSemaphoreGive(gui_mutex);
    }
}

void updateWiFiImages(void)
{
    int wifiLevel = getWiFiLevel();
    
    // Select appropriate WiFi image
    const void* wifiImg = &ui_img_wifi0_png;
    switch(wifiLevel) {
        case 3: wifiImg = &ui_img_wifi3_png; break;
        case 2: wifiImg = &ui_img_wifi2_png; break;
        case 1: wifiImg = &ui_img_wifi1_png; break;
        default: wifiImg = &ui_img_wifi0_png; break;
    }
    
    // Update WiFi images on all screens with mutex protection
    if (gui_mutex && xSemaphoreTake(gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // NoConnectionScreen - NoWifiImage
        if (ui_NoWifiImage) {
            lv_img_set_src(ui_NoWifiImage, wifiImg);
            lv_obj_clear_flag(ui_NoWifiImage, LV_OBJ_FLAG_HIDDEN);
        }
        
        // setupWifiScreen - NoWifiImage1
        if (ui_NoWifiImage1) {
            lv_img_set_src(ui_NoWifiImage1, wifiImg);
            lv_obj_clear_flag(ui_NoWifiImage1, LV_OBJ_FLAG_HIDDEN);
        }
        
        // mainScreen - WifiImage
        if (ui_WifiImage) {
            lv_img_set_src(ui_WifiImage, wifiImg);
            lv_obj_clear_flag(ui_WifiImage, LV_OBJ_FLAG_HIDDEN);
        }
        
        xSemaphoreGive(gui_mutex);
    }
}

// Combined function to update both battery and WiFi images
void updateStatusImages(void)
{
    // Check if enough time has passed since last update (30 seconds)
    // OR if this is the first initialization
    if (!statusImagesInitialized || (millis() - statusUpdate >= TIME_UPDATE))
    {
        updateBatteryImages();
        updateWiFiImages();
        
        statusUpdate = millis();
        statusImagesInitialized = true;
    }
}

void updateShotCounters(uint32_t coffee_count, uint32_t flush_count)
{
    static uint32_t last_coffee_count = UINT32_MAX;
    static uint32_t last_flush_count = UINT32_MAX;

    if (coffee_count == last_coffee_count && flush_count == last_flush_count) {
        return;
    }

    char coffee_str[16];
    char flush_str[16];
    snprintf(coffee_str, sizeof(coffee_str), "%lu", static_cast<unsigned long>(coffee_count));
    snprintf(flush_str, sizeof(flush_str), "%lu", static_cast<unsigned long>(flush_count));

    if (gui_mutex && xSemaphoreTake(gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ui_CoffeeCountLabel) {
            lv_label_set_text(ui_CoffeeCountLabel, coffee_str);
        }
        if (ui_FlushCountLabel) {
            lv_label_set_text(ui_FlushCountLabel, flush_str);
        }
        xSemaphoreGive(gui_mutex);
    }

    last_coffee_count = coffee_count;
    last_flush_count = flush_count;
}

// Show NoConnectionScreen with custom error message
// This function redirects to the NoConnectionScreen and updates the error label
// Usage example: showNoConnectionScreen("WiFi Disconnected!\nPlease reconnect");
void showNoConnectionScreen(const char* errorMessage)
{
    // LVGL calls with mutex protection
    if (gui_mutex && xSemaphoreTake(gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Check if we're already on NoConnectionScreen to avoid recursive screen loading
        if (lv_scr_act() != ui_NoConnectionScreen)
        {
            debugln("Redirecting to NoConnectionScreen");
            lv_disp_load_scr(ui_NoConnectionScreen);
        }
        
        // Update the error label with custom message
        if (ui_ErrorLabel) {
            lv_label_set_text(ui_ErrorLabel, errorMessage);
        }
        
        xSemaphoreGive(gui_mutex);
    }
}

// WiFi connection monitoring with retry mechanism
// Checks if WiFi is connected, attempts reconnection with retries before showing error
static bool wasConnected = false;
static unsigned long lastWiFiCheck = 0;
static unsigned long reconnectStartTime = 0;
static unsigned long waitUntilTime = 0;
static const unsigned long WIFI_CHECK_INTERVAL = 5000;       // Check every 5 seconds
static const unsigned long WIFI_RECONNECT_DELAY = 30000;     // 30 seconds between retry attempts
static const unsigned long WIFI_CONNECT_TIMEOUT = 15000;     // 15 seconds to wait for connection
static const int MAX_RECONNECT_ATTEMPTS = 5;                 // Maximum retry attempts
static int reconnectAttempts = 0;
static bool isReconnecting = false;
static bool waitingForConnection = false;

void checkWiFiConnection(void)
{
    unsigned long currentMillis = millis();
    bool isConnected = WiFi.isConnected();
    
    // If connected and we were reconnecting, reset the retry counter
    if (isConnected) {
        if (isReconnecting) {
            Serial.println("✓ WiFi reconnected successfully!");
            isReconnecting = false;
            waitingForConnection = false;
            reconnectAttempts = 0;
        }
        wasConnected = true;
        return;
    }
    
    // WiFi is disconnected - handle reconnection logic
    if (!isConnected && wasConnected) {
        // First time detecting disconnection
        if (!isReconnecting) {
            Serial.println("⚠ WiFi disconnected! Starting reconnection attempts...");
            isReconnecting = true;
            reconnectAttempts = 0;
            waitUntilTime = 0;  // Start immediately
            waitingForConnection = false;
        }
        
        // If we're waiting for a connection to establish
        if (waitingForConnection) {
            // Check if we've exceeded the connection timeout
            if (currentMillis - reconnectStartTime >= WIFI_CONNECT_TIMEOUT) {
                Serial.println("⏱ Connection timeout");
                waitingForConnection = false;
                
                // Check if we've exhausted all retry attempts
                if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
                    Serial.println("❌ All reconnection attempts failed!");
                    Serial.println("Showing NoConnectionScreen...");
                    
                    isReconnecting = false;
                    reconnectAttempts = 0;
                    wasConnected = false;
                    
                    showNoConnectionScreen(
                        "WiFi Connection Lost!\n"
                        "Failed to reconnect\n"
                        "after 5 attempts.\n"
                        "Please restart WiFi"
                    );
                } else {
                    // Schedule next attempt in 30 seconds
                    waitUntilTime = currentMillis + WIFI_RECONNECT_DELAY;
                    Serial.print("⏳ Next attempt in 30 seconds... (");
                    Serial.print(MAX_RECONNECT_ATTEMPTS - reconnectAttempts);
                    Serial.println(" attempts remaining)");
                }
            }
            // Still waiting for connection, check periodically
            else if (currentMillis - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
                lastWiFiCheck = currentMillis;
                // Connection check happens at top of function
            }
            return;
        }
        
        // If it's time to attempt reconnection
        if (currentMillis >= waitUntilTime) {
            reconnectAttempts++;
            
            Serial.print("🔄 Reconnection attempt ");
            Serial.print(reconnectAttempts);
            Serial.print(" of ");
            Serial.println(MAX_RECONNECT_ATTEMPTS);
            
            // Attempt to reconnect
            WiFi.reconnect();
            reconnectStartTime = currentMillis;
            waitingForConnection = true;
        }
    }
    
    // Regular WiFi status check
    if (currentMillis - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
        lastWiFiCheck = currentMillis;
    }
}
