#include "brewing_display.h"
#include "water_alarm.h"
#include "config.h"
#include "ui/ui.h"
#include <Arduino.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Debug output
#define DEBUG_BREWING 1
#if DEBUG_BREWING
#define brewing_debug(x) Serial.print(x)
#define brewing_debugln(x) Serial.println(x)
#else
#define brewing_debug(x)
#define brewing_debugln(x)
#endif

// Brewing states
typedef enum {
    BREWING_STATE_IDLE = 0,      // Not brewing
    BREWING_STATE_ACTIVE = 1,    // Currently brewing
    BREWING_STATE_FLASHING = 2   // Flashing final time after brewing stops
} BrewingState;

// Global variables
static bool g_initialized = false;
static BrewingState g_state = BREWING_STATE_IDLE;
static int64_t g_brewing_start_time = 0;
static int g_final_seconds = 0;  // Final seconds value to flash
static lv_timer_t* g_update_timer = NULL;
static bool g_timer_paused = true;
static SemaphoreHandle_t g_gui_mutex = NULL;
static unsigned long g_flash_start_time = 0;
static const unsigned long FLASH_DURATION_MS = 3000;  // 3 seconds
static const unsigned long FLASH_TOGGLE_MS = 200;     // Flash toggle every 200ms
static bool g_gpio_simulation_active = false;
static bool g_last_gpio_state = true;
static bool g_allow_update_from_gpio = false;

// Forward declarations
static void update_elapsed_time_display(void);
static void show_brewing_ui(void);
static void hide_brewing_ui(void);
static void restore_normal_ui(void);
static void restore_normal_ui_no_mutex(void);  // Version without mutex for timer callback
static void start_brewing(int64_t start_time);
static void stop_brewing(void);

// Helper macro for mutex protection
#define TAKE_MUTEX() if (g_gui_mutex && xSemaphoreTake(g_gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
#define GIVE_MUTEX() if (g_gui_mutex) xSemaphoreGive(g_gui_mutex)

/**
 * Set the GUI mutex for thread-safe LVGL access
 */
void brewing_display_set_mutex(void* mutex) {
    g_gui_mutex = (SemaphoreHandle_t)mutex;
    brewing_debugln("[Brewing] Mutex set for thread-safe operation");
}

/**
 * Initialize the brewing display system
 */
