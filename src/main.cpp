#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <time.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "secrets.h"

// ===== CONFIGURACIÓN =====
const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* MDNS_NAME     = "esp32-luces";

const long GMT_OFFSET_SEC      = -6 * 3600; // UTC-6 México Centro
const int  DAYLIGHT_OFFSET_SEC = 0;

const uint16_t kIrLed  = 4;
const int RELAY_PIN    = 12;
const int INTERNAL_LED = 2;

const unsigned long WIFI_HEARTBEAT_MS = 60000;
const unsigned long WIFI_CHECK_MS     = 10000;
const unsigned long SYNC_RETRY_MS     = 300000; // Reintentar sincronización cada 5 min

// ===== ESTADO GLOBAL =====
IRsend irsend(kIrLed);
AsyncWebServer server(80);

bool relayState    = false;
bool ledStripState = false;
bool wifiConnected = false;
bool timeSynced    = false; // Reemplaza ntpSynced

int  scheduleOnHour  = 18, scheduleOnMin  = 0;
int  scheduleOffHour = 22, scheduleOffMin = 30;
bool scheduleEnabled      = true;
bool scheduleTriggeredOn  = false;
bool scheduleTriggeredOff = false;

// Hora manual: segundos desde medianoche (-1 = no configurada)
long          manualBaseSecs = -1;
unsigned long manualBaseMs   = 0;

unsigned long lastHeartbeat = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastSyncRetry = 0;

// ===== IR =====
uint32_t buildCode(uint8_t cmd) {
  return (uint32_t)0xFF << 24 | (uint32_t)0x00 << 16 | (uint32_t)cmd << 8 | (uint8_t)(~cmd);
}

void sendSignal(uint8_t cmd, const char* label) {
  uint32_t code = buildCode(cmd);
  Serial.printf("[IR] %s: 0x%08X\n", label, code);
  irsend.sendNEC(code);
  delay(100);
}

void irOn()  {
  sendSignal(161, "ON-1"); sendSignal(162, "ON-2"); sendSignal(224, "ON-3");
  ledStripState = true;
}
void irOff() {
  sendSignal(226, "OFF");
  ledStripState = false;
}

// ===== RELÉ =====
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, state ? HIGH : LOW);
  Serial.printf("[RELAY] %s\n", state ? "ON" : "OFF");
}

// ===== LED INTERNO =====
void blinkLED(int n, int ms = 80) {
  for (int i = 0; i < n; i++) {
    digitalWrite(INTERNAL_LED, HIGH); delay(ms);
    digitalWrite(INTERNAL_LED, LOW);  delay(ms);
  }
}

void updateLEDStatus() {
  if (wifiConnected) {
    unsigned long now = millis();
    if (now - lastHeartbeat >= WIFI_HEARTBEAT_MS) { blinkLED(2, 60); lastHeartbeat = now; }
  } else {
    digitalWrite(INTERNAL_LED, HIGH);
  }
}

// ===== WIFI =====
void checkWiFi() {
  unsigned long now = millis();
  if (now - lastWifiCheck < WIFI_CHECK_MS) return;
  lastWifiCheck = now;
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok != wifiConnected) {
    wifiConnected = ok;
    Serial.printf("[WiFi] %s\n", ok ? "Reconectado" : "Desconectado");
  }
}

