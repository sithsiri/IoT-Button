#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <HTTPClient.h>

#define LED_PIN 22

WiFiMulti wifiMulti;
WebServer server(80);

unsigned long lastRequestTime = 0;
const unsigned long TIMEOUT_MS = 5 * 60 * 1000; // 5 minutes
const char* CONFIG_FILE = "/config.txt";

struct Config {
  std::vector<std::pair<String, String>> ssids;
  String postUrl;
  String jsonBody;
} config;

void errorFlash(int n = 3) {
  for (int i = 0; i < n; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
}

// Load configuration from SPIFFS
bool loadConfig() {
  if (!SPIFFS.exists(CONFIG_FILE)) return false;

  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file) return false;

  config.ssids.clear();
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.startsWith("SSID:")) {
      String ssid = line.substring(5);
      String pwd = file.readStringUntil('\n');
      pwd.trim();
      config.ssids.push_back({ssid, pwd});
    } else if (line.startsWith("URL:")) {
      config.postUrl = line.substring(4);
    } else if (line.startsWith("BODY:")) {
      config.jsonBody = line.substring(5);
    }
  }
  file.close();
  return true;
}

// Save configuration to SPIFFS
void saveConfig() {
  File file = SPIFFS.open(CONFIG_FILE, "w");
  for (auto& pair : config.ssids) {
    file.printf("SSID:%s\n%s\n", pair.first.c_str(), pair.second.c_str());
  }
  file.printf("URL:%s\n", config.postUrl.c_str());
  file.printf("BODY:%s\n", config.jsonBody.c_str());
  file.close();
}

// Connect using WiFiMulti
bool connectWiFi() {
  for (auto& pair : config.ssids) {
    wifiMulti.addAP(pair.first.c_str(), pair.second.c_str());
  }

  Serial.println("Connecting to WiFi...");
  for (int i = 0; i < 3; ++i) {
    if (wifiMulti.run() == WL_CONNECTED) {
      Serial.println("Connected!");
      return true;
    }
    errorFlash(1);
  }
  return false;
}

String stationMAC() {
  uint8_t baseMac[6];
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, baseMac);

  char macStr[18]; // 6 bytes * 2 chars + 5 colons + 1 null terminator
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          baseMac[0], baseMac[1], baseMac[2],
          baseMac[3], baseMac[4], baseMac[5]);
  return String(macStr);
}

void handleRoot() {
  lastRequestTime = millis();

  String html = "<html><body><h2>" + String(WiFi.getHostname()) + " WiFi + POST Config</h2>";

  html += "MAC Address: " + String(stationMAC());
  html += "<h3>Saved WiFi Networks:</h3><ul>";
  for (const auto& pair : config.ssids) {
    html += "<li>" + pair.first;
    html += " <a href='/delete?ssid=" + pair.first + "' onclick='return confirm(\"Delete " + pair.first + "?\")'>[Delete]</a></li>";
  }
  html += "</ul><hr>";

  html += "<form method='POST' action='/save'>";
  html += "Add SSID: <input name='ssid'><br>Password: <input name='password'><br><hr>";

  html += "Request URL: <input name='url' value='" + config.postUrl + "'><br>";
  html += "JSON Body: <br><textarea name='body' rows='10' cols='50'>" + config.jsonBody + "</textarea><br>";

  html += "<input type='submit' value='Save'></form></body></html>";
  server.send(200, "text/html", html);
  errorFlash(1);
}

void handleDelete() {
  lastRequestTime = millis();

  String targetSSID = server.arg("ssid");
  config.ssids.erase(std::remove_if(config.ssids.begin(), config.ssids.end(),
    [&](const std::pair<String, String>& entry) {
      return entry.first == targetSSID;
    }), config.ssids.end());

  saveConfig();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Deleted. Redirecting...");
  errorFlash(1);
}

// Save config from form
void handleSave() {
  lastRequestTime = millis();
  String ssid = server.arg("ssid");
  String pwd = server.arg("password");
  String url = server.arg("url");
  String body = server.arg("body");

  if (ssid.length()) config.ssids.push_back({ssid, pwd});
  if (url.length()) config.postUrl = url;
  if (body.length()) config.jsonBody = body;

  saveConfig();

  server.send(200, "text/html", "Saved. Rebooting...");
  errorFlash(3);
  ESP.restart();
}

// Send POST request
void sendPost() {
  if ((WiFi.status() == WL_CONNECTED) && config.postUrl.length()) {
    HTTPClient http;
    http.begin(config.postUrl);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(config.jsonBody);
    Serial.printf("POST sent. Code: %d\n", code);
    http.end();
  }
  errorFlash(1);
}

// Setup AP mode
void setupAP() {
  WiFi.softAP(WiFi.getHostname(), "password");
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/delete", HTTP_GET, handleDelete);
  server.begin();
}

void goToDeepSleep() {
  Serial.println("Entering deep sleep due to inactivity...");
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  SPIFFS.begin(true);
  loadConfig();
  pinMode(12, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  if (digitalRead(12) == 1 && connectWiFi()) {
    digitalWrite(LED_PIN, LOW);
    sendPost();
    digitalWrite(LED_PIN, HIGH);
    goToDeepSleep();  // After sending POST
  } else {
    // If settings pin was driven low or could not connect
    setupAP();
    lastRequestTime = millis();
    errorFlash();
  }
}

void loop() {
  server.handleClient();
  if (millis() - lastRequestTime > TIMEOUT_MS) {
    goToDeepSleep();
  }
}
