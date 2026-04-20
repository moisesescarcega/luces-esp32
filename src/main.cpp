#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <time.h>
#include <ESPmDNS.h>

// ===== CONFIGURACIÓN WIFI =====
const char* WIFI_SSID     = "INFINITUM0FED";   // Nombre de tu red WiFi
const char* WIFI_PASSWORD = "dJAHF9TPTE"; // Contraseña de tu red WiFi

// ===== CONFIGURACIÓN mDNS =====
// Acceso por nombre: http://esp32-luces.local
const char* MDNS_NAME = "esp32-luces";

// ===== CONFIGURACIÓN NTP (hora en tiempo real) =====
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -6 * 3600; // UTC-6 (México Centro) — ajusta si es necesario
const int   DAYLIGHT_OFFSET_SEC = 0;

// ===== CONFIGURACIÓN DE PINES =====
const uint16_t kIrLed  = 4;  // Diodo IR (tira LED)
const int RELAY_PIN    = 12; // Módulo Relé (foco)
const int INTERNAL_LED = 2;  // LED interno (feedback visual)

// ===== PROGRAMACIÓN HORARIA DE LA TIRA LED =====
// Enciende de lunes a domingo de 18:30 a 22:30
int scheduleOnHour   = 18;
int scheduleOnMin    = 30;
int scheduleOffHour  = 22;
int scheduleOffMin   = 30;
bool scheduleEnabled = true;

// ===== VARIABLES DE ESTADO =====
IRsend irsend(kIrLed);
AsyncWebServer server(80);
bool relayState      = false;
bool ledStripState   = false;
bool scheduleTriggeredOn  = false;
bool scheduleTriggeredOff = false;

// ===== SISTEMA IR =====
uint32_t buildConfirmedCode(uint8_t cmd) {
  uint8_t invCmd = ~cmd;
  return (uint32_t)0xFF << 24 | (uint32_t)0x00 << 16 | (uint32_t)cmd << 8 | invCmd;
}

void sendSignal(uint8_t cmd, const char* label) {
  uint32_t code = buildConfirmedCode(cmd);
  digitalWrite(INTERNAL_LED, HIGH);
  Serial.printf("[IR] Enviando %s: Comando %d | Código: 0x%08X\n", label, cmd, code);
  irsend.sendNEC(code);
  delay(100);
  digitalWrite(INTERNAL_LED, LOW);
}

void irOn() {
  sendSignal(161, "ON-1");
  sendSignal(162, "ON-2");
  sendSignal(224, "ON-3");
  ledStripState = true;
  Serial.println("[IR] Tira LED encendida");
}

void irOff() {
  sendSignal(226, "OFF");
  ledStripState = false;
  Serial.println("[IR] Tira LED apagada");
}

// ===== SISTEMA RELÉ =====
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  digitalWrite(INTERNAL_LED, HIGH);
  delay(100);
  digitalWrite(INTERNAL_LED, LOW);
  Serial.printf("[RELAY] Estado: %s\n", relayState ? "ON" : "OFF");
}

// ===== PROGRAMACIÓN HORARIA =====
void checkSchedule() {
  if (!scheduleEnabled) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int currentHour = timeinfo.tm_hour;
  int currentMin  = timeinfo.tm_min;

  bool isOnTime  = (currentHour == scheduleOnHour  && currentMin == scheduleOnMin);
  bool isOffTime = (currentHour == scheduleOffHour && currentMin == scheduleOffMin);

  if (isOnTime && !scheduleTriggeredOn) {
    Serial.println("[SCHEDULE] Hora de encendido → Activando tira LED");
    irOn();
    scheduleTriggeredOn  = true;
    scheduleTriggeredOff = false;
  }

  if (isOffTime && !scheduleTriggeredOff) {
    Serial.println("[SCHEDULE] Hora de apagado → Apagando tira LED");
    irOff();
    scheduleTriggeredOff = true;
    scheduleTriggeredOn  = false;
  }

  if (!isOnTime)  scheduleTriggeredOn  = false;
  if (!isOffTime) scheduleTriggeredOff = false;
}

