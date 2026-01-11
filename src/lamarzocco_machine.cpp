#include "lamarzocco_machine.h"
#include "config.h"
#include "boiler_display.h"
#include "water_alarm.h"
#include "brewing_display.h"
#include "activity_monitor.h"
#include "update_screen.h"
#include <ArduinoJson.h>

LaMarzoccoMachine* LaMarzoccoMachine::_instance = nullptr;

LaMarzoccoMachine::LaMarzoccoMachine(LaMarzoccoClient& client, LaMarzoccoWebSocket& websocket)
    : _client(client), _websocket(websocket), _power_state(false), _steam_state(false) {
    _instance = this;
    _websocket.set_message_callback(_websocket_message_handler);
}

void LaMarzoccoMachine::_websocket_message_handler(const String& message) {
    if (_instance) {
        // Parse JSON message with large buffer for La Marzocco messages (can be 2-3KB)
        JsonDocument doc;
        
        Serial.println("\n========== WEBSOCKET MESSAGE RECEIVED ==========");
        Serial.print("Message length: ");
        Serial.print(message.length());
        Serial.println(" bytes");
        
        DeserializationError error = deserializeJson(doc, message);
        
        if (error) {
            Serial.print("❌ JSON parse error: ");
            Serial.println(error.c_str());
            Serial.print("Error code: ");
            Serial.println((int)error.code());
            
            if (error == DeserializationError::NoMemory) {
                Serial.println("💡 JSON document is too large - increase buffer size");
            }
            return;
        }
        
        Serial.println("✓ JSON parsed successfully");
        
        // Variables to store extracted data
        const char* machine_status = nullptr;
        const char* machine_mode = nullptr;
        const char* coffee_boiler_status = nullptr;
        int64_t coffee_ready_time = 0;
        float coffee_target_temp = 0.0;
        const char* steam_boiler_status = nullptr;
        int64_t steam_ready_time = 0;
        const char* steam_target_level = nullptr;
        bool no_water_alarm = false;
        bool is_brewing = false;
        int64_t brewing_start_time = 0;
        
        // Parse widgets array to extract boiler and machine status
        if (doc.containsKey("widgets")) {
            JsonArray widgets = doc["widgets"].as<JsonArray>();
            Serial.print("Widgets count: ");
            Serial.println(widgets.size());
            
            for (JsonVariant widget : widgets) {
                const char* code = widget["code"];
                if (!code) continue;
                
                // Extract machine status
                if (strcmp(code, "CMMachineStatus") == 0) {
                    Serial.println("✓ Found CMMachineStatus widget");
                    JsonObject output = widget["output"].as<JsonObject>();
                    
                    machine_status = output["status"];
                    machine_mode = output["mode"];
                    
                    // Check if brewing
                    if (machine_status && strcmp(machine_status, "Brewing") == 0) {
                        is_brewing = true;
                        // Extract brewingStartTime
                        if (output.containsKey("brewingStartTime") && !output["brewingStartTime"].isNull()) {
                            brewing_start_time = output["brewingStartTime"].as<long long>();
                            Serial.print("☕ Brewing started at: ");
                            Serial.println((long long)brewing_start_time);
                        }
                    } else {
                        is_brewing = false;
                        brewing_start_time = 0;
                    }
                    
                    if (machine_status) {
                        _instance->_power_state = (strcmp(machine_status, "PoweredOn") == 0);
                        Serial.print("📊 Machine status: ");
                        Serial.print(machine_status);
                        if (machine_mode) {
                            Serial.print(" (mode: ");
                            Serial.print(machine_mode);
                            Serial.print(")");
                        }
                        Serial.println();
                    }
                }
                // Extract coffee boiler status and ready time
                else if (strcmp(code, "CMCoffeeBoiler") == 0) {
                    Serial.println("☕ Found CMCoffeeBoiler widget");
                    JsonObject output = widget["output"].as<JsonObject>();
                    
                    coffee_boiler_status = output["status"];
                    if (output.containsKey("readyStartTime") && !output["readyStartTime"].isNull()) {
                        coffee_ready_time = output["readyStartTime"].as<long long>();
                    }
                    if (output.containsKey("targetTemperature")) {
                        coffee_target_temp = output["targetTemperature"].as<float>();
                    }
                    
                    Serial.print("  Status: ");
                    Serial.print(coffee_boiler_status ? coffee_boiler_status : "null");
                    Serial.print(", TargetTemp: ");
                    Serial.print(coffee_target_temp);
                    Serial.print("°C, ReadyStartTime: ");
                    Serial.println((long long)coffee_ready_time);
                }
                // Extract steam boiler status and ready time
                else if (strcmp(code, "CMSteamBoilerLevel") == 0) {
                    Serial.println("♨️  Found CMSteamBoilerLevel widget");
                    JsonObject output = widget["output"].as<JsonObject>();
                    
                    steam_boiler_status = output["status"];
                    if (output.containsKey("readyStartTime") && !output["readyStartTime"].isNull()) {
                        steam_ready_time = output["readyStartTime"].as<long long>();
                    }
                    if (output.containsKey("targetLevel")) {
                        steam_target_level = output["targetLevel"];
                    }
                    
                    // Update internal steam state based on status
                    if (steam_boiler_status) {
                        if (strcmp(steam_boiler_status, "Off") != 0 && strcmp(steam_boiler_status, "StandBy") != 0) {
                            _instance->_steam_state = true;
                        } else {
                            _instance->_steam_state = false;
                        }
                    }
                    
                    Serial.print("  Status: ");
                    Serial.print(steam_boiler_status ? steam_boiler_status : "null");
                    Serial.print(", TargetLevel: ");
                    Serial.print(steam_target_level ? steam_target_level : "null");
                    Serial.print(", ReadyStartTime: ");
                    Serial.println((long long)steam_ready_time);
                }
                // Check for NoWater alarm
                else if (strcmp(code, "CMNoWater") == 0) {
                    Serial.println("💧 Found CMNoWater widget");
                    JsonObject output = widget["output"].as<JsonObject>();
                    
                    if (output.containsKey("allarm")) {
                        no_water_alarm = output["allarm"].as<bool>();
                        Serial.print("  NoWater alarm: ");
                        Serial.println(no_water_alarm ? "TRUE ⚠️" : "false");
                    }
                }
            }
        }

        static bool last_brewing_state = false;
        static bool last_brewing_state_valid = false;
        if (machine_status) {
            if (!last_brewing_state_valid || is_brewing != last_brewing_state) {
                activity_monitor_mark_machine_activity();
                bool was_brewing = last_brewing_state_valid && last_brewing_state;
                last_brewing_state = is_brewing;
                last_brewing_state_valid = true;
                if (_instance && was_brewing && !is_brewing) {
                    _instance->request_stats_refresh();
                }
            }
        }
        
        // Check if any boiler reports NoWater status
        if (coffee_boiler_status && strcmp(coffee_boiler_status, "NoWater") == 0) {
            Serial.println("⚠️  Coffee boiler reports NoWater!");
            no_water_alarm = true;
        }
        if (steam_boiler_status && strcmp(steam_boiler_status, "NoWater") == 0) {
            Serial.println("⚠️  Steam boiler reports NoWater!");
            no_water_alarm = true;
        }
        
        // Update water alarm state
        water_alarm_set(no_water_alarm);
        
        // Update brewing display
        brewing_display_update(is_brewing, brewing_start_time);
        
        // Update boiler displays if we have machine status
        // Boiler displays (labels) continue to update even during water alarm
        // Only the arcs are hidden by water_alarm system
        if (machine_status) {
            Serial.println("\n🔄 Updating boiler displays...");
            
            // Format temperature and level strings
            char coffee_temp_str[16] = "";
            char steam_level_str[16] = "";
            
            if (coffee_target_temp > 0) {
                snprintf(coffee_temp_str, sizeof(coffee_temp_str), "%.0f°C", coffee_target_temp);
            }
            
            if (steam_target_level) {
                // Convert "Level2" to "L2", "Level1" to "L1", etc.
                if (strncmp(steam_target_level, "Level", 5) == 0) {
                    snprintf(steam_level_str, sizeof(steam_level_str), "L%s", steam_target_level + 5);
                } else {
                    strncpy(steam_level_str, steam_target_level, sizeof(steam_level_str) - 1);
                }
            }
            
            // If machine is OFF or StandBy, use that for both boilers
            if (strcmp(machine_status, "Off") == 0 || strcmp(machine_status, "StandBy") == 0) {
                boiler_display_update(BOILER_COFFEE, machine_status, 
                                     coffee_boiler_status ? coffee_boiler_status : "Off", 
                                     coffee_ready_time,
                                     coffee_temp_str[0] ? coffee_temp_str : nullptr);
                boiler_display_update(BOILER_STEAM, machine_status, 
                                     steam_boiler_status ? steam_boiler_status : "Off", 
                                     steam_ready_time,
                                     steam_level_str[0] ? steam_level_str : nullptr);
            } else {
                // Machine is ON, update each boiler independently
                if (coffee_boiler_status) {
                    boiler_display_update(BOILER_COFFEE, machine_status, 
                                         coffee_boiler_status, coffee_ready_time,
                                         coffee_temp_str[0] ? coffee_temp_str : nullptr);
                }
                
                if (steam_boiler_status) {
                    boiler_display_update(BOILER_STEAM, machine_status, 
                                         steam_boiler_status, steam_ready_time,
                                         steam_level_str[0] ? steam_level_str : nullptr);
                }
            }
        } else {
            Serial.println("⚠ No machine status found, skipping boiler updates");
        }
        
        // Check for command responses
        if (doc.containsKey("commands")) {
            JsonArray commands = doc["commands"].as<JsonArray>();
            if (commands.size() > 0) {
                Serial.println("\n📋 Command responses:");
                for (JsonVariant cmd : commands) {
                    const char* id = cmd["id"];
                    const char* status = cmd["status"];
                    if (id && status) {
                        Serial.print("  ✓ Command ");
                        Serial.print(id);
                        Serial.print(": ");
                        Serial.println(status);
                    }
                }
            }
        }
        
        Serial.println("===============================================\n");
    }
}

