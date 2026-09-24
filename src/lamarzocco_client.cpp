#include "lamarzocco_client.h"
#include "config.h"
#include <time.h>

static const char* BASE_URL = "lion.lamarzocco.io";
static const char* CUSTOMER_APP_URL = "https://lion.lamarzocco.io/api/customer-app";
static const unsigned long TOKEN_TIME_TO_REFRESH = 10 * 60;  // 10 minutes

LaMarzoccoClient::LaMarzoccoClient(Preferences& prefs) 
    : _prefs(prefs), _initialized(false) {
    _client.setInsecure();  // For now, accept self-signed certs
}

LaMarzoccoClient::~LaMarzoccoClient() {
}

bool LaMarzoccoClient::init(const String& username, const String& password, const String& serial_number) {
    _username = username;
    _password = password;
    _serial_number = serial_number;
    
    // Load installation key
    if (!LaMarzoccoAuth::load_installation_key(_prefs, _installation_key)) {
        debugln("No installation key found, need to generate one");
        return false;
    }
    
    _initialized = true;
    return true;
}

bool LaMarzoccoClient::get_installation_key(InstallationKey& key) const {
    if (!_initialized) {
        return false;
    }
    key = _installation_key;
    return true;
}

bool LaMarzoccoClient::register_client() {
    if (!_initialized) {
        debugln("Client not initialized");
        return false;
    }
    
    String base_string = LaMarzoccoAuth::generate_base_string(_installation_key);
    String proof = LaMarzoccoAuth::generate_request_proof(base_string, _installation_key.secret);
    String public_key_b64 = LaMarzoccoAuth::base64_encode(_installation_key.public_key_der, _installation_key.public_key_len);
    
    HTTPClient http;
    http.begin(_client, String(CUSTOMER_APP_URL) + "/auth/init");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-App-Installation-Id", _installation_key.installation_id);
    http.addHeader("X-Request-Proof", proof);
    
    JsonDocument request;
    request["pk"] = public_key_b64;
    
    String request_body;
    serializeJson(request, request_body);
    
    int http_code = http.POST(request_body);
    String response = http.getString();
    http.end();
    
    if (http_code == 200 || http_code == 201) {
        debugln("Registration successful");
        return true;
    } else {
        debug("Registration failed: ");
        debugln(http_code);
        debugln(response);
        return false;
    }
}

bool LaMarzoccoClient::_sign_in() {
    JsonDocument request;
    request["username"] = _username;
    request["password"] = _password;
    
    String request_body;
    serializeJson(request, request_body);
    
    HTTPClient http;
    http.begin(_client, String(CUSTOMER_APP_URL) + "/auth/signin");
    http.addHeader("Content-Type", "application/json");
    _add_auth_headers(http);
    
    int http_code = http.POST(request_body);
    String response = http.getString();
    http.end();
    _last_auth_status = http_code;
    
    if (http_code == 200) {
        JsonDocument response_doc;
        deserializeJson(response_doc, response);
        
        _access_token.access_token = response_doc["accessToken"].as<String>();
        _access_token.refresh_token = response_doc["refreshToken"].as<String>();
        
        // Parse expires_at (assuming it's in the response, adjust as needed)
        unsigned long expires_in = response_doc["expiresIn"].as<unsigned long>();
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            _access_token.expires_at = mktime(&timeinfo) + expires_in;
        } else {
            _access_token.expires_at = (millis() / 1000) + expires_in;
        }
        
        debugln("Sign in successful");
        return true;
    } else {
        debug("Sign in failed: ");
        debugln(http_code);
        debugln(response);
        return false;
    }
}

bool LaMarzoccoClient::_refresh_token() {
    if (_access_token.refresh_token.length() == 0) {
        return _sign_in();
    }
    
    JsonDocument request;
    request["username"] = _username;
    request["refreshToken"] = _access_token.refresh_token;
    
    String request_body;
    serializeJson(request, request_body);
    
    HTTPClient http;
    http.begin(_client, String(CUSTOMER_APP_URL) + "/auth/refreshtoken");
    http.addHeader("Content-Type", "application/json");
    _add_auth_headers(http);
    
    int http_code = http.POST(request_body);
    String response = http.getString();
    http.end();
    _last_auth_status = http_code;
    
    if (http_code == 200) {
        JsonDocument response_doc;
        deserializeJson(response_doc, response);
        
        _access_token.access_token = response_doc["accessToken"].as<String>();
        if (response_doc.containsKey("refreshToken")) {
            _access_token.refresh_token = response_doc["refreshToken"].as<String>();
        }
        
        unsigned long expires_in = response_doc["expiresIn"].as<unsigned long>();
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            _access_token.expires_at = mktime(&timeinfo) + expires_in;
        } else {
            _access_token.expires_at = (millis() / 1000) + expires_in;
        }
        
        debugln("Token refresh successful");
        return true;
    } else {
        debug("Token refresh failed: ");
        debugln(http_code);
        return _sign_in();  // Fallback to sign in
    }
}

bool LaMarzoccoClient::get_access_token() {
    if (!_initialized) {
        return false;
    }
    
    struct tm timeinfo;
    unsigned long now = 0;
    if (getLocalTime(&timeinfo)) {
        now = mktime(&timeinfo);
    } else {
        now = millis() / 1000;
    }
    
    if (!_access_token.isValid() || _access_token.expires_at < now + TOKEN_TIME_TO_REFRESH) {
        if (_access_token.refresh_token.length() > 0 && _access_token.expires_at > now) {
            return _refresh_token();
        } else {
            return _sign_in();
        }
    }
    
    return true;
}

void LaMarzoccoClient::_add_auth_headers(HTTPClient& http) {
    String installation_id, timestamp, nonce, signature;
    LaMarzoccoAuth::generate_extra_request_headers(_installation_key, installation_id, timestamp, nonce, signature);
    
    http.addHeader("X-App-Installation-Id", installation_id);
    http.addHeader("X-Timestamp", timestamp);
    http.addHeader("X-Nonce", nonce);
    http.addHeader("X-Request-Signature", signature);
}

bool LaMarzoccoClient::api_call(const String& method, const String& endpoint, JsonDocument* request_body, JsonDocument* response_body) {
    if (!get_access_token()) {
        return false;
    }
    
    String url = String(CUSTOMER_APP_URL) + endpoint;
    
    HTTPClient http;
    http.begin(_client, url);
    http.addHeader("Content-Type", "application/json");
    _add_auth_headers(http);
    http.addHeader("Authorization", "Bearer " + _access_token.access_token);
    
    String request_str;
    if (request_body) {
        serializeJson(*request_body, request_str);
    }
    
    int http_code = 0;
    if (method == "GET") {
        http_code = http.GET();
    } else if (method == "POST") {
        http_code = http.POST(request_str);
    } else if (method == "PUT") {
        http_code = http.PUT(request_str);
    } else if (method == "DELETE") {
        http_code = http.sendRequest("DELETE", request_str);
    } else {
        http.end();
        return false;
    }
    
    String response_str = http.getString();
    http.end();
    
    if (http_code >= 200 && http_code < 300) {
        if (response_body && response_str.length() > 0) {
            deserializeJson(*response_body, response_str);
        }
        return true;
    } else {
        debug("API call failed: ");
        debugln(http_code);
        debugln(response_str);
        return false;
    }
}