// ===== INTERFAZ WEB =====
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Control de Luces</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: sans-serif;
      background: #1a1a2e;
      color: #eee;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 2rem 1rem;
      min-height: 100vh;
    }
    h1 { margin-bottom: 2rem; font-size: 1.5rem; color: #a0c4ff; }
    .card {
      background: #16213e;
      border-radius: 12px;
      padding: 1.5rem;
      width: 100%;
      max-width: 360px;
      margin-bottom: 1.5rem;
    }
    h2 { font-size: 1rem; color: #a0c4ff; margin-bottom: 1rem; }
    .btn {
      display: block;
      width: 100%;
      padding: 0.8rem;
      border: none;
      border-radius: 8px;
      font-size: 1rem;
      cursor: pointer;
      margin-bottom: 0.6rem;
      transition: opacity 0.2s;
    }
    .btn:active { opacity: 0.7; }
    .btn-on   { background: #4caf50; color: #fff; }
    .btn-off  { background: #e53935; color: #fff; }
    .btn-save { background: #0f3460; color: #a0c4ff; border: 1px solid #a0c4ff; }
    .status { font-size: 0.85rem; color: #aaa; margin-top: 0.5rem; text-align: center; }
    label { font-size: 0.85rem; color: #aaa; display: block; margin-bottom: 0.3rem; }
    input[type="time"] {
      background: #0f3460;
      border: 1px solid #a0c4ff;
      color: #eee;
      border-radius: 6px;
      padding: 0.4rem 0.6rem;
      font-size: 0.95rem;
      width: 100%;
      margin-bottom: 0.8rem;
    }
    .toggle-row { display: flex; align-items: center; gap: 0.5rem; margin-bottom: 0.8rem; }
  </style>
</head>
<body>
  <h1>💡 Control de Luces</h1>

  <div class="card">
    <h2>🔌 Foco (Relé)</h2>
    <button class="btn btn-on"  onclick="fetch('/relay/on') .then(()=>updateStatus('relay','ON')) ">Encender</button>
    <button class="btn btn-off" onclick="fetch('/relay/off').then(()=>updateStatus('relay','OFF'))">Apagar</button>
    <p class="status" id="relay-status">Estado: --</p>
  </div>

  <div class="card">
    <h2>🌈 Tira LED (IR)</h2>
    <button class="btn btn-on"  onclick="fetch('/ir/on') .then(()=>updateStatus('led','ON')) ">Encender</button>
    <button class="btn btn-off" onclick="fetch('/ir/off').then(()=>updateStatus('led','OFF'))">Apagar</button>
    <p class="status" id="led-status">Estado: --</p>
  </div>

  <div class="card">
    <h2>⏰ Programación Tira LED</h2>
    <div class="toggle-row">
      <input type="checkbox" id="scheduleEnabled" onchange="toggleSchedule(this.checked)">
      <label for="scheduleEnabled" style="margin:0">Programación activa</label>
    </div>
    <label>Hora de encendido</label>
    <input type="time" id="onTime" value="18:30">
    <label>Hora de apagado</label>
    <input type="time" id="offTime" value="22:30">
    <button class="btn btn-save" onclick="saveSchedule()">Guardar horario</button>
    <p class="status" id="schedule-status"></p>
  </div>

  <script>
    function updateStatus(device, state) {
      document.getElementById(device === 'relay' ? 'relay-status' : 'led-status')
        .textContent = 'Estado: ' + state;
    }
    function toggleSchedule(enabled) {
      fetch('/schedule/toggle?enabled=' + (enabled ? '1' : '0'))
        .then(() => document.getElementById('schedule-status').textContent =
          enabled ? 'Programación activada' : 'Programación desactivada');
    }
    function saveSchedule() {
      const on  = document.getElementById('onTime').value;
      const off = document.getElementById('offTime').value;
      fetch(`/schedule/set?on=${on}&off=${off}`)
        .then(() => document.getElementById('schedule-status').textContent =
          `Horario guardado: ${on} – ${off}`);
    }
    fetch('/status').then(r => r.json()).then(data => {
      document.getElementById('relay-status').textContent = 'Estado: ' + (data.relay ? 'ON' : 'OFF');
      document.getElementById('led-status').textContent   = 'Estado: ' + (data.led   ? 'ON' : 'OFF');
      document.getElementById('scheduleEnabled').checked  = data.scheduleEnabled;
      document.getElementById('onTime').value  = data.onTime;
      document.getElementById('offTime').value = data.offTime;
    });
  </script>
</body>
</html>
)rawliteral";

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n===== INICIALIZANDO SISTEMA =====");

  irsend.begin();
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(INTERNAL_LED, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // WiFi
  Serial.printf("Conectando a WiFi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConectado. IP: %s\n", WiFi.localIP().toString().c_str());

  // mDNS
  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("mDNS activo: http://%s.local\n", MDNS_NAME);
  }

  // NTP
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.println("Hora NTP sincronizada");

  // Rutas
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", HTML_PAGE);
  });

  server.on("/relay/on", HTTP_GET, [](AsyncWebServerRequest* req) {
    setRelay(true);
    req->send(200, "text/plain", "OK");
  });

  server.on("/relay/off", HTTP_GET, [](AsyncWebServerRequest* req) {
    setRelay(false);
    req->send(200, "text/plain", "OK");
  });

  server.on("/ir/on", HTTP_GET, [](AsyncWebServerRequest* req) {
    irOn();
    req->send(200, "text/plain", "OK");
  });

  server.on("/ir/off", HTTP_GET, [](AsyncWebServerRequest* req) {
    irOff();
    req->send(200, "text/plain", "OK");
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    char onTime[6], offTime[6];
    snprintf(onTime,  6, "%02d:%02d", scheduleOnHour,  scheduleOnMin);
    snprintf(offTime, 6, "%02d:%02d", scheduleOffHour, scheduleOffMin);
    String json = "{";
    json += "\"relay\":"           + String(relayState     ? "true" : "false") + ",";
    json += "\"led\":"             + String(ledStripState   ? "true" : "false") + ",";
    json += "\"scheduleEnabled\":" + String(scheduleEnabled ? "true" : "false") + ",";
    json += "\"onTime\":\""        + String(onTime)  + "\",";
    json += "\"offTime\":\""       + String(offTime) + "\"";
    json += "}";
    req->send(200, "application/json", json);
  });

  server.on("/schedule/toggle", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("enabled")) {
      scheduleEnabled = req->getParam("enabled")->value() == "1";
      Serial.printf("[SCHEDULE] %s\n", scheduleEnabled ? "Activada" : "Desactivada");
    }
    req->send(200, "text/plain", "OK");
  });

  server.on("/schedule/set", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("on") && req->hasParam("off")) {
      String on  = req->getParam("on")->value();
      String off = req->getParam("off")->value();
      scheduleOnHour  = on.substring(0, 2).toInt();
      scheduleOnMin   = on.substring(3, 5).toInt();
      scheduleOffHour = off.substring(0, 2).toInt();
      scheduleOffMin  = off.substring(3, 5).toInt();
      Serial.printf("[SCHEDULE] Nuevo horario: %02d:%02d – %02d:%02d\n",
        scheduleOnHour, scheduleOnMin, scheduleOffHour, scheduleOffMin);
    }
    req->send(200, "text/plain", "OK");
  });

  server.begin();
  Serial.println("Servidor web iniciado");
  Serial.println("=================================\n");
}

// ===== LOOP PRINCIPAL =====
void loop() {
  checkSchedule();
  delay(10);
}