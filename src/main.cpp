#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <LV_Helper.h>
#include <ui/ui.h>
#include "Preferences.h"
#include "web.h"
#include <WiFi.h>
#include "config.h"
#include "update_screen.h"
#include "lamarzocco_client.h"
#include "lamarzocco_websocket.h"
#include "lamarzocco_machine.h"
#include "lamarzocco_auth.h"
#include "boiler_display.h"
#include "water_alarm.h"
#include "brewing_display.h"
#include "activity_monitor.h"

Preferences preferences;
LaMarzoccoClient* g_client = nullptr;
LaMarzoccoWebSocket* g_websocket = nullptr;
LaMarzoccoMachine* g_machine = nullptr;

LilyGo_Class amoled;
SemaphoreHandle_t gui_mutex;
void Task_LVGL(void *pvParameters);
void updateSerialLoggingPowerState(bool force);

// WiFi connection variables
const int MAX_WIFI_RETRIES = 10;
const int WIFI_TIMEOUT_MS = 15000;

bool connectToWiFi(const String &ssid, const String &password)
{
  debugln("Attempting to connect to WiFi...");
  WiFi.begin(ssid.c_str(), password.c_str());
  WiFi.setSleep(false);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < MAX_WIFI_RETRIES)
  {
    delay(WIFI_TIMEOUT_MS / MAX_WIFI_RETRIES);
    debug(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    debugln("");
    debugln("WiFi connected!");
    debug("IP address: ");
    debugln(WiFi.localIP());
    return true;
  }
  else
  {
    debugln("");
    debugln("Failed to connect to WiFi");
    WiFi.disconnect();
    return false;
  }
}

void updateSerialLoggingPowerState(bool force)
{
  static bool serial_enabled = true;
  static unsigned long last_check_ms = 0;
  unsigned long now = millis();

  if (!force && (now - last_check_ms) < 5000) {
    return;
  }
  last_check_ms = now;

  bool usb_powered = amoled.isVbusIn();
  if (usb_powered && !serial_enabled) {
    Serial.begin(115200);
    serial_enabled = true;
  } else if (!usb_powered && serial_enabled) {
    Serial.flush();
    Serial.end();
    serial_enabled = false;
  }
}

//function to enter deep sleep mode.  This helps to save power when device not in use and using a battery.
void enterDeepSleep() {
    Serial.println("Preparing to sleep...");
    
    // 1. Turn off the display so you know it worked
    amoled.setBrightness(0);
    
    // 2. CRITICAL: Wait for button release!
    // This loop blocks the code until you let go of the button.
    // Otherwise, the device sleeps and wakes up instantly.
    while (digitalRead(0) == LOW) {
        delay(10);
    }
    
    // 3. Small debounce delay to ensure the signal is clean
    delay(100);

    Serial.println("Goodnight!");

    // 4. Configure Wakeup Source
    // Wake up when GPIO 0 goes LOW (Pressed again)
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    
    // 5. Enter Deep Sleep
    esp_deep_sleep_start();
}

