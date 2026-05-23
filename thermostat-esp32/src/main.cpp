#include <Arduino.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <DNSServer.h>
#include <Preferences.h>
#define DEBUG 0
unsigned long lastSampleTime = 0;
const unsigned long SAMPLE_INTERVAL_MS = 5000; // Read sensor every 5 seconds
const int BOOT_BUTTON_PIN = 0; // Physical BOOT button on the ESP32
const int LED_PIN = 2;
DNSServer dnsServer;
Preferences preferences;
// ===== Button Timing =====
unsigned long buttonPressStartTime = 0;
bool isButtonPressed = false;
// ===== History buffer (5 minutes @ 1 Hz) =====
static const size_t HISTORY_MAX = 300;
int DESIRED_TMP_CELSIUS = 25;
String RELAY_ON_URL = "http://relay.local/turn-heater-on";
String RELAY_OFF_URL = "http://relay.local/turn-heater-off";
 
struct Sample {
  uint32_t t_ms;
  float tC;
  float hPct;
};
 
Sample hist[HISTORY_MAX];
size_t histHead = 0;
size_t histCount = 0;
// ===== PINS =====
const uint8_t DHT_PIN = 4;
// ===== DHT22 object =====
#define DHTTYPE DHT11   // Define the exact sensor type
DHT dht(DHT_PIN, DHTTYPE); // Initialize the Adafruit DHT object
// ===== WiFi =====
const char* MDNS_NAME = "thermostat";  
// ===== Web =====
WebServer server(80);
// ===== HTTP handlers =====
void startCaptivePortal() {
  Serial.println("\n[Captive Portal] Starting...");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Thermostat_Setup"); 
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, []() {
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<style>body{font-family:Arial; padding:20px; text-align:center;} input{margin:10px 0; padding:10px; width:100%; box-sizing:border-box;} button{padding:10px 20px; background:#007BFF; color:white; border:none; border-radius:5px;}</style></head>"
                  "<body><h2>Thermostat Setup</h2>"
                  "<form action='/save' method='POST'>"
                  "<input type='text' name='ssid' placeholder='WiFi Network Name (SSID)' required><br>"
                  "<input type='password' name='pass' placeholder='WiFi Password' required><br>"
                  "<button type='submit'>Save & Connect</button></form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");
    
    preferences.begin("wifi-creds", false);
    preferences.putString("ssid", newSsid);
    preferences.putString("password", newPass);
    preferences.end();
    
    server.send(200, "text/html", "<html><body><h2>Saved!</h2><p>Rebooting thermostat...</p></body></html>");
    delay(1500);
    ESP.restart();
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.print("[Captive Portal] Active at IP: ");
  Serial.println(WiFi.softAPIP());

  // Trap the microcontroller here until setup is complete
  unsigned long lastBlinkTime = 0;
  bool ledState = false;

  while(true) {
    // 1. Handle Web Traffic
    dnsServer.processNextRequest();
    server.handleClient();
    
    // 2. Blink the LED every 500ms
    if (millis() - lastBlinkTime > 500) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      lastBlinkTime = millis();
    }
    
    delay(10);
  }
}
void handleRoot() {
  File f = SPIFFS.open("/index.html", "r");
  if (!f) { server.send(500, "text/plain", "index.html missing"); return; }
  server.streamFile(f, "text/html");
  f.close();
}
void pushSample(const Sample& s) {
  hist[histHead] = s;
  histHead = (histHead + 1) % HISTORY_MAX;
  if (histCount < HISTORY_MAX) histCount++;
}
void handleTmpChange(int tmp) {
  HTTPClient http;
  http.setTimeout(1000);
  if (tmp < DESIRED_TMP_CELSIUS) {
    http.begin(RELAY_ON_URL);
  } else {
    http.begin(RELAY_OFF_URL);
  }
  http.GET();
  http.end();
}
void getCurrentDesiredTmp() {
  server.send(200, "text/plain", String(DESIRED_TMP_CELSIUS));
}
void takeAndStoreSample() {
  Sample s;
  s.t_ms = millis();
  Serial.println("[DHT22] Reading sensor...");
  s.tC   = dht.readTemperature();
  s.hPct = dht.readHumidity();
  Serial.println("Temp: " + String(s.tC) + "°C");
  Serial.println("Humidity: " + String(s.hPct) + "%");
  handleTmpChange(s.tC);
  pushSample(s);
}
 
Sample getSampleByAge(size_t iOldToNew) {
  size_t idx = (histHead + HISTORY_MAX - histCount + iOldToNew) % HISTORY_MAX;
  return hist[idx];
}
void handleDesiredTempChange() {
  // Always check if the client actually provided the argument
  if (!server.hasArg("desired-tmp")) {
    server.send(400, "text/plain", "Bad Request: Missing desired-tmp parameter");
    return;
  }

  // Grab the value by its specific name
  int desiredTmp = server.arg("desired-tmp").toInt();
  
  // Update the global state
  DESIRED_TMP_CELSIUS = desiredTmp;
  
  Serial.println("Desired tmp has been set to " + String(desiredTmp) + " degrees Celsius.");
  
  // Always send a 200 OK back to the browser so it knows the request succeeded
  server.send(200, "text/plain", "Temperature updated to " + String(desiredTmp) + " degrees Celsius.");
}
void handleApiHistory() {
  takeAndStoreSample();
  uint32_t nowms = millis();
  size_t n = histCount;
  if (n == 0) { server.send(200, "application/json", "{\"ok\":true,\"n\":0}"); return; }
 
  String json;
  json.reserve(n * 64 + 128);
  json += "{\"ok\":true,\"n\":" + String(n);
 
  // seconds ago array
  json += ",\"s\":[";
  for (size_t i = 0; i < n; ++i) {
    Sample s = getSampleByAge(i);
    uint32_t secAgo = (nowms - s.t_ms) / 1000;
    json += String(secAgo);
    if (i + 1 < n) json += ",";
  }
  json += "]";
 
  auto appendSeries = [&](const char* key, float Sample::*field, int digits) {
    json += ",\""; json += key; json += "\":[";
    for (size_t i = 0; i < n; ++i) {
      Sample s = getSampleByAge(i);
      json += String(s.*field, digits);
      if (i + 1 < n) json += ",";
    }
    json += "]";
  };
 
  appendSeries("tc",  &Sample::tC,   2);
  appendSeries("rh",  &Sample::hPct, 1);
 
  json += "}";
  server.send(200, "application/json", json);
}
void handleApiStatus() {
  bool up = WiFi.status() == WL_CONNECTED;
  long rssi = up ? WiFi.RSSI() : -127;   // dBm
  uint32_t uptimeS = millis() / 1000;
 
  String json = "{";
  json += "\"ok\":true";
  json += ",\"rssi_dbm\":" + String(rssi);
  json += ",\"uptime_s\":" + String(uptimeS);
  json += ",\"hostname\":\"" + String(MDNS_NAME) + "\"";
  json += ",\"ip\":\"" + (up ? WiFi.localIP().toString() : String("")) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}
