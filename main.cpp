#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <AccelStepper.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include <vector>

// ============================================================================
// WI-FI CONFIGURATION
// By default, starts an Access Point "ESP32-Piano-Bot" (IP: 192.168.4.1)
// and optionally connects to your local Wi-Fi network if configured below.
// ============================================================================
const char* AP_SSID = "ESP32-Piano-Bot";
const char* AP_PASS = "piano1234";

const char* STA_SSID = ""; // Optional: Enter your local WiFi SSID
const char* STA_PASS = ""; // Optional: Enter your local WiFi Password

// ============================================================================
// PIN DEFINITIONS (ESP32-S3)
// NOTE: DM556 optocoupler inputs are typically rated for 5V logic.
// Connect PUL+, DIR+, ENA+ to 5V, and switch PUL-, DIR-, ENA- via NPN/MOSFET
// or use a 3.3V-to-5V logic level shifter (74HCT245 / TXS0108E).
// ============================================================================
constexpr int PIN_STEP  = 4;   // Connect to DM556 PUL- (or PUL+ with level shifter)
constexpr int PIN_DIR   = 5;   // Connect to DM556 DIR-
constexpr int PIN_ENA   = 6;   // Connect to DM556 ENA- (Optional enable pin)
constexpr int PIN_SERVO = 7;   // Connect to MG90S PWM Signal (Orange wire)
constexpr int PIN_HOME  = 15;  // Optional Limit Switch for Homing (NO to GND)

// ============================================================================
// MECHANICAL & STEPPER PARAMETERS
// ⚠️ HARDWARE NOTE ON TRAVEL LENGTH:
// 22 positions (Keys 0 to 21) spaced 10.5mm apart require:
// 21 intervals * 10.5mm = 220.5mm of linear travel (plus carriage width).
// Because your lead screw is 200mm, MAX_SAFE_TRAVEL_MM caps travel in software
// so the carriage does not crash or spin off the end of the 200mm T8 screw.
// Set MAX_SAFE_TRAVEL_MM to 225.0f if using a longer lead screw.
// ============================================================================
constexpr int   NUM_KEYS           = 22;
constexpr float KEY_SPACING_MM     = 10.5f;   // 10.5mm between each key
constexpr float MAX_SAFE_TRAVEL_MM = 225.0f;  // Adjust to ~185.0f to physically protect a 200mm screw!

// T8 Lead Screw Pitch/Lead:
// Standard 4-start T8 screw moves 8.0mm per revolution.
// (If using a 2-start T8, change to 4.0f; if 1-start, change to 2.0f)
constexpr float LEAD_SCREW_MM_PER_REV = 8.0f;

// Stepper & DM556 Settings:
// 1.8 deg/step = 200 full steps/rev.
// Set DM556 DIP switches SW5=OFF, SW6=OFF, SW7=ON, SW8=ON for 1600脉冲/rev (8x microstepping)
constexpr float MOTOR_FULL_STEPS   = 200.0f;
constexpr float MICROSTEPS         = 8.0f;    // 200 * 8 = 1600 steps/rev
constexpr float STEPS_PER_MM       = (MOTOR_FULL_STEPS * MICROSTEPS) / LEAD_SCREW_MM_PER_REV; // 200 steps/mm

// Motion Profile (in steps/sec and steps/sec^2)
constexpr float MAX_SPEED_STEPS    = 4000.0f; // 4000 steps/s = 20 mm/s
constexpr float ACCEL_STEPS        = 8000.0f; // 8000 steps/s^2 = 40 mm/s^2

// ============================================================================
// MG90S SERVO PARAMETERS
// ============================================================================
constexpr int SERVO_REST_ANGLE     = 0;       // Resting angle (degrees)
constexpr int SERVO_ACTUATE_ANGLE  = 45;      // Actuated +45 deg angle
constexpr uint32_t SERVO_STRIKE_MS = 180;     // Time held down to strike key (ms)
constexpr uint32_t SERVO_RETURN_MS = 180;     // Time allowed for servo to clear key before moving (ms)

// ============================================================================
// DATA STRUCTURES & STATE MACHINE
// ============================================================================
struct NoteEvent {
    int key;              // 0 to 21
    uint32_t hold_ms;     // Downward strike duration (default SERVO_STRIKE_MS)
    uint32_t delay_after; // Pause after note before moving to next key (ms)
};

