#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// --- Wi-Fi Credentials for Internet ---
const char* internet_ssid = "iPhone";
const char* internet_pass = "";

// --- Wi-Fi Credentials for Players ---
const char* ap_ssid = "Gemma-Dungeon-AP";
const char* ap_pass = "hackathon123";

// --- Gemini API Configuration ---
const char* api_key = "";
const char* api_host = "generativelanguage.googleapis.com";
const int   api_port = 443;

// --- RSSI "Hold" Config ---
const float HOLD_THRESHOLD_DB   = 5.0;   // dB shift from baseline that counts as "holding"
const float RSSI_ALPHA          = 0.25;  // smoothing (lower = smoother, slower)
const unsigned long SAMPLE_MS   = 50;
const int   CHALLENGE_CHANCE    = 35;    // % chance after each player action

const unsigned long HOLD_START_WINDOW = 10000; // time to begin holding
const unsigned long HOLD_DURATION     = 4000;  // how long to hold
const unsigned long DROPOUT_GRACE     = 700;   // tolerate brief RSSI flicker

WebServer server(80);

String globalStoryLog = "";
String challengeBanner = "";

enum State { IDLE, HOLD_WAIT, HOLD_ACTIVE, RESOLVING };
State state = IDLE;

float smoothedRssi = -70.0;
float baselineRssi = -70.0;
bool  held = false;
unsigned long stateStart = 0;
unsigned long lastHeldTime = 0;
unsigned long lastSample = 0;
unsigned long lastPrint = 0;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Gemma Dungeon Master</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: monospace; background: #121212; color: #00ffcc; text-align: center; padding: 20px; }
        #log { background: #1e1e1e; border: 1px solid #00ffcc; height: 350px; overflow-y: scroll; padding: 15px; text-align: left; margin-bottom: 15px; line-height: 1.4; }
        #banner { min-height: 28px; margin-bottom: 12px; padding: 8px; font-size: 18px; font-weight: bold; color: #ffcc00; border: 1px dashed #ffcc00; border-radius: 4px; display: none; }
        input, button { padding: 12px; font-size: 16px; background: #222; color: #00ffcc; border: 1px solid #00ffcc; border-radius: 4px; }
        input { width: 65%; }
    </style>
</head>
<body>
    <h1>Gemma Dungeon Master</h1>
    <div id="banner"></div>
    <div id="log">Connecting to Dungeon...</div>
    <input type="text" id="actionInput" placeholder="Enter your move..." onkeydown="if(event.key === 'Enter') sendAction();">
    <button onclick="sendAction()">Send Action</button>

    <script>
        setInterval(updateLog, 2000);
        setInterval(updateStatus, 400);
        updateLog();

        function updateLog() {
            fetch('/history').then(r => r.text()).then(data => {
                let log = document.getElementById('log');
                if (log.innerHTML !== data) {
                    log.innerHTML = data;
                    log.scrollTop = log.scrollHeight;
                }
            }).catch(() => {});
        }

        function updateStatus() {
            fetch('/status').then(r => r.text()).then(data => {
                let b = document.getElementById('banner');
                if (data) { b.style.display = 'block'; b.innerHTML = data; }
                else { b.style.display = 'none'; }
            }).catch(() => {});
        }

        function sendAction() {
            let input = document.getElementById('actionInput');
            if (input.value.trim()) {
                let actionText = input.value;
                input.value = '';
                fetch('/action?msg=' + encodeURIComponent(actionText))
                .then(r => r.text())
                .then(data => {
                    let log = document.getElementById('log');
                    log.innerHTML = data;
                    log.scrollTop = log.scrollHeight;
                });
            }
        }
    </script>
</body>
</html>
)rawliteral";

void queryGemmaAI(String promptText, String &outputResponse);
String askDM(String prompt);
void startChallenge();
void updateChallenge();
void finishChallenge(bool success, String detail);
void sampleRssi();
bool isHolding();