void handleApiSensor() { 
  Serial.println("[DHT22] Reading sensor...");
  
  // The Adafruit library handles the timing delays internally
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  // Check if any reads failed (isnan = is not a number)
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("DHT22 error: Failed to read from sensor!");
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"Sensor read failed\"}");
    return;
  }
  Serial.println("Temp: " + String(temperature) + "°C");
  Serial.println("Humidity: " + String(humidity) + "%");
  String json = "{";
  json += "\"ok\":" + String("true");
  json += ",\"temperature_c\":" + String(temperature);
  json += ",\"humidity_pct\":" + String(humidity);
  json += "}";
  server.send(200, "application/json", json);
  handleTmpChange(temperature);
}
// ===== MAIN SETUP =====
void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // Ensure LED is off on boot
  delay(100);

  // 2. Load Credentials
  preferences.begin("wifi-creds", true); 
  String savedSsid = preferences.getString("ssid", "");
  String savedPass = preferences.getString("password", "");
  preferences.end();

  // 3. Routing Logic / Network Connection
  if (savedSsid == "") {
    Serial.println("\n[WiFi] Memory blank. First-time setup required.");
    startCaptivePortal();
  } else {
    Serial.printf("\n[WiFi] Attempting to connect to: %s\n", savedSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_NAME);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      Serial.print(".");
      delay(500);
    }
    Serial.println();

    // FIX: The Roadblock - Trap it here if WiFi failed!
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n[WiFi] Failed to connect! Router offline or bad password.");
      startCaptivePortal(); 
    }

    // 4. Start Subsystems ONLY if WiFi connected successfully
    Serial.printf("[WiFi] Connected securely! IP: %s\n", WiFi.localIP().toString().c_str());

    if (!SPIFFS.begin(true)) {
      Serial.println("[SPIFFS] An error has occurred while mounting SPIFFS");
    } else {
      Serial.println("[SPIFFS] Mounted successfully");
    }

    dht.begin();
    Serial.println("DHT initiated");

    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[mDNS] http://%s.local/\n", MDNS_NAME);
    }

    // Routes
    server.on("/",           HTTP_GET, handleRoot);
    server.on("/api/sensor", HTTP_GET, handleApiSensor);
    server.on("/api/history",HTTP_GET, handleApiHistory);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/set-desired-tmp", HTTP_GET, handleDesiredTempChange);
    server.on("/api/get-desired-tmp", HTTP_GET, getCurrentDesiredTmp);
    server.onNotFound([]() {
      String path = server.uri();
      if (path == "/") { handleRoot(); return; }
      if (!SPIFFS.exists(path)) { server.send(404, "text/plain", "Not found"); return; }
      String ct = "text/plain";
      if      (path.endsWith(".html")) ct = "text/html";
      else if (path.endsWith(".css"))  ct = "text/css";
      else if (path.endsWith(".js"))   ct = "application/javascript";
      File f = SPIFFS.open(path, "r"); server.streamFile(f, ct); f.close();
    });

    server.begin();
    Serial.println("[HTTP] Server started");
    takeAndStoreSample();
  }
}
void loop() {
  if (DEBUG != 1) {
    server.handleClient();

    // ===== FACTORY RESET CHECK =====
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      if (!isButtonPressed) {
        isButtonPressed = true;
        buttonPressStartTime = millis(); // Start the stopwatch
      } else if (millis() - buttonPressStartTime > 1000) {
        // Button held for 3 seconds!
        Serial.println("\n[Security] Factory Reset Triggered!");
        Serial.println("Wiping WiFi credentials...");
        
        preferences.begin("wifi-creds", false);
        preferences.clear(); // Wipes everything in this namespace
        preferences.end();
        
        Serial.println("Rebooting into Captive Portal...");
        delay(1000);
        ESP.restart(); // Force reboot
      }
    } else {
      isButtonPressed = false; // Reset stopwatch if button is released early
    }

    if (millis() - lastSampleTime >= SAMPLE_INTERVAL_MS) {
      takeAndStoreSample();
      lastSampleTime = millis();
    }
    delay(16); // Give the ESP32 breathing room
  } else if (DEBUG == 1) {
    Serial.println("Enter mock temp for testing...");

    // 1. THE BLOCKING WAIT: Trap the ESP32 here until data arrives
    while (Serial.available() == 0) {
      delay(10); // Small delay keeps the ESP32's watchdog timer happy
    }

    // 2. THE READ: Grab the incoming characters and convert to an integer
    int tmp = Serial.parseInt();

    // 3. THE FLUSH: Clear out the invisible '\n' or '\r' from the Enter key
    while (Serial.available() > 0) {
      Serial.read();
    }

    Serial.print("Executing handleTmpChange for: ");
    Serial.println(tmp);

    // 4. THE EXECUTION
    handleTmpChange(tmp);
  }
}