bool LaMarzoccoMachine::set_power(bool enabled) {
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        debugln("Serial number not set");
        return false;
    }
    
    JsonDocument request;
    request["mode"] = enabled ? "BrewingMode" : "StandBy";
    
    JsonDocument response;
    bool success = _client.api_call("POST", 
                                     "/things/" + serial + "/command/CoffeeMachineChangeMode",
                                     &request, &response);
    
    if (success) {
        _power_state = enabled;
        debug("Power set to: ");
        debugln(enabled ? "ON" : "OFF");
    } else {
        debugln("Failed to set power");
    }
    
    return success;
}

bool LaMarzoccoMachine::toggle_power() {
    return set_power(!_power_state);
}

bool LaMarzoccoMachine::set_steam(bool enabled) {
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        debugln("Serial number not set");
        return false;
    }
    
    JsonDocument request;
    request["boilerIndex"] = 1;  // Steam boiler index
    request["enabled"] = enabled;
    
    JsonDocument response;
    bool success = _client.api_call("POST", 
                                     "/things/" + serial + "/command/CoffeeMachineSettingSteamBoilerEnabled",
                                     &request, &response);
    
    if (success) {
        _steam_state = enabled;
        debug("Steam boiler set to: ");
        debugln(enabled ? "ON" : "OFF");
    } else {
        debugln("Failed to set steam boiler");
    }
    
    return success;
}