// ===== HTTP TIME SYNC (Puerto 443) =====
bool syncHTTP() {
  if (!wifiConnected) return false;
  
  WiFiClientSecure client;
  client.setInsecure(); // Evita verificación de certificados para mitigar expiraciones
  HTTPClient http;
  long epoch = 0;

  // 1. WorldTimeAPI (Endpoint txt simple)
  Serial.println("[HTTP Time] Probando WorldTimeAPI...");
  http.begin(client, "https://worldtimeapi.org/api/timezone/America/Mexico_City.txt");
  if (http.GET() == HTTP_CODE_OK) {
    String payload = http.getString();
    int idx = payload.indexOf("unixtime: ");
    if (idx != -1) {
      epoch = payload.substring(idx + 10, payload.indexOf("\n", idx)).toInt();
    }
  }
  http.end();

  // 2. Fallback: Google Date Header
  if (epoch == 0) {
    Serial.println("[HTTP Time] Probando Google Header...");
    const char* headerKeys[] = {"Date"};
    http.begin(client, "https://www.google.com/");
    http.collectHeaders(headerKeys, 1);
    if (http.sendRequest("HEAD") > 0) {
      String dateStr = http.header("Date");
      if (dateStr.length() > 0) {
        struct tm tm;
        // Parsear formato: Wed, 21 Oct 2015 07:28:00 GMT
        if (strptime(dateStr.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm) != NULL) {
          setenv("TZ", "GMT0", 1); tzset(); // Configurar entorno a GMT para mktime
          epoch = mktime(&tm);
        }
      }
    }
    http.end();
  }

  // Si se obtuvo el epoch, configurar el reloj del sistema
  if (epoch > 0) {
    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    // Restaurar zona horaria a CST (UTC-6) sin horario de verano
    setenv("TZ", "CST6", 1);
    tzset();

    struct tm ti;
    if (getLocalTime(&ti, 500)) {
      char buf[20]; strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
      Serial.printf("[HTTP Time] Sincronizado → %s\n", buf);
      timeSynced = true;
      manualBaseSecs = -1; // Descarta hora manual
      return true;
    }
  }

  Serial.println("[HTTP Time] Sin respuesta o fallo en conversión en todos los servidores");
  timeSynced = false;
  return false;
}

// Reintento periódico no bloqueante
void retryTimeSync() {
  unsigned long now = millis();
  if (!timeSynced && wifiConnected && now - lastSyncRetry >= SYNC_RETRY_MS) {
    lastSyncRetry = now;
    syncHTTP();
  }
}

// ===== FUENTE DE TIEMPO UNIFICADA =====
bool getCurrentTime(struct tm* t) {
  if (timeSynced && getLocalTime(t, 100)) return true;
  if (manualBaseSecs >= 0) {
    long total = manualBaseSecs + (long)((millis() - manualBaseMs) / 1000);
    memset(t, 0, sizeof(struct tm));
    t->tm_hour = (total / 3600) % 24;
    t->tm_min  = (total / 60)   % 60;
    t->tm_sec  =  total         % 60;
    t->tm_year = 125; t->tm_mday = 1;
    return true;
  }
  return false;
}

String getTimeString() {
  struct tm t;
  if (!getCurrentTime(&t)) return "Sin hora";
  char buf[10];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}

// ===== PROGRAMACIÓN HORARIA =====
void checkSchedule() {
  if (!scheduleEnabled) return;
  struct tm t;
  if (!getCurrentTime(&t)) return;

  bool isOn  = (t.tm_hour == scheduleOnHour  && t.tm_min == scheduleOnMin);
  bool isOff = (t.tm_hour == scheduleOffHour && t.tm_min == scheduleOffMin);

  if (!isOn)  scheduleTriggeredOn  = false;
  if (!isOff) scheduleTriggeredOff = false;

  if (isOn  && !scheduleTriggeredOn)  { irOn();  scheduleTriggeredOn  = true; Serial.println("[SCHEDULE] Encendido"); }
  if (isOff && !scheduleTriggeredOff) { irOff(); scheduleTriggeredOff = true; Serial.println("[SCHEDULE] Apagado");  }
}