// ---------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1000);

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(internet_ssid, internet_pass);
    Serial.print("Connecting to Internet");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected to Internet!");

    WiFi.softAP(ap_ssid, ap_pass);
    Serial.print("Player Access Point IP: ");
    Serial.println(WiFi.softAPIP());

    // Calibrate RSSI baseline - keep hands off the ESP32 and hotspot phone still
    Serial.println("Calibrating RSSI baseline - don't touch the ESP32!");
    float sum = 0; int count = 0;
    for (int i = 0; i < 40; i++) {
        int r = WiFi.RSSI();
        if (r < 0) { sum += r; count++; }
        delay(50);
    }
    if (count > 0) baselineRssi = sum / count;
    smoothedRssi = baselineRssi;
    Serial.printf("RSSI baseline: %.1f dBm\n", baselineRssi);

    // --- Routes ---
    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", index_html); });
    server.on("/history", HTTP_GET, []() { server.send(200, "text/html", globalStoryLog); });
    server.on("/status", HTTP_GET, []() { server.send(200, "text/html", challengeBanner); });

    server.on("/action", HTTP_GET, []() {
        if (!server.hasArg("msg")) {
            server.send(400, "text/plain", "Missing message");
            return;
        }
        if (state != IDLE) {  // ignore actions during a challenge / AI call
            server.send(200, "text/html", globalStoryLog);
            return;
        }

        String playerAction = server.arg("msg");
        Serial.printf("Received action: %s\n", playerAction.c_str());
        globalStoryLog += "<br><br><b>Player:</b> " + playerAction;

        String narration = askDM("The party takes this action: " + playerAction + ". Continue the text RPG adventure.");
        globalStoryLog += "<br><br><b>DM:</b> " + narration;
        server.send(200, "text/html", globalStoryLog);

        if (random(0, 100) < CHALLENGE_CHANCE) startChallenge();
    });

    server.begin();

    // --- Opening scene ---
    state = RESOLVING;
    challengeBanner = "The Dungeon Master is conjuring the dungeon...";
    globalStoryLog = "Welcome to the dungeon! Summoning your adventure...<br>";

    String intro = askDM(
        "Begin a brand new dungeon adventure for a party of players. Describe the setting vividly in 3-4 sentences. "
        "End with a sudden danger that can only be overcome by someone physically gripping an ancient, humming "
        "relic (the ESP32 device) and holding it steady right now.");
    globalStoryLog = "<b>DM:</b> " + intro;

    startChallenge();  // immediate first challenge
}

void loop() {
    server.handleClient();
    sampleRssi();
    updateChallenge();
    delay(5);
}

// ---------------------------------------------------------------
// RSSI sensing
void sampleRssi() {
    unsigned long now = millis();
    if (now - lastSample < SAMPLE_MS) return;
    lastSample = now;

    int r = WiFi.RSSI();           // RSSI of the connected hotspot
    if (r >= 0) return;            // 0 = not connected / invalid, skip

    smoothedRssi = RSSI_ALPHA * r + (1.0 - RSSI_ALPHA) * smoothedRssi;

    // Slowly track drift, but only while idle and not being touched
    float dev = fabs(smoothedRssi - baselineRssi);
    if (state == IDLE && dev < HOLD_THRESHOLD_DB) {
        baselineRssi += 0.005 * (smoothedRssi - baselineRssi);
    }

    // Serial Plotter-friendly output
    if (now - lastPrint > 100) {
        lastPrint = now;
        Serial.printf("DATA,%d,%d\n", (int)smoothedRssi, (int)baselineRssi);
    }
}

// Hysteresis: engage at full threshold, release at half
bool isHolding() {
    float dev = fabs(smoothedRssi - baselineRssi);
    if (!held && dev >= HOLD_THRESHOLD_DB) held = true;
    else if (held && dev < HOLD_THRESHOLD_DB * 0.5) held = false;
    return held;
}

// ---------------------------------------------------------------
// Hold challenge
void startChallenge() {
    state = HOLD_WAIT;
    stateStart = millis();
    lastHeldTime = stateStart;
    held = false;
    challengeBanner = "✋ HOLD THE ESP32 NOW! Wrap your hand around it for " + String(HOLD_DURATION / 1000) + " seconds!";
    globalStoryLog += "<br><br><i>[CHALLENGE: Grab the ESP32 and HOLD it for " + String(HOLD_DURATION / 1000) + " seconds!]</i>";
    Serial.println("Hold challenge started");
}