bool LaMarzoccoMachine::toggle_steam() {
    Serial.println("===========================================");
    Serial.println("STEAM BUTTON PRESSED - Toggling Steam Boiler");
    Serial.print("Current steam state: ");
    Serial.println(_steam_state ? "ON" : "OFF");
    Serial.print("Target steam state: ");
    Serial.println(_steam_state ? "OFF" : "ON");
    Serial.println("===========================================");
    
    return set_steam(!_steam_state);
}

bool LaMarzoccoMachine::connect_websocket() {
    if (is_websocket_connected()) {
        return true;  // Already connected
    }
    
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        debugln("Serial number not set");
        return false;
    }
    
    return _websocket.connect(serial);
}

bool LaMarzoccoMachine::is_websocket_connected() const {
    return _websocket.is_connected();
}

void LaMarzoccoMachine::disconnect_websocket() {
    _websocket.disconnect();
}

void LaMarzoccoMachine::loop() {
    // Call websocket loop regularly - this is critical for connection
    _websocket.loop();

    if (_stats_refresh_pending) {
        unsigned long now = millis();
        static const unsigned long STATS_REFRESH_MIN_INTERVAL_MS = 5000;
        if (now - _last_stats_refresh_ms >= STATS_REFRESH_MIN_INTERVAL_MS) {
            _stats_refresh_pending = false;
            _last_stats_refresh_ms = now;
            _refresh_shot_counters();
        }
    }

    // Auto-reconnect logic: If disconnected, try to reconnect periodically
    // This ensures we get a fresh access token instead of reusing an expired one
    static unsigned long last_reconnect_attempt = 0;
    const unsigned long RECONNECT_INTERVAL_MS = 30000;  // Try reconnect every 30 seconds

    if (!is_websocket_connected()) {
        unsigned long now = millis();
        if (now - last_reconnect_attempt >= RECONNECT_INTERVAL_MS) {
            last_reconnect_attempt = now;
            Serial.println("[AUTO-RECONNECT] WebSocket disconnected, attempting reconnect with fresh token...");
            connect_websocket();  // This will refresh the access token and reconnect
        }
    }
}

void LaMarzoccoMachine::request_stats_refresh() {
    _stats_refresh_pending = true;
}

void LaMarzoccoMachine::_refresh_shot_counters() {
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        return;
    }

    JsonDocument response;
    String endpoint = "/things/" + serial + "/stats/COFFEE_AND_FLUSH_COUNTER/1";
    if (!_client.api_call("GET", endpoint, nullptr, &response)) {
        debugln("Failed to fetch coffee/flush counters");
        return;
    }

    JsonVariant output = response["output"];
    if (output.isNull()) {
        debugln("Coffee/flush counter response missing output");
        return;
    }

    uint32_t total_coffee = output["totalCoffee"] | 0;
    uint32_t total_flush = output["totalFlush"] | 0;
    updateShotCounters(total_coffee, total_flush);
}