// ===== HTML =====
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Control de Luces</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;flex-direction:column;align-items:center;padding:2rem 1rem;min-height:100vh}
    h1{margin-bottom:.5rem;font-size:1.5rem;color:#a0c4ff}
    .conn-bar{display:flex;align-items:center;gap:.5rem;font-size:.8rem;color:#aaa;margin-bottom:.5rem}
    .dot{width:10px;height:10px;border-radius:50%;background:#555;transition:background .3s}
    .dot.ok{background:#4caf50}.dot.err{background:#e53935}.dot.warn{background:#ff9800}
    .time-display{background:#16213e;border-radius:10px;padding:.8rem 1.2rem;margin-bottom:1.2rem;text-align:center;border:1px solid #0f3460;width:100%;max-width:360px}
    .tlabel{font-size:.75rem;color:#888;text-transform:uppercase;letter-spacing:1px;margin-bottom:.3rem}
    .time-value{font-size:1.4rem;font-family:'Courier New',monospace;color:#a0c4ff;font-weight:bold}
    .ntp-row{font-size:.75rem;margin-top:.3rem;display:flex;align-items:center;justify-content:center;gap:.4rem}
    .ntp-row.synced{color:#4caf50}.ntp-row.manual{color:#ff9800}.ntp-row.error{color:#e53935}
    .manual-section{margin-top:.7rem;display:none;border-top:1px solid #2a3060;padding-top:.7rem;text-align:left}
    .manual-section .mlabel{color:#ff9800;font-size:.8rem;display:block;margin-bottom:.4rem}
    .manual-section input{background:#0f3460;border:1px solid #ff9800;color:#eee;border-radius:6px;padding:.35rem .6rem;font-size:.9rem;width:100%;margin-bottom:.5rem}
    .card{background:#16213e;border-radius:12px;padding:1.5rem;width:100%;max-width:360px;margin-bottom:1.5rem}
    h2{font-size:1rem;color:#a0c4ff;margin-bottom:1rem}
    .btn{display:block;width:100%;padding:.8rem;border:none;border-radius:8px;font-size:1rem;cursor:pointer;margin-bottom:.6rem;transition:opacity .2s}
    .btn:active{opacity:.7}
    .btn-on{background:#4caf50;color:#fff}.btn-off{background:#e53935;color:#fff}
    .btn-save{background:#0f3460;color:#a0c4ff;border:1px solid #a0c4ff}
    .btn-sm{background:transparent;border:1px solid #a0c4ff;color:#a0c4ff;padding:.3rem .8rem;border-radius:6px;font-size:.75rem;cursor:pointer;margin-top:.4rem;transition:all .2s;display:inline-block}
    .btn-sm:hover{background:#a0c4ff;color:#16213e}.btn-sm:disabled{opacity:.5;cursor:not-allowed}
    .btn-warn{border-color:#ff9800;color:#ff9800}.btn-warn:hover{background:#ff9800;color:#1a1a2e}
    .status{font-size:.85rem;color:#aaa;margin-top:.5rem;text-align:center}
    label{font-size:.85rem;color:#aaa;display:block;margin-bottom:.3rem}
    input[type="time"]{background:#0f3460;border:1px solid #a0c4ff;color:#eee;border-radius:6px;padding:.4rem .6rem;font-size:.95rem;width:100%;margin-bottom:.8rem}
    .toggle-row{display:flex;align-items:center;gap:.5rem;margin-bottom:.8rem}
  </style>
</head>
<body>
  <h1>💡 Control de Luces</h1>

  <div class="conn-bar">
    <div class="dot" id="wifi-dot"></div>
    <span id="wifi-label">Verificando WiFi...</span>
  </div>

  <div class="time-display">
    <div class="tlabel">🕐 Hora del ESP32</div>
    <div class="time-value" id="current-time">--:--:--</div>
    <div class="ntp-row" id="ntp-row">
      <span class="dot" id="ntp-dot"></span>
      <span id="ntp-text">Verificando...</span>
    </div>
    <button class="btn-sm" id="refresh-btn" onclick="refreshTime()">🔄 Actualizar</button>

    <div class="manual-section" id="manual-section">
      <span class="mlabel">⚠ HTTP Time no disponible — ingresa la hora actual</span>
      <input type="time" id="manualTime">
      <button class="btn-sm btn-warn" onclick="setManualTime()">✅ Aplicar hora</button>
    </div>
  </div>

  <div class="card">
    <h2>🔌 Foco (Relé)</h2>
    <button class="btn btn-on"  onclick="cmd('/relay/on',  'relay', 'ON')">Encender</button>
    <button class="btn btn-off" onclick="cmd('/relay/off', 'relay', 'OFF')">Apagar</button>
    <p class="status" id="relay-status">Estado: --</p>
  </div>

  <div class="card">
    <h2>🌈 Tira LED (IR)</h2>
    <button class="btn btn-on"  onclick="cmd('/ir/on',  'led', 'ON')">Encender</button>
    <button class="btn btn-off" onclick="cmd('/ir/off', 'led', 'OFF')">Apagar</button>
    <p class="status" id="led-status">Estado: --</p>
  </div>

  <div class="card">
    <h2>⏰ Programación Tira LED</h2>
    <div class="toggle-row">
      <input type="checkbox" id="scheduleEnabled" onchange="toggleSchedule(this.checked)">
      <label for="scheduleEnabled" style="margin:0">Programación activa</label>
    </div>
    <label>Encendido</label>
    <input type="time" id="onTime"  value="18:00">
    <label>Apagado</label>
    <input type="time" id="offTime" value="22:30">
    <button class="btn btn-save" onclick="saveSchedule()">Guardar horario</button>
    <p class="status" id="schedule-status"></p>
  </div>

  <script>
    function cmd(ep, dev, state) {
      fetch(ep).then(r => {
        const id = dev === 'relay' ? 'relay-status' : 'led-status';
        document.getElementById(id).textContent = r.ok ? 'Estado: ' + state : 'Error';
        if (!r.ok) setWifi(false);
      }).catch(() => setWifi(false));
    }

    function setWifi(ok) {
      document.getElementById('wifi-dot').className     = 'dot ' + (ok ? 'ok' : 'err');
      document.getElementById('wifi-label').textContent = ok ? 'WiFi: Conectado' : 'WiFi: Sin conexión';
    }

    function setNTP(synced, manual, timeStr) {
      document.getElementById('current-time').textContent = timeStr || '--:--:--';
      const dot  = document.getElementById('ntp-dot');
      const text = document.getElementById('ntp-text');
      const row  = document.getElementById('ntp-row');
      const sect = document.getElementById('manual-section');
      if (synced) {
        dot.className = 'dot ok'; text.textContent = 'HTTP: Sincronizado ✓';
        row.className = 'ntp-row synced'; sect.style.display = 'none';
      } else if (manual) {
        dot.className = 'dot warn'; text.textContent = 'Hora manual ⚠';
        row.className = 'ntp-row manual'; sect.style.display = 'none';
      } else {
        dot.className = 'dot err'; text.textContent = 'Sin hora ✗';
        row.className = 'ntp-row error'; sect.style.display = 'block';
      }
    }

    function refreshTime() {
      const btn = document.getElementById('refresh-btn');
      btn.disabled = true; btn.textContent = '⏳';
      fetch('/time')
        .then(r => r.json())
        .then(d => setNTP(d.synced, d.manual, d.time))
        .catch(() => setNTP(false, false, 'Error'))
        .finally(() => { btn.disabled = false; btn.textContent = '🔄 Actualizar'; });
    }

    function setManualTime() {
      const val = document.getElementById('manualTime').value;
      if (!val) return;
      const [h, m] = val.split(':');
      fetch('/time/set?h=' + h + '&m=' + m)
        .then(r => { if (r.ok) refreshTime(); })
        .catch(() => setWifi(false));
    }

    function toggleSchedule(on) {
      fetch('/schedule/toggle?enabled=' + (on ? '1' : '0'))
        .then(() => document.getElementById('schedule-status').textContent = on ? 'Programación activada' : 'Programación desactivada')
        .catch(() => setWifi(false));
    }

    function saveSchedule() {
      const on  = document.getElementById('onTime').value;
      const off = document.getElementById('offTime').value;
      fetch('/schedule/set?on=' + on + '&off=' + off)
        .then(r => {
          document.getElementById('schedule-status').textContent =
            r.ok ? 'Guardado: ' + on + ' – ' + off : 'Error al guardar';
        }).catch(() => setWifi(false));
    }

    fetch('/status').then(r => r.json()).then(d => {
      document.getElementById('relay-status').textContent = 'Estado: ' + (d.relay ? 'ON' : 'OFF');
      document.getElementById('led-status').textContent   = 'Estado: ' + (d.led   ? 'ON' : 'OFF');
      document.getElementById('scheduleEnabled').checked  = d.scheduleEnabled;
      document.getElementById('onTime').value  = d.onTime;
      document.getElementById('offTime').value = d.offTime;
      setWifi(true);
    }).catch(() => setWifi(false));

    refreshTime();
    setInterval(() => fetch('/ping').then(r => setWifi(r.ok)).catch(() => setWifi(false)), 15000);
    setInterval(refreshTime, 10000);
  </script>
</body>
</html>
)rawliteral";

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n===== INICIALIZANDO =====");

  irsend.begin();
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(INTERNAL_LED, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(INTERNAL_LED, LOW);

  IPAddress localIP(192,168,100,201), gateway(192,168,100,254),
            subnet(255,255,255,0),   dns1(8,8,8,8), dns2(8,8,4,4);
  // WiFi.config(localIP, gateway, subnet, dns1, dns2);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 60000) {
    delay(500); Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] Timeout — sin conexión");
    Serial.println("=========================\n");
    return;
  }

  wifiConnected = true;
  Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
  if (MDNS.begin(MDNS_NAME)) Serial.printf("[mDNS] http://%s.local\n", MDNS_NAME);

  syncHTTP(); 

  // ── RUTAS ──────────────────────────────────────────────────────────────────

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html", HTML_PAGE);
  });

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", "PONG");
  });

  server.on("/time", HTTP_GET, [](AsyncWebServerRequest* r) {
    bool manual = (!timeSynced && manualBaseSecs >= 0);
    String json = "{\"synced\":"  + String(timeSynced ? "true" : "false") +
                  ",\"manual\":"  + String(manual     ? "true" : "false") +
                  ",\"time\":\""  + getTimeString() + "\"}";
    r->send(200, "application/json", json);
  });

  server.on("/time/set", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("h") && r->hasParam("m")) {
      int h = r->getParam("h")->value().toInt();
      int m = r->getParam("m")->value().toInt();
      int s = r->hasParam("s") ? r->getParam("s")->value().toInt() : 0;
      manualBaseSecs = (long)h * 3600 + (long)m * 60 + s;
      manualBaseMs   = millis();
      timeSynced      = false;
      Serial.printf("[TIME] Hora manual: %02d:%02d:%02d\n", h, m, s);
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/relay/on",  HTTP_GET, [](AsyncWebServerRequest* r) { setRelay(true);  r->send(200, "text/plain", "OK"); });
  server.on("/relay/off", HTTP_GET, [](AsyncWebServerRequest* r) { setRelay(false); r->send(200, "text/plain", "OK"); });
  server.on("/ir/on",     HTTP_GET, [](AsyncWebServerRequest* r) { irOn();           r->send(200, "text/plain", "OK"); });
  server.on("/ir/off",    HTTP_GET, [](AsyncWebServerRequest* r) { irOff();          r->send(200, "text/plain", "OK"); });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    char on[6], off[6];
    snprintf(on,  6, "%02d:%02d", scheduleOnHour,  scheduleOnMin);
    snprintf(off, 6, "%02d:%02d", scheduleOffHour, scheduleOffMin);
    String json = "{\"relay\":"           + String(relayState     ? "true" : "false") +
                  ",\"led\":"             + String(ledStripState   ? "true" : "false") +
                  ",\"scheduleEnabled\":" + String(scheduleEnabled ? "true" : "false") +
                  ",\"onTime\":\""  + on  + "\"" +
                  ",\"offTime\":\"" + off + "\"}";
    r->send(200, "application/json", json);
  });

  server.on("/schedule/toggle", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("enabled"))
      scheduleEnabled = r->getParam("enabled")->value() == "1";
    Serial.printf("[SCHEDULE] %s\n", scheduleEnabled ? "Activada" : "Desactivada");
    r->send(200, "text/plain", "OK");
  });

  server.on("/schedule/set", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("on") && r->hasParam("off")) {
      String on  = r->getParam("on")->value();
      String off = r->getParam("off")->value();
      scheduleOnHour  = on.substring(0,2).toInt();  scheduleOnMin  = on.substring(3,5).toInt();
      scheduleOffHour = off.substring(0,2).toInt(); scheduleOffMin = off.substring(3,5).toInt();
      Serial.printf("[SCHEDULE] Horario: %02d:%02d – %02d:%02d\n",
        scheduleOnHour, scheduleOnMin, scheduleOffHour, scheduleOffMin);
    }
    r->send(200, "text/plain", "OK");
  });

  server.begin();
  Serial.println("[Web] Servidor iniciado");
  Serial.println("=========================\n");
}

// ===== LOOP =====
void loop() {
  checkWiFi();
  retryTimeSync();  
  checkSchedule();  
  updateLEDStatus();
  delay(10);
}