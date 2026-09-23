#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "lamarzocco_auth.h"
#include "Preferences.h"

struct AccessToken {
    String access_token;
    String refresh_token;
    unsigned long expires_at;  // Unix timestamp in seconds
    
    bool isValid() const {
        return access_token.length() > 0 && expires_at > (unsigned long)(millis() / 1000);
    }
};

class LaMarzoccoClient {
public:
    LaMarzoccoClient(Preferences& prefs);
    ~LaMarzoccoClient();
    
    // Initialize client with credentials
    bool init(const String& username, const String& password, const String& serial_number);
    
    // Register client (call after generating installation key)
    bool register_client();
    
    // Get access token (sign in or refresh)
    bool get_access_token();

    // HTTP status of the last sign in attempt. A 4xx is the server saying it
    // rejected the credentials; anything else means the answer never arrived.
    int get_last_auth_status() const { return _last_auth_status; }
    
    // Make authenticated API call
    bool api_call(const String& method, const String& endpoint, JsonDocument* request_body, JsonDocument* response_body);
    
    // Get installation key
    bool get_installation_key(InstallationKey& key) const;
    
    // Check if initialized
    bool is_initialized() const { return _initialized; }
    
    // Get serial number
    String get_serial_number() const { return _serial_number; }
    
    // Get access token string (for websocket)
    String get_access_token_string() const { return _access_token.access_token; }
    
private:
    int _last_auth_status = 0;
    Preferences& _prefs;
    InstallationKey _installation_key;
    AccessToken _access_token;
    String _username;
    String _password;
    String _serial_number;
    bool _initialized;
    WiFiClientSecure _client;
    
    // Internal helpers
    bool _sign_in();
    bool _refresh_token();
    bool _make_request(const String& method, const String& url, JsonDocument* request_body, JsonDocument* response_body, bool needs_auth);
    void _add_auth_headers(HTTPClient& http);
};