enum PlayerState {
    STATE_IDLE,
    STATE_MOVING_TO_KEY,
    STATE_SERVO_PRESSING,
    STATE_SERVO_RELEASING,
    STATE_WAITING_POST_NOTE
};

AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);
Servo keyServo;
WebServer server(80);

std::vector<NoteEvent> sequence;
size_t currentNoteIdx = 0;
PlayerState playerState = STATE_IDLE;
uint32_t stateTimer = 0;
int currentTargetKey = 0;

// ============================================================================
// EMBEDDED WEB INTERFACE (HTML / CSS / JS)
// ============================================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32-S3 Stepper Piano Controller</title>
<style>
  :root { --bg: #0f172a; --card: #1e293b; --accent: #3b82f6; --green: #10b981; --red: #ef4444; --text: #f8fafc; }
  body { font-family: system-ui, -apple-system, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; }
  .container { max-width: 850px; margin: 0 auto; }
  h1 { margin-top: 0; font-size: 1.6rem; border-bottom: 1px solid #334155; padding-bottom: 12px; }
  .card { background: var(--card); border-radius: 10px; padding: 18px; margin-bottom: 18px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }
  .status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; }
  .stat-box { background: #0f172a; padding: 12px; border-radius: 8px; border: 1px solid #334155; }
  .stat-label { font-size: 0.78rem; color: #94a3b8; text-transform: uppercase; }
  .stat-val { font-size: 1.2rem; font-weight: 700; margin-top: 4px; color: #38bdf8; }
  textarea { width: 100%; height: 220px; background: #0f172a; color: #a7f3d0; border: 1px solid #475569; border-radius: 8px; padding: 12px; font-family: monospace; font-size: 0.9rem; box-sizing: border-box; }
  .btn-row { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 12px; }
  button, input::file-selector-button { background: var(--accent); color: white; border: none; padding: 10px 18px; border-radius: 6px; font-weight: 600; cursor: pointer; transition: opacity 0.15s; }
  button:hover { opacity: 0.9; }
  .btn-run { background: var(--green); font-size: 1.05rem; padding: 12px 28px; }
  .btn-stop { background: var(--red); }
  .btn-secondary { background: #475569; }
  .piano-strip { display: flex; gap: 4px; overflow-x: auto; padding: 8px 0; }
  .key-btn { min-width: 34px; height: 90px; background: #f8fafc; color: #0f172a; border-radius: 0 0 5px 5px; display: flex; flex-direction: column; justify-content: flex-end; align-items: center; padding-bottom: 6px; font-size: 0.75rem; font-weight: 700; border: 1px solid #64748b; }
  .key-btn:hover { background: #bae6fd; }
  .msg { margin-top: 10px; font-size: 0.9rem; color: #fde047; min-height: 1.2em; }
</style>
</head>
<body>
<div class="container">
  <h1>🎹 ESP32-S3 Stepper Piano Controller</h1>

  <div class="card">
    <div class="status-grid">
      <div class="stat-box"><div class="stat-label">State</div><div class="stat-val" id="st-state">IDLE</div></div>
      <div class="stat-box"><div class="stat-label">Position (mm)</div><div class="stat-val" id="st-mm">0.0</div></div>
      <div class="stat-box"><div class="stat-label">Target Key</div><div class="stat-val" id="st-key">0</div></div>
      <div class="stat-box"><div class="stat-label">Progress</div><div class="stat-val" id="st-prog">0 / 0</div></div>
    </div>
    <div class="btn-row" style="margin-top: 16px;">
      <button class="btn-run" onclick="runSequence()">▶ RUN SEQUENCE</button>
      <button class="btn-stop" onclick="stopSequence()">⏹ STOP</button>
      <button class="btn-secondary" onclick="sweepAllKeys()">🔄 Demo All 22 Keys</button>
      <button class="btn-secondary" onclick="setZero()">📍 Set Current Pos as Zero (Key 0)</button>
    </div>
    <div class="msg" id="status-msg">Ready.</div>
  </div>

  <div class="card">
    <h3 style="margin-top:0;">Upload or Edit Piano Sequence JSON</h3>
    <p style="font-size:0.85rem; color:#94a3b8; margin-top:0;">
      Supported formats: Simple array <code>[0, 4, 7, 12]</code> or detailed object with <code>key</code> (0–21), optional <code>hold_ms</code>, and <code>delay_ms</code>.
    </p>
    <input type="file" id="jsonFile" accept=".json" onchange="loadFile(event)" style="margin-bottom: 10px;">
    <textarea id="jsonInput">{
  "sequence": [
    {"key": 0,  "hold_ms": 180, "delay_ms": 200},
    {"key": 2,  "hold_ms": 180, "delay_ms": 200},
    {"key": 4,  "hold_ms": 180, "delay_ms": 200},
    {"key": 5,  "hold_ms": 180, "delay_ms": 200},
    {"key": 7,  "hold_ms": 300, "delay_ms": 400},
    {"key": 5,  "hold_ms": 180, "delay_ms": 200},
    {"key": 4,  "hold_ms": 180, "delay_ms": 200},
    {"key": 2,  "hold_ms": 180, "delay_ms": 200},
    {"key": 0,  "hold_ms": 400, "delay_ms": 500}
  ]
}</textarea>
    <div class="btn-row">
      <button onclick="uploadJson()">⬆️ Upload JSON to ESP32</button>
      <button class="btn-run" onclick="uploadAndRun()">⬆️ Upload & ▶ Run Immediately</button>
    </div>
  </div>

  <div class="card">
    <h3 style="margin-top:0;">Manual Key Test (Click Key 0–21 to Move & Strike 45°)</h3>
    <div class="piano-strip" id="pianoStrip"></div>
  </div>
</div>

<script>
  const strip = document.getElementById('pianoStrip');
  for (let i = 0; i < 22; i++) {
    const btn = document.createElement('button');
    btn.className = 'key-btn';
    btn.innerHTML = `<span>K${i}</span><span style="font-size:0.62rem;color:#64748b">${(i*10.5).toFixed(1)}</span>`;
    btn.onclick = () => playSingleKey(i);
    strip.appendChild(btn);
  }

  function setMsg(text) { document.getElementById('status-msg').innerText = text; }

  function loadFile(event) {
    const file = event.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = e => {
      document.getElementById('jsonInput').value = e.target.result;
      setMsg("Loaded file: " + file.name + ". Click Upload JSON to send to ESP32.");
    };
    reader.readAsText(file);
  }

  async function uploadJson() {
    const raw = document.getElementById('jsonInput').value;
    try { JSON.parse(raw); } catch (e) { setMsg("❌ Invalid JSON syntax: " + e.message); return false; }
    const res = await fetch('/api/upload', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: raw });
    const data = await res.json();
    setMsg(data.message || "Uploaded");
    return res.ok;
  }

  async function runSequence() {
    const res = await fetch('/api/run', { method: 'POST' });
    const data = await res.json();
    setMsg(data.message);
  }

  async function uploadAndRun() {
    if (await uploadJson()) { await runSequence(); }
  }

  async function stopSequence() {
    const res = await fetch('/api/stop', { method: 'POST' });
    const data = await res.json();
    setMsg(data.message);
  }

  async function setZero() {
    const res = await fetch('/api/zero', { method: 'POST' });
    const data = await res.json();
    setMsg(data.message);
  }

  async function playSingleKey(k) {
    const payload = JSON.stringify({ sequence: [{ key: k, hold_ms: 180, delay_ms: 100 }] });
    await fetch('/api/upload', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: payload });
    await runSequence();
  }

  async function sweepAllKeys() {
    const seq = [];
    for (let i = 0; i < 22; i++) seq.push({ key: i, hold_ms: 180, delay_ms: 150 });
    document.getElementById('jsonInput').value = JSON.stringify({ sequence: seq }, null, 2);
    await uploadAndRun();
  }

  setInterval(async () => {
    try {
      const res = await fetch('/api/status');
      const s = await res.json();
      document.getElementById('st-state').innerText = s.state;
      document.getElementById('st-mm').innerText = s.pos_mm.toFixed(2);
      document.getElementById('st-key').innerText = s.target_key;
      document.getElementById('st-prog').innerText = `${s.current_index} / ${s.total_notes}`;
    } catch (e) {}
  }, 300);
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================
long keyToSteps(int keyIndex) {
    keyIndex = constrain(keyIndex, 0, NUM_KEYS - 1);
    float targetMm = keyIndex * KEY_SPACING_MM;
    if (targetMm > MAX_SAFE_TRAVEL_MM) {
        targetMm = MAX_SAFE_TRAVEL_MM;
    }
    return lround(targetMm * STEPS_PER_MM);
}

const char* stateToString(PlayerState s) {
    switch (s) {
        case STATE_IDLE:              return "IDLE";
        case STATE_MOVING_TO_KEY:     return "MOVING";
        case STATE_SERVO_PRESSING:    return "STRIKING (45°)";
        case STATE_SERVO_RELEASING:   return "RELEASING (0°)";
        case STATE_WAITING_POST_NOTE: return "NOTE DELAY";
        default:                      return "UNKNOWN";
    }
}

void startNextNote() {
    if (currentNoteIdx >= sequence.size()) {
        playerState = STATE_IDLE;
        keyServo.write(SERVO_REST_ANGLE);
        Serial.println("[Piano] Sequence complete!");
        return;
    }

    NoteEvent& note = sequence[currentNoteIdx];
    currentTargetKey = constrain(note.key, 0, NUM_KEYS - 1);
    long targetSteps = keyToSteps(currentTargetKey);

    Serial.printf("[Piano] Note %u/%u -> Moving to Key %d (%.2f mm, %ld steps)
",
                  (unsigned int)(currentNoteIdx + 1),
                  (unsigned int)sequence.size(),
                  currentTargetKey,
                  currentTargetKey * KEY_SPACING_MM,
                  targetSteps);

    stepper.moveTo(targetSteps);
    playerState = STATE_MOVING_TO_KEY;
}

// ============================================================================
// WEB SERVER HANDLERS
// ============================================================================
void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
    JsonDocument doc;
    doc["state"] = stateToString(playerState);
    doc["pos_steps"] = stepper.currentPosition();
    doc["pos_mm"] = stepper.currentPosition() / STEPS_PER_MM;
    doc["target_key"] = currentTargetKey;
    doc["current_index"] = (playerState == STATE_IDLE) ? 0 : (currentNoteIdx + 1);
    doc["total_notes"] = sequence.size();

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void handleUpload() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{"message":"No JSON body received"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        String msg = "{"message":"JSON parse error: " + String(err.c_str()) + ""}";
        server.send(400, "application/json", msg);
        return;
    }

    // Stop active playback before replacing sequence
    playerState = STATE_IDLE;
    keyServo.write(SERVO_REST_ANGLE);
    sequence.clear();

    JsonArray arr;
    if (doc.is<JsonArray>()) {
        arr = doc.as<JsonArray>();
    } else if (doc["sequence"].is<JsonArray>()) {
        arr = doc["sequence"].as<JsonArray>();
    } else if (doc["notes"].is<JsonArray>()) {
        arr = doc["notes"].as<JsonArray>();
    } else {
        server.send(400, "application/json", "{"message":"JSON must be an array or contain a 'sequence' array"}");
        return;
    }

    for (JsonVariant v : arr) {
        NoteEvent ev;
        if (v.is<int>()) {
            ev.key = v.as<int>();
            ev.hold_ms = SERVO_STRIKE_MS;
            ev.delay_after = 150;
        } else if (v.is<JsonObject>()) {
            ev.key = v["key"] | 0;
            ev.hold_ms = v["hold_ms"] | SERVO_STRIKE_MS;
            ev.delay_after = v["delay_ms"] | 150;
        } else {
            continue;
        }

        if (ev.key >= 0 && ev.key < NUM_KEYS) {
            sequence.push_back(ev);
        }
    }

    String resp = "{"message":"Loaded " + String(sequence.size()) + " notes into memory."}";
    Serial.printf("[Web] Uploaded sequence with %u notes.
", (unsigned int)sequence.size());
    server.send(200, "application/json", resp);
}

void handleRun() {
    if (sequence.empty()) {
        server.send(400, "application/json", "{"message":"No sequence loaded! Upload JSON first."}");
        return;
    }
    keyServo.write(SERVO_REST_ANGLE);
    currentNoteIdx = 0;
    startNextNote();
    server.send(200, "application/json", "{"message":"Sequence playback started!"}");
}

void handleStop() {
    playerState = STATE_IDLE;
    stepper.stop();
    keyServo.write(SERVO_REST_ANGLE);
    server.send(200, "application/json", "{"message":"Playback stopped and servo reset to 0°."}");
}

void handleZero() {
    if (playerState != STATE_IDLE) {
        server.send(400, "application/json", "{"message":"Stop playback before zeroing."}");
        return;
    }
    stepper.setCurrentPosition(0);
    currentTargetKey = 0;
    server.send(200, "application/json", "{"message":"Current position calibrated as Key 0 (0.0 mm)."}");
}

// ============================================================================
// MAIN SETUP & NON-BLOCKING STATE LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ESP32-S3 Stepper Piano Bot Starting ===");

    // Configure Stepper Enable Pin (Active LOW on most DM556 setups)
    pinMode(PIN_ENA, OUTPUT);
    digitalWrite(PIN_ENA, LOW);

    // Configure AccelStepper for DM556 (minimum pulse width 5us for optocouplers)
    stepper.setMinPulseWidth(5);
    stepper.setMaxSpeed(MAX_SPEED_STEPS);
    stepper.setAcceleration(ACCEL_STEPS);
    stepper.setCurrentPosition(0);

    // Configure MG90S Servo (50Hz standard PWM, 500us-2400us pulse range)
    ESP32PWM::allocateTimer(0);
    keyServo.setPeriodHertz(50);
    keyServo.attach(PIN_SERVO, 500, 2400);
    keyServo.write(SERVO_REST_ANGLE);

    // Load default 22-key sweep so Run works immediately on boot
    for (int i = 0; i < NUM_KEYS; i++) {
        sequence.push_back({i, SERVO_STRIKE_MS, 150});
    }

    // Start Wi-Fi Access Point + Optional Station Mode
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[WiFi] AP Started! SSID: %s | Pass: %s | URL: http://%s\n",
                  AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());

    if (strlen(STA_SSID) > 0) {
        WiFi.begin(STA_SSID, STA_PASS);
        Serial.printf("[WiFi] Connecting to %s", STA_SSID);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 8000) {
            delay(250);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WiFi] Connected! Local URL: http://%s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\n[WiFi] STA timeout, continuing with AP mode.");
        }
    }

    // Register Web Server Routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/upload", HTTP_POST, handleUpload);
    server.on("/api/run", HTTP_POST, handleRun);
    server.on("/api/stop", HTTP_POST, handleStop);
    server.on("/api/zero", HTTP_POST, handleZero);
    server.begin();
    Serial.println("[Web] Server listening on port 80.");
}

void loop() {
    server.handleClient();

    switch (playerState) {
        case STATE_IDLE:
            break;

        case STATE_MOVING_TO_KEY:
            // Run non-blocking stepper pulse generator until target key is reached
            if (stepper.distanceToGo() != 0) {
                stepper.run();
            } else {
                // Reached target key position -> Actuate MG90S Servo +45 degrees
                keyServo.write(SERVO_REST_ANGLE + SERVO_ACTUATE_ANGLE);
                stateTimer = millis();
                playerState = STATE_SERVO_PRESSING;
            }
            break;

        case STATE_SERVO_PRESSING:
            // Wait for servo to finish pressing key
            if (millis() - stateTimer >= sequence[currentNoteIdx].hold_ms) {
                // Return servo to original position (0 degrees)
                keyServo.write(SERVO_REST_ANGLE);
                stateTimer = millis();
                playerState = STATE_SERVO_RELEASING;
            }
            break;

        case STATE_SERVO_RELEASING:
            // Wait for servo arm to physically clear the piano key before moving stepper!
            if (millis() - stateTimer >= SERVO_RETURN_MS) {
                stateTimer = millis();
                playerState = STATE_WAITING_POST_NOTE;
            }
            break;

        case STATE_WAITING_POST_NOTE:
            if (millis() - stateTimer >= sequence[currentNoteIdx].delay_after) {
                currentNoteIdx++;
                startNextNote();
            }
            break;
    }
}
