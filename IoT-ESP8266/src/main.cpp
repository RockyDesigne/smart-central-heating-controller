#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>

// Define a structured block of memory for our credentials
struct Credentials {
  char ssid[32];     // Max length of an SSID is 32 chars
  char password[64]; // Max length of a WPA2 password is 63 chars
};

Credentials creds;   // Create a global instance of our struct
DNSServer dnsServer;
// ===== Button Timing =====
unsigned long buttonPressStartTime = 0;
bool isButtonPressed = false;
// ===== WiFi =====
const char* MDNS_NAME = "relay";  
// ===== Web =====
ESP8266WebServer server(80);
// ===== PINS =====
const int ledPin = LED_BUILTIN;
const int BOOT_BUTTON_PIN = D3;
const int relayPin = D1;
void startCaptivePortal() {
  Serial.println("\n[Captive Portal] Starting...");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Relay_Setup"); 
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, []() {
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<style>body{font-family:Arial; padding:20px; text-align:center;} input{margin:10px 0; padding:10px; width:100%; box-sizing:border-box;} button{padding:10px 20px; background:#007BFF; color:white; border:none; border-radius:5px;}</style></head>"
                  "<body><h2>Relay Setup</h2>"
                  "<form action='/save' method='POST'>"
                  "<input type='text' name='ssid' placeholder='WiFi Network Name (SSID)' required><br>"
                  "<input type='password' name='pass' placeholder='WiFi Password' required><br>"
                  "<button type='submit'>Save & Connect</button></form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");
    
    // 1. Wipe the old struct clean with zeros
    memset(&creds, 0, sizeof(creds));
    
    // 2. Safely copy the new strings into the char arrays
    strncpy(creds.ssid, newSsid.c_str(), sizeof(creds.ssid) - 1);
    strncpy(creds.password, newPass.c_str(), sizeof(creds.password) - 1);
    
    // 3. Write it to memory address 0
    EEPROM.put(0, creds);
    
    // 4. THE EEPROM TRAP: You MUST call commit() or it won't actually save!
    EEPROM.commit();
    
    server.send(200, "text/html", "<html><body><h2>Saved!</h2><p>Rebooting relay...</p></body></html>");
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
    
    if (millis() - lastBlinkTime > 500) {
      ledState = !ledState;
      digitalWrite(ledPin, ledState ? HIGH : LOW);
      lastBlinkTime = millis();
    }
    
    delay(10);
  }
}
void turnHeaterOn() {
  digitalWrite(ledPin, LOW);  // Turn LED ON (Active LOW)
  // Turn the heater ON (Send Ground/0V to the IN pin)
  digitalWrite(relayPin, LOW);
  server.send(200);
}
void turnHeaterOff() {
  digitalWrite(ledPin, HIGH); // Turn LED OFF
  // Turn the heater OFF (disconnects the pin entirely, causing the relay to pull up to 5V and get set to OFF)
  digitalWrite(relayPin, HIGH);
  server.send(200);
}

void setup() {
  Serial.begin(115200);

  pinMode(ledPin, OUTPUT);
  // THE MAGIC FIX: Open Drain allows the active-low relay to shut off
  pinMode(relayPin, OUTPUT_OPEN_DRAIN);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  // CRITICAL FAILSAFE FOR ACTIVE-LOW RELAYS:
  // Immediately set to HIGH so the heater starts in the OFF state
  digitalWrite(relayPin, HIGH);
  digitalWrite(ledPin, HIGH);

  // Tell the ESP8266 we need 512 bytes of EEPROM
  EEPROM.begin(512); 
  
  // Grab the data from memory address 0 and shove it into our struct
  EEPROM.get(0, creds); 

  // Convert the char arrays back to Strings so your WiFi code can read them
  String savedSsid = String(creds.ssid);
  String savedPass = String(creds.password);

  if (savedSsid == "") {
    Serial.println("\n[WiFi] Memory blank. First-time setup required.");
    startCaptivePortal();
  }

  Serial.printf("\n[WiFi] Attempting to connect to: %s\n", savedSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_NAME);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[mDNS] http://%s.local/\n", MDNS_NAME);
    } else {
      Serial.println("[mDNS] start failed");
    }
  } else {
    // FIX: The Roadblock - Trap it here if WiFi failed!
      Serial.println("\n[WiFi] Failed to connect! Router offline or bad password.");
      startCaptivePortal(); 
  }

  server.on("/turn-heater-on",           HTTP_GET, turnHeaterOn);
  server.on("/turn-heater-off",           HTTP_GET, turnHeaterOff);

  server.begin();
  Serial.println("[HTTP] Server started");
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("[HTTP] Open: http://" + WiFi.localIP().toString() + "/  or  http://" + MDNS_NAME + ".local/");
}

void loop() {
  server.handleClient();
  MDNS.update();
  // ===== FACTORY RESET CHECK =====
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      if (!isButtonPressed) {
        isButtonPressed = true;
        buttonPressStartTime = millis(); // Start the stopwatch
      } else if (millis() - buttonPressStartTime > 1000) {
        // Button held for 3 seconds!
        Serial.println("\n[Security] Factory Reset Triggered!");
        Serial.println("Wiping WiFi credentials...");
        
        // Wipe the struct with zeros
        memset(&creds, 0, sizeof(creds));
        
        // Write the blank struct to memory and save it
        EEPROM.put(0, creds);
        EEPROM.commit();
        
        Serial.println("Rebooting into Captive Portal...");
        delay(1000);
        ESP.restart(); // Force reboot
      }
    } else {
      isButtonPressed = false; // Reset stopwatch if button is released early
    }
  delay(16);
}