void updateChallenge() {
    if (state == IDLE || state == RESOLVING) return;

    bool holding = isHolding();
    unsigned long now = millis();
    unsigned long elapsed = now - stateStart;

    switch (state) {
        case HOLD_WAIT:
            if (holding) {
                state = HOLD_ACTIVE;
                stateStart = now;
                lastHeldTime = now;
            } else if (elapsed > HOLD_START_WINDOW) {
                finishChallenge(false, "The player never gripped the relic in time.");
            }
            break;

        case HOLD_ACTIVE:
            if (holding) lastHeldTime = now;

            if (now - lastHeldTime > DROPOUT_GRACE) {
                finishChallenge(false, "The player let go after " + String(elapsed / 1000.0, 1) + " seconds.");
            } else if (elapsed >= HOLD_DURATION) {
                finishChallenge(true, "The player held the relic steady the whole time.");
            } else {
                challengeBanner = "✋ HOLD... " + String((HOLD_DURATION - elapsed) / 1000.0, 1) + "s left";
            }
            break;

        default: break;
    }
}

void finishChallenge(bool success, String detail) {
    state = RESOLVING;
    String result = success ? "SUCCESS" : "FAILURE";
    challengeBanner = success ? "✅ Success! The DM is responding..." : "❌ Failed! The DM is responding...";
    globalStoryLog += "<br><br><b>Challenge " + result + ":</b> " + detail;
    Serial.println("Challenge " + result + ": " + detail);

    String narration = askDM(
        "A physical challenge was just attempted by the party: hold the relic steady for " + String(HOLD_DURATION / 1000) +
        " seconds. Result: " + result + ". " + detail +
        " Narrate the consequence briefly (a reward for success, a mild penalty for failure), then set up the next situation.");
    globalStoryLog += "<br><br><b>DM:</b> " + narration;

    held = false;
    challengeBanner = "";
    state = IDLE;
}

// ---------------------------------------------------------------
// AI helpers
String askDM(String prompt) {
    String raw = "";
    queryGemmaAI(prompt, raw);
    Serial.println("RAW RESPONSE:");
    Serial.println(raw);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, raw, DeserializationOption::NestingLimit(10));

    String clean = "";
    if (!error) {
        JsonArray parts = doc["candidates"][0]["content"]["parts"];
        for (JsonObject part : parts) {
            if (!part["thought"].as<bool>() && part["text"].is<const char*>()) {
                clean = part["text"].as<String>();
            }
        }
    } else {
        Serial.print("JSON Parsing failed: ");
        Serial.println(error.c_str());
    }
    if (clean == "") clean = "The dungeon shifts silently... (AI parse fallback)";
    clean.replace("\n", "<br>");
    return clean;
}

void queryGemmaAI(String promptText, String &outputResponse) {
    WiFiClientSecure client;
    client.setInsecure();

    if (client.connect(api_host, api_port)) {
        String url = "/v1beta/models/gemma-4-26b-a4b-it:generateContent";

        JsonDocument req;
        req["systemInstruction"]["parts"][0]["text"] =
            "You are a strict, creative Dungeon Master running a multi-user text RPG played on an ESP32. "
            "Respond briefly (under 120 words) in narrative style. Players sometimes must physically hold the device.";
        req["contents"][0]["parts"][0]["text"] = promptText;

        String payload;
        serializeJson(req, payload);

        client.print(String("POST ") + url + " HTTP/1.0\r\n");
        client.print(String("Host: ") + api_host + "\r\n");
        client.print("Content-Type: application/json\r\n");
        client.print("x-goog-api-key: " + String(api_key) + "\r\n");
        client.print("Content-Length: " + String(payload.length()) + "\r\n");
        client.print("Connection: close\r\n\r\n");
        client.print(payload);

        while (client.connected()) {
            String line = client.readStringUntil('\n');
            if (line == "\r") break;
        }

        outputResponse = "";
        unsigned long timeout = millis();
        while (client.connected() && millis() - timeout < 10000) {
            while (client.available()) {
                outputResponse += (char)client.read();
                timeout = millis();
            }
            yield();
        }
        client.stop();

        int start = outputResponse.indexOf('{');
        int end = outputResponse.lastIndexOf('}');
        if (start >= 0 && end > start) {
            outputResponse = outputResponse.substring(start, end + 1);
        }
    } else {
        outputResponse = "{\"error\": \"Socket connection failed\"}";
    }
}