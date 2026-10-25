# ESP-Dungeon

An **ESP32-powered multiplayer text RPG** where players connect over Wi-Fi, submit actions through a browser UI, and occasionally complete a **physical “hold the device” challenge** detected via RSSI changes.  
The ESP32 acts as both web host (for players) and AI client (for dungeon narration via Gemma API).

## What this project does

- Hosts a local web interface on the ESP32 (`WebServer` on port 80)
- Lets players submit RPG actions from a phone/laptop browser
- Sends action prompts to Gemini/Gemma (`generateContent`) for DM narration
- Injects random physical challenges where a player must hold the ESP32 steady
- Uses **RSSI deviation + smoothing + hysteresis** to detect “holding” behavior
- Appends all story/challenge events to an in-memory HTML log

---

## Repository structure

```txt
ESP-Dungeon/
├── sketch_oct25a_copy_20261025162907.ino   # Main firmware (game logic, web server, AI calls, challenge system)
└── README.md                                # Project documentation
```

---

## Core architecture

### 1) Networking mode (AP + STA)
The ESP32 runs in `WIFI_AP_STA` mode:

- **STA**: connects to internet hotspot (`internet_ssid` / `internet_pass`) so it can call the AI API
- **AP**: creates local player network (`Gemma-Dungeon-AP`) for browser clients

### 2) Browser game interface
The firmware serves an embedded HTML page (`index_html`) with:

- Story log panel (`/history`, polled every 2s)
- Live challenge/status banner (`/status`, polled every 400ms)
- Action input that sends player text to `/action?msg=...`

### 3) Game flow & state machine
Challenge/game progression uses:

```cpp
enum State { IDLE, HOLD_WAIT, HOLD_ACTIVE, RESOLVING };
```

- `IDLE`: normal player actions accepted
- `HOLD_WAIT`: waiting for player to begin holding device
- `HOLD_ACTIVE`: hold is in progress and timed
- `RESOLVING`: AI narration being generated after challenge/action

### 4) RSSI “hold” detection
Hold detection is based on the connected STA RSSI:

- Baseline calibration at startup (`baselineRssi`)
- Exponential smoothing (`RSSI_ALPHA`)
- Threshold check (`HOLD_THRESHOLD_DB`)
- Hysteresis in `isHolding()` to avoid rapid toggling
- Dropout grace (`DROPOUT_GRACE`) to tolerate short signal flicker

### 5) AI narration pipeline
`askDM()` and `queryGemmaAI()`:

- Build JSON request with system instruction + prompt
- Send HTTPS request using `WiFiClientSecure`
- Parse response JSON via `ArduinoJson`
- Extract non-thought text segments
- Convert newlines to `<br>` for HTML rendering

---

## Main endpoints

- `GET /` → game UI
- `GET /history` → full HTML story log
- `GET /status` → current challenge banner text
- `GET /action?msg=<text>` → submit player action and get updated log

---

## Key configuration values (in code)

- `CHALLENGE_CHANCE` (default `35`)  
  Chance (percent) that a challenge starts after each action
- `HOLD_START_WINDOW` (`10000 ms`)  
  Time allowed to start holding
- `HOLD_DURATION` (`4000 ms`)  
  Required continuous hold time
- `HOLD_THRESHOLD_DB` (`5.0`)  
  RSSI deviation threshold for “hold detected”
- `api_key`  
  Must be set for AI calls to work

---

## Requirements

- ESP32 board
- Arduino framework/toolchain with:
  - `WiFi.h`
  - `WebServer.h`
  - `WiFiClientSecure.h`
  - `ArduinoJson.h`
- Internet-connected hotspot for STA mode
- Browser-capable client device to join ESP AP

---

## Setup

1. Open `sketch_oct25a_copy_20261025162907.ino`
2. Set:
   - `internet_ssid`
   - `internet_pass`
   - `api_key`
3. Flash firmware to ESP32
4. Open serial monitor at `115200`
5. Connect a phone/laptop to `Gemma-Dungeon-AP`
6. Browse to the AP IP shown in serial logs (commonly `192.168.4.1`)

---

## Security notes (important)

Current code includes hardcoded credential fields and uses:

```cpp
client.setInsecure();
```

That disables TLS certificate validation. For safer deployment:

- Move secrets to a non-committed config header or build flags
- Use proper certificate validation (root CA or pinning)
- Rotate credentials if they were ever committed publicly

---

## Known limitations

- All game/story state is RAM-only (`globalStoryLog`), so reboot clears progress
- Single sketch file; no modular split yet
- No authentication on local web endpoints
- Basic error fallback on AI parse/network errors

---

## Suggested next improvements

- Split into modules (`network`, `dm_client`, `challenge`, `ui_routes`)
- Add persistent story storage (SPIFFS/LittleFS/NVS)
- Add multi-party turn management/player IDs
- Add retry/backoff and richer AI error handling
- Externalize all secrets/config from source