void setup()
{
  Serial.begin(115200);
  preferences.begin("config", false);
  pinMode(0, INPUT_PULLUP);

  bool rslt = false;

  // Automatically determine the access device
  rslt = amoled.begin();

  if (!rslt)
  {
    while (1)
    {
      debug("The board model cannot be detected, please raise the Core Debug Level to an error");
      delay(1000);
    }
  }

  updateSerialLoggingPowerState(true);

  activity_monitor_init(USER_INACTIVITY_TIMEOUT_MS, MACHINE_INACTIVITY_TIMEOUT_MS);

  gui_mutex = xSemaphoreCreateMutex();
  if (gui_mutex == NULL)
  {
    // Handle semaphore creation failure
    log_i("gui_mutex semaphore creation failure");
    return;
  }

  xTaskCreatePinnedToCore(Task_LVGL,
                          "Task_LVGL",
                          1024 * 16,  // Increased for crypto operations
                          NULL,
                          3,
                          NULL,
                          0);

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  delay(500);

  String ssid = preferences.getString("SSID", "");
  String pass = preferences.getString("PASS", "");
  if (ssid == "" || pass == "")
  {
    debugln("No WiFi credentials found, starting WiFi setup");
    lv_disp_load_scr(ui_NoConnectionScreen);
    setupWEB();
  }
  else
  {
    debugln("Found WiFi credentials");
    if (connectToWiFi(ssid, pass))
    {
      lv_disp_load_scr(ui_mainScreen);
      
      // Initialize La Marzocco client
      String email = preferences.getString("USER_EMAIL", "");
      String password = preferences.getString("USER_PASS", "");
      String machine_serial = preferences.getString("MACHINE", "");
      
      if (email.length() > 0 && password.length() > 0 && machine_serial.length() > 0) {
        debugln("Initializing La Marzocco client...");
        
        // Check if installation key exists, if not generate it first
        InstallationKey key;
        if (!LaMarzoccoAuth::load_installation_key(preferences, key)) {
          debugln("Generating installation key...");
          
          // Clear any partial keys that might exist (check before removing to avoid errors)
          if (preferences.isKey("INSTALLATION_ID")) preferences.remove("INSTALLATION_ID");
          if (preferences.isKey("INSTALLATION_SECRET")) preferences.remove("INSTALLATION_SECRET");
          if (preferences.isKey("INSTALLATION_PRIVKEY")) preferences.remove("INSTALLATION_PRIVKEY");
          if (preferences.isKey("INSTALLATION_PUBKEY")) preferences.remove("INSTALLATION_PUBKEY");
          if (preferences.isKey("INSTALLATION_PRIVKEY_LEN")) preferences.remove("INSTALLATION_PRIVKEY_LEN");
          if (preferences.isKey("INSTALLATION_PUBKEY_LEN")) preferences.remove("INSTALLATION_PUBKEY_LEN");
          if (preferences.isKey("INST_ID")) preferences.remove("INST_ID");
          if (preferences.isKey("INST_SECRET")) preferences.remove("INST_SECRET");
          if (preferences.isKey("INST_PRIVKEY")) preferences.remove("INST_PRIVKEY");
          if (preferences.isKey("INST_PUBKEY")) preferences.remove("INST_PUBKEY");
          if (preferences.isKey("INST_PRIVLEN")) preferences.remove("INST_PRIVLEN");
          if (preferences.isKey("INST_PUBLEN")) preferences.remove("INST_PUBLEN");
          
          String installation_id = LaMarzoccoAuth::generate_uuid();
          if (LaMarzoccoAuth::generate_installation_key(installation_id, key)) {
            if (LaMarzoccoAuth::save_installation_key(preferences, key)) {
              debugln("Installation key generated and saved");
            } else {
              debugln("Failed to save installation key");
            }
          } else {
            debugln("Failed to generate installation key");
          }
        } else {
          debugln("Installation key found");
        }
        
        g_client = new LaMarzoccoClient(preferences);
        if (g_client->init(email, password, machine_serial)) {
          // Register client if needed
          debugln("Registering client...");
          if (!g_client->register_client()) {
            debugln("Registration failed - will retry on first API call");
            // Note: Registration failures are not critical, will retry during API calls
          }
          
          // Try to get access token (authenticate)
          if (!g_client->get_access_token()) {
            debugln("Authorization failed - invalid credentials");
            

            showNoConnectionScreen(
              "Authorization Failed!\n"
              "Invalid credentials\n"
              "Please restart WiFi Setup"
            );
            
            delete g_client;
            g_client = nullptr;
            setupWEB();
          } else {
            // Initialize websocket and machine
            g_websocket = new LaMarzoccoWebSocket(*g_client);
            g_machine = new LaMarzoccoMachine(*g_client, *g_websocket);
            
            debugln("La Marzocco client initialized");

            // Initial refresh of coffee/flush counters
            g_machine->request_stats_refresh();
            
            // Auto-connect WebSocket on startup
            debugln("Auto-connecting to WebSocket...");
            if (g_machine->connect_websocket()) {
              debugln("✓ WebSocket connection initiated on startup");
            } else {
              debugln("✗ Failed to initiate WebSocket connection on startup");
              // Note: WebSocket failures are not critical, will retry automatically
            }
          }
        } else {
          debugln("Failed to initialize La Marzocco client");
          
          showNoConnectionScreen(
            "Client Init Failed!\n"
            "Missing installation key\n"
            "Please restart WiFi Setup"
          );
          
          delete g_client;
          g_client = nullptr;
          setupWEB();
        }
      } else {
        debugln("Missing La Marzocco credentials");
        // Note: Missing credentials is expected on first run, no error message needed
      }
    }
    else
    {
      debugln("WiFi connection failed after retries, starting WiFi setup");
      lv_disp_load_scr(ui_NoConnectionScreen);
      setupWEB();
    }
  }
}

void loop()
{
  updateDateTime();
  updateStatusImages();  // Update battery and WiFi images (initial + every 30 seconds)
  checkWiFiConnection(); // Monitor WiFi connection and redirect if disconnected
  updateSerialLoggingPowerState(false);
  
  // Check GPIO 15 for brewing simulation mode
  brewing_display_check_gpio_simulation();
  
  // Handle websocket and machine loop - MUST be called frequently
  // WebSocket requires regular loop() calls to process messages
  if (g_machine) {
    g_machine->loop();  // This calls websocket.loop()
  }
  
  // Small delay to prevent watchdog issues, but keep loop responsive
  delay(10);
  
  // Fast reconnection check (every 5 seconds)
  static unsigned long last_check = 0;
  static unsigned long last_reconnect_attempt = 0;
  static const unsigned long CHECK_INTERVAL = 5000;     // Check every 5 seconds
  static const unsigned long RECONNECT_INTERVAL = 10000; // Try reconnect every 10 seconds
  
  if (millis() - last_check > CHECK_INTERVAL) {
    last_check = millis();
    if (g_machine) {
      if (g_machine->is_websocket_connected()) {
        // Connected - only log occasionally to reduce noise
        static unsigned long last_log = 0;
        if (millis() - last_log > 60000) { // Log every 60 seconds when connected
          Serial.println("[STATUS] ✓ WebSocket connected");
          last_log = millis();
        }
      } else {
        // Disconnected - try to reconnect quickly
        if (millis() - last_reconnect_attempt > RECONNECT_INTERVAL) {
          last_reconnect_attempt = millis();
          Serial.println("[RECONNECT] WebSocket disconnected, reconnecting...");
          
          // Try to reconnect
          if (g_machine->connect_websocket()) {
            Serial.println("[RECONNECT] ✓ Reconnection initiated");
          } else {
            Serial.println("[RECONNECT] ✗ Reconnection failed, will retry in 10s");
          }
        }
      }
    }
  }

  static bool display_dimmed = false;
  unsigned long last_user_ms = activity_monitor_last_user_ms();
  unsigned long last_machine_ms = activity_monitor_last_machine_ms();
  unsigned long now = millis();
  bool user_dim_inactive = (USER_DIM_TIMEOUT_MS > 0) &&
                           (now >= last_user_ms) &&
                           (static_cast<uint32_t>(now - last_user_ms) >= USER_DIM_TIMEOUT_MS);
  bool machine_dim_inactive = (MACHINE_DIM_TIMEOUT_MS > 0) &&
                              (now >= last_machine_ms) &&
                              (static_cast<uint32_t>(now - last_machine_ms) >= MACHINE_DIM_TIMEOUT_MS);
  bool should_dim = user_dim_inactive && machine_dim_inactive;
  if (should_dim && !display_dimmed) {
    amoled.setBrightness(DISPLAY_BRIGHTNESS_DIM);
    display_dimmed = true;
  } else if (!should_dim && display_dimmed) {
    amoled.setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
    display_dimmed = false;
  }

  bool user_inactive = activity_monitor_is_user_inactive(now);
  bool machine_inactive = activity_monitor_is_machine_inactive(now);
  if (user_inactive && machine_inactive) {
    Serial.print("[SLEEP] Inactivity timeout: ");
    Serial.println("user + machine");
    enterDeepSleep();
  }
  // Check if BOOT button (GPIO 0) is held down to turn OFF
    // (GPIO 0 is LOW when pressed)
    if (digitalRead(0) == LOW) {
        delay(100); // Debounce
        unsigned long startTime = millis();
        
        // Wait to see if user holds it for 2 seconds
        while (digitalRead(0) == LOW) {
            if (millis() - startTime > 2000) {
                // User held it for 2 seconds -> SLEEP
                enterDeepSleep(); 
            }
        }
    }
}

void Task_LVGL(void *pvParameters)
{
  beginLvglHelper(amoled);
  ui_init();
  
  // Initialize boiler display system after UI is ready
  boiler_display_set_mutex((void*)gui_mutex);  // Set mutex for thread-safe LVGL access
  boiler_display_init();
  
  // Initialize water alarm display system
  water_alarm_set_mutex((void*)gui_mutex);
  water_alarm_init();
  
  // Initialize brewing display system
  brewing_display_set_mutex((void*)gui_mutex);
  brewing_display_init();
  
  // Main LVGL loop
  while (1)
  {
    if (xSemaphoreTake(gui_mutex, portMAX_DELAY) == pdTRUE)
    {
      lv_timer_handler();
      xSemaphoreGive(gui_mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