void brewing_display_init(void) {
    if (g_initialized) {
        brewing_debugln("[Brewing] Already initialized");
        return;
    }
    
    brewing_debugln("[Brewing] Initializing brewing display system...");
    
    // Initialize GPIO 15 for brewing simulation mode
    pinMode(BREWING_SIM_PIN, INPUT_PULLUP);
    g_last_gpio_state = digitalRead(BREWING_SIM_PIN) == HIGH;
    brewing_debug("[Brewing] GPIO 15 initialized (current state: ");
    brewing_debug(g_last_gpio_state ? "HIGH" : "LOW");
    brewing_debugln(")");
    
    TAKE_MUTEX() {
        // Hide brewing elements by default
        if (ui_SecPanel) {
            lv_obj_add_flag(ui_SecPanel, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_SecValueLabel) {
            lv_obj_add_flag(ui_SecValueLabel, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_SecondsLabel) {
            lv_obj_add_flag(ui_SecondsLabel, LV_OBJ_FLAG_HIDDEN);
        }
        GIVE_MUTEX();
    }
    
    // Create a timer for periodic updates (50ms for very smooth real-time updates)
    // 50ms = 20 updates per second, which gives smooth tenths updates
    g_update_timer = lv_timer_create(brewing_display_timer_callback, 50, NULL);
    if (g_update_timer) {
        lv_timer_set_repeat_count(g_update_timer, -1);  // Repeat indefinitely
        lv_timer_pause(g_update_timer);  // Start paused
    }
    g_timer_paused = true;
    
    g_initialized = true;
    g_state = BREWING_STATE_IDLE;
    g_brewing_start_time = 0;
    g_final_seconds = 0;
    brewing_debugln("[Brewing] Initialization complete");
}

/**
 * Start brewing mode
 */
static void start_brewing(int64_t start_time) {
    if (g_state == BREWING_STATE_ACTIVE) {
        // Already brewing, just update start time if different
        if (start_time != g_brewing_start_time) {
            g_brewing_start_time = start_time;
        }
        return;
    }
    
    brewing_debugln("[Brewing] ===== STARTING BREWING MODE =====");
    g_state = BREWING_STATE_ACTIVE;
    g_brewing_start_time = start_time;
    g_final_seconds = 0;
    
    // Show brewing UI (sets initial 0.0 display)
    show_brewing_ui();
    
    // Start/resume timer for real-time updates (50ms = 20 updates/sec)
    if (g_update_timer) {
        lv_timer_set_period(g_update_timer, 50);
        if (g_timer_paused) {
            lv_timer_resume(g_update_timer);
            g_timer_paused = false;
            brewing_debugln("[Brewing] Timer started (50ms period)");
        }
    }
}

/**
 * Stop brewing mode
 */
static void stop_brewing(void) {
    // Every dashboard message that does not say "Brewing" arrives here, and a
    // machine at rest sends plenty of them - a boiler reaching temperature, a
    // status change after the shot. Only leaving an active shot is a stop.
    //
    // Without this the second message ran the ending a second time while the
    // final time was still flashing. The start time had already been cleared
    // by the first one, so the elapsed time came out as zero: the display
    // flashed "0" instead of the time the shot took, and the three seconds
    // started over.
    if (g_state != BREWING_STATE_ACTIVE) {
        return;
    }
    
    brewing_debugln("[Brewing] ===== STOPPING BREWING MODE =====");
    
    // Capture final seconds value for flashing
    if (g_brewing_start_time > 0) {
        int64_t now_ms = brewing_display_get_current_time_ms();
        int64_t elapsed_ms = now_ms - g_brewing_start_time;
        if (elapsed_ms > 0) {
            g_final_seconds = (int)(elapsed_ms / 1000);
        } else {
            g_final_seconds = 0;
        }
    } else {
        g_final_seconds = 0;
    }
    
    brewing_debug("[Brewing] Final seconds to flash: ");
    brewing_debugln(g_final_seconds);
    
    // Transition to flashing state
    g_state = BREWING_STATE_FLASHING;
    g_flash_start_time = millis();
    g_brewing_start_time = 0;  // Clear start time to stop timer updates
    
    // Update display to show only seconds (no tenths) for flashing
    char seconds_str[16];
    snprintf(seconds_str, sizeof(seconds_str), "%d", g_final_seconds);
    
    TAKE_MUTEX() {
        // Show brewing elements for flashing
        if (ui_SecValueLabel) {
            lv_label_set_text(ui_SecValueLabel, seconds_str);
            lv_obj_clear_flag(ui_SecValueLabel, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_SecPanel) {
            lv_obj_clear_flag(ui_SecPanel, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_SecondsLabel) {
            lv_obj_clear_flag(ui_SecondsLabel, LV_OBJ_FLAG_HIDDEN);
        }
        GIVE_MUTEX();
    }
    
    // Ensure timer is running for flash effect
    if (g_update_timer) {
        if (g_timer_paused) {
            lv_timer_resume(g_update_timer);
            g_timer_paused = false;
        }
        // Set period to 50ms for smooth flash toggling
        lv_timer_set_period(g_update_timer, 50);
    }
    
    brewing_debugln("[Brewing] Entered flashing state - will restore UI after 3 seconds");
}

/**
 * Update brewing state based on machine status
 * 
 * This function is called from:
 * 1. Websocket handler (lamarzocco_machine.cpp) when machine status changes
 * 2. GPIO simulation (brewing_display_check_gpio_simulation) when GPIO state changes
 * 
 * GPIO simulation takes priority: if GPIO simulation is active, websocket updates are ignored
 * (unless g_allow_update_from_gpio is set, which allows GPIO to control its own state)
 */
void brewing_display_update(bool is_brewing, int64_t brewing_start_time) {
    if (!g_initialized) {
        brewing_debugln("[Brewing] ERROR: Not initialized!");
        return;
    }
    
    // If GPIO simulation is active, ignore websocket updates
    // Exception: allow updates if they're coming from GPIO simulation itself
    if (g_gpio_simulation_active && !g_allow_update_from_gpio) {
        brewing_debugln("[Brewing] Ignoring websocket update - GPIO simulation is active");
        return;
    }
    
    // Reset the flag after checking
    g_allow_update_from_gpio = false;
    
    // Handle state transitions (same logic for both GPIO and websocket)
    if (is_brewing) {
        // Start brewing
        if (brewing_start_time <= 0) {
            // Use current time if no start time provided (GPIO simulation case)
            brewing_start_time = brewing_display_get_current_time_ms();
        }
        start_brewing(brewing_start_time);
    } else {
        // Stop brewing (enters flashing state, then restores UI after 3 seconds)
        stop_brewing();
    }
}

/**
 * Timer callback for periodic updates
 */
void brewing_display_timer_callback(lv_timer_t* timer) {
    if (!g_initialized) return;
    
    switch (g_state) {
        case BREWING_STATE_ACTIVE:
            // Update elapsed time during brewing
            update_elapsed_time_display();
            break;
            
        case BREWING_STATE_FLASHING:
            // Handle flashing effect
            {
                unsigned long elapsed = millis() - g_flash_start_time;
                
                if (elapsed >= FLASH_DURATION_MS) {
                    // Flash duration complete - return to idle and restore normal UI
                    brewing_debugln("[Brewing] Flash complete (3 seconds) - returning to normal UI");
                    g_state = BREWING_STATE_IDLE;
                    g_final_seconds = 0;
                    
                    // Restore normal UI (timer callback already has mutex, so use no-mutex version)
                    restore_normal_ui_no_mutex();
                    
                    // Pause timer
                    if (g_update_timer && !g_timer_paused) {
                        lv_timer_pause(g_update_timer);
                        g_timer_paused = true;
                        brewing_debugln("[Brewing] Timer paused");
                    }
                } else {
                    // Flash effect: toggle visibility every FLASH_TOGGLE_MS
                    bool visible = (elapsed / FLASH_TOGGLE_MS) % 2 == 0;
                    
                    // Timer callback runs within LVGL task context (mutex already held)
                    if (ui_SecPanel) {
                        if (visible) {
                            lv_obj_clear_flag(ui_SecPanel, LV_OBJ_FLAG_HIDDEN);
                        } else {
                            lv_obj_add_flag(ui_SecPanel, LV_OBJ_FLAG_HIDDEN);
                        }
                        lv_obj_invalidate(ui_SecPanel);
                    }
                    if (ui_SecValueLabel) {
                        if (visible) {
                            lv_obj_clear_flag(ui_SecValueLabel, LV_OBJ_FLAG_HIDDEN);
                        } else {
                            lv_obj_add_flag(ui_SecValueLabel, LV_OBJ_FLAG_HIDDEN);
                        }
                        lv_obj_invalidate(ui_SecValueLabel);
                    }
                    if (ui_SecondsLabel) {
                        if (visible) {
                            lv_obj_clear_flag(ui_SecondsLabel, LV_OBJ_FLAG_HIDDEN);
                        } else {
                            lv_obj_add_flag(ui_SecondsLabel, LV_OBJ_FLAG_HIDDEN);
                        }
                        lv_obj_invalidate(ui_SecondsLabel);
                    }
                }
            }
            break;
            
        case BREWING_STATE_IDLE:
        default:
            // Nothing to do in idle state
            break;
    }
}

/**
 * Update elapsed time display (format: seconds.tenths, e.g., 1.1, 1.2, 1.3)
 * NOTE: This function is called from the LVGL timer callback, which runs within
 * the LVGL task context where the mutex is already held. Do NOT take the mutex here.
 * Optimized: Only updates text if value changed to reduce unnecessary redraws.
 */
static void update_elapsed_time_display(void) {
    if (g_brewing_start_time <= 0) {
        return;
    }
    
    int64_t now_ms = brewing_display_get_current_time_ms();
    int64_t elapsed_ms = now_ms - g_brewing_start_time;
    
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }
    
    // Calculate seconds and tenths of seconds
    int total_seconds = (int)(elapsed_ms / 1000);
    int tenths = (int)((elapsed_ms % 1000) / 100);  // Tenths (0-9)
    
    // Format as "SS.M" (e.g., "1.1", "1.2", "1.3" for 1.1, 1.2, 1.3 seconds)
    static char last_time_str[16] = "";
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%d.%d", total_seconds, tenths);
    
    // Only update if value changed (optimization - reduces unnecessary redraws)
    if (strcmp(time_str, last_time_str) != 0) {
        strncpy(last_time_str, time_str, sizeof(last_time_str) - 1);
        
        // Timer callback runs within LVGL task context (mutex already held)
        if (ui_SecValueLabel) {
            lv_label_set_text(ui_SecValueLabel, time_str);
            lv_obj_invalidate(ui_SecValueLabel);
        }
    }
}

/**
 * Show brewing UI elements and hide normal UI elements
 * CRITICAL: Must hide ALL normal UI elements to prevent overlap
 */
static void show_brewing_ui(void) {
    TAKE_MUTEX() {
        brewing_debugln("[Brewing] Showing brewing UI, hiding ALL normal UI elements");
        
        // FIRST: Hide ALL normal UI elements to prevent overlap
        // Hide buttons
        if (ui_powerButton) {
            lv_obj_add_flag(ui_powerButton, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_powerButton);
        }
        if (ui_steamButton) {
            lv_obj_add_flag(ui_steamButton, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_steamButton);
        }
        
        // Hide arcs
        if (ui_Arc2) {
            lv_obj_add_flag(ui_Arc2, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_Arc2);
        }
        if (ui_Arc3) {
            lv_obj_add_flag(ui_Arc3, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_Arc3);
        }
        
        // Hide labels
        if (ui_CoffeeLabel) {
            lv_obj_add_flag(ui_CoffeeLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_CoffeeLabel);
        }
        if (ui_SteamLabel) {
            lv_obj_add_flag(ui_SteamLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_SteamLabel);
        }
        
        // Hide images
        if (ui_CoffeeImage) {
            lv_obj_add_flag(ui_CoffeeImage, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_CoffeeImage);
        }
        if (ui_SteamImage) {
            lv_obj_add_flag(ui_SteamImage, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_SteamImage);
        }
        
        // Hide temperature labels
        if (ui_CoffeeTempLabel) {
            lv_obj_add_flag(ui_CoffeeTempLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_CoffeeTempLabel);
        }
        if (ui_BoilerTempLabel) {
            lv_obj_add_flag(ui_BoilerTempLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_BoilerTempLabel);
        }
        
        // THEN: Show brewing elements
        if (ui_SecPanel) {
            lv_obj_clear_flag(ui_SecPanel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_SecPanel);
        }
        if (ui_SecValueLabel) {
            lv_obj_clear_flag(ui_SecValueLabel, LV_OBJ_FLAG_HIDDEN);
            // Set initial text to 0.0
            lv_label_set_text(ui_SecValueLabel, "0.0");
            lv_obj_invalidate(ui_SecValueLabel);
        }
        if (ui_SecondsLabel) {
            lv_obj_clear_flag(ui_SecondsLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui_SecondsLabel);
        }
        
        // Force screen refresh to ensure all changes are visible immediately
        if (ui_mainScreen) {
            lv_obj_invalidate(ui_mainScreen);
        }
        
        brewing_debugln("[Brewing] All normal UI elements hidden, brewing UI shown");
        GIVE_MUTEX();
    }
}

/**
 * Hide brewing UI elements
 */
static void hide_brewing_ui(void) {
    TAKE_MUTEX() {
        if (ui_SecPanel) {
            lv_obj_add_flag(ui_SecPanel, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_SecValueLabel) {
            lv_obj_add_flag(ui_SecValueLabel, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_SecondsLabel) {
            lv_obj_add_flag(ui_SecondsLabel, LV_OBJ_FLAG_HIDDEN);
        }
        GIVE_MUTEX();
    }
}

/**
 * Restore normal UI elements after brewing completes (without mutex)
 * This version is for use in timer callback where mutex is already held
 */
static void restore_normal_ui_no_mutex(void) {
    brewing_debugln("[Brewing] Restoring normal UI (no mutex) - hiding brewing elements, showing all normal elements");
    
    // Hide all brewing elements first
    if (ui_SecPanel) {
        lv_obj_add_flag(ui_SecPanel, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_SecValueLabel) {
        lv_obj_add_flag(ui_SecValueLabel, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_SecondsLabel) {
        lv_obj_add_flag(ui_SecondsLabel, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Show buttons (always visible in normal mode)
    if (ui_powerButton) {
        lv_obj_clear_flag(ui_powerButton, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_steamButton) {
        lv_obj_clear_flag(ui_steamButton, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Check water alarm state
    bool water_alarm_active = water_alarm_is_active();
    
    if (!water_alarm_active) {
        // Show all boiler-related elements if water alarm is not active
        // Arcs (must be shown so boiler_display can update them)
        if (ui_Arc2) {
            lv_obj_clear_flag(ui_Arc2, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_Arc3) {
            lv_obj_clear_flag(ui_Arc3, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Coffee and steam labels
        if (ui_CoffeeLabel) {
            lv_obj_clear_flag(ui_CoffeeLabel, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_SteamLabel) {
            lv_obj_clear_flag(ui_SteamLabel, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Coffee and steam images/icons
        if (ui_CoffeeImage) {
            lv_obj_clear_flag(ui_CoffeeImage, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_SteamImage) {
            lv_obj_clear_flag(ui_SteamImage, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Temperature labels
        if (ui_CoffeeTempLabel) {
            lv_obj_clear_flag(ui_CoffeeTempLabel, LV_OBJ_FLAG_HIDDEN);
        }
        if (ui_BoilerTempLabel) {
            lv_obj_clear_flag(ui_BoilerTempLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // If water alarm is active, water_alarm system will manage visibility
    
    // Invalidate screen to ensure everything is redrawn immediately
    if (ui_mainScreen) {
        lv_obj_invalidate(ui_mainScreen);
    }
    
    brewing_debugln("[Brewing] Normal UI restored - all brewing elements hidden, all normal elements shown");
}

/**
 * Restore normal UI elements after brewing completes
 * This restores all UI elements to their normal state, including arcs and labels
 */
static void restore_normal_ui(void) {
    TAKE_MUTEX() {
        restore_normal_ui_no_mutex();
        GIVE_MUTEX();
    }
    
    // Note: boiler_display_update() will be called from websocket handler
    // with the current machine state, which will update arcs and labels with correct values
}

/**
 * Get current Unix timestamp in milliseconds (GMT/UTC)
 */
int64_t brewing_display_get_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

/**
 * Check if brewing mode is currently active
 * Returns true for BOTH active brewing and flashing states
 * This ensures normal UI stays hidden during the entire brewing sequence
 */
bool brewing_display_is_active(void) {
    return g_initialized && (g_state == BREWING_STATE_ACTIVE || g_state == BREWING_STATE_FLASHING);
}

/**
 * Check GPIO pin for brewing simulation mode
 * Should be called regularly from main loop
 * GPIO LOW (GND) = enter brewing mode
 * GPIO HIGH = exit brewing mode
 */
void brewing_display_check_gpio_simulation(void) {
    if (!g_initialized) {
        return;
    }
    
    // Read GPIO state (LOW = 0 = GND, HIGH = 1 = pulled up)
    bool current_gpio_state = digitalRead(BREWING_SIM_PIN) == HIGH;
    
    // Check if state changed
    if (current_gpio_state != g_last_gpio_state) {
        g_last_gpio_state = current_gpio_state;
        
        if (!current_gpio_state) {
            // GPIO is LOW (GND) - enter brewing mode
            if (!g_gpio_simulation_active) {
                g_gpio_simulation_active = true;
                brewing_debugln("[Brewing] ========================================");
                brewing_debugln("[Brewing] GPIO 15 LOW - Entering BREWING SIMULATION mode");
                brewing_debugln("[Brewing] ========================================");
                
                // Start brewing with current time as start time
                g_allow_update_from_gpio = true;
                int64_t current_time = brewing_display_get_current_time_ms();
                brewing_display_update(true, current_time);
            }
        } else {
            // GPIO is HIGH - exit brewing mode
            if (g_gpio_simulation_active) {
                g_gpio_simulation_active = false;
                brewing_debugln("[Brewing] ========================================");
                brewing_debugln("[Brewing] GPIO 15 HIGH - Exiting BREWING SIMULATION mode");
                brewing_debugln("[Brewing] ========================================");
                
                // If we're in flashing state, skip it and go directly to idle
                if (g_state == BREWING_STATE_FLASHING) {
                    brewing_debugln("[Brewing] Skipping flash - GPIO released during flash");
                    g_state = BREWING_STATE_IDLE;
                    g_final_seconds = 0;
                    
                    // Pause timer first
                    if (g_update_timer && !g_timer_paused) {
                        lv_timer_pause(g_update_timer);
                        g_timer_paused = true;
                    }
                    
                    // Restore normal UI
                    restore_normal_ui();
                } else if (g_state == BREWING_STATE_ACTIVE) {
                    // Stop brewing (will enter flashing state, then restore after 3 seconds)
                    g_allow_update_from_gpio = true;
                    brewing_display_update(false, 0);
                    // Timer will continue running to handle flashing state
                } else {
                    // Already idle, just ensure UI is restored
                    if (g_state != BREWING_STATE_IDLE) {
                        g_state = BREWING_STATE_IDLE;
                        restore_normal_ui();
                    }
                }
            }
        }
    }
    
    // If GPIO simulation is active, ensure brewing mode stays active
    // But allow flashing state to complete if GPIO was released
    if (g_gpio_simulation_active && g_state != BREWING_STATE_ACTIVE && g_state != BREWING_STATE_FLASHING) {
        // Re-enter brewing mode if somehow we exited it (but not if we're flashing)
        g_allow_update_from_gpio = true;
        int64_t current_time = brewing_display_get_current_time_ms();
        brewing_display_update(true, current_time);
    }
    
    // Note: We don't check if GPIO simulation is inactive while in ACTIVE state here
    // because that's handled when GPIO state changes (above). This prevents conflicts
    // with websocket-triggered brewing. Websocket will handle stopping brewing when
    // it receives a non-brewing status.
}
