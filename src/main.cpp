#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <time.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ===== CONFIGURACIÓN WIFI =====
const char* WIFI_SSID     = "Totalplay-3EB7";   // Nombre de tu red WiFi
const char* WIFI_PASSWORD = "3EB75116FYw9Dem4"; // Contraseña de tu red WiFi

// ===== CONFIGURACIÓN mDNS =====
// Acceso por nombre: http://esp32-luces.local
const char* MDNS_NAME = "esp32-luces";

// ===== CONFIGURACIÓN NTP =====
const char* NTP_SERVER      = "pool.ntp.org";
const long  GMT_OFFSET_SEC  = -6 * 3600; // UTC-6 (México Centro)
const int   DAYLIGHT_OFFSET_SEC = 0;

// ===== CONFIGURACIÓN BLE =====
// UUIDs únicos para el servicio y características BLE
#define BLE_SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define BLE_CHAR_RELAY_UUID     "12345678-1234-1234-1234-123456789ab1"
#define BLE_CHAR_IR_UUID        "12345678-1234-1234-1234-123456789ab2"
#define BLE_CHAR_STATUS_UUID    "12345678-1234-1234-1234-123456789ab3"

// ===== CONFIGURACIÓN DE PINES =====
const uint16_t kIrLed  = 4;  // Diodo IR (tira LED)
const int RELAY_PIN    = 12; // Módulo Relé (foco)
const int INTERNAL_LED = 2;  // LED interno (feedback visual)

// ===== TIEMPOS =====
const unsigned long WIFI_TIMEOUT_MS    = 60000; // 60s para conectar WiFi
const unsigned long WIFI_HEARTBEAT_MS  = 60000; // Parpadeo cada 60s si WiFi OK
const unsigned long WIFI_CHECK_MS      = 10000; // Verificar conexión cada 10s

// ===== PROGRAMACIÓN HORARIA =====
int scheduleOnHour   = 18;
int scheduleOnMin    = 00;
int scheduleOffHour  = 22;
int scheduleOffMin   = 30;
bool scheduleEnabled = true;

// ===== VARIABLES DE ESTADO =====
IRsend irsend(kIrLed);
AsyncWebServer server(80);

bool relayState            = false;
bool ledStripState         = false;
bool scheduleTriggeredOn   = false;
bool scheduleTriggeredOff  = false;

bool wifiConnected         = false;
bool bleEnabled            = false;

unsigned long lastHeartbeat   = 0;
unsigned long lastWifiCheck   = 0;

BLEServer*         pBLEServer      = nullptr;
BLECharacteristic* pCharRelay      = nullptr;
BLECharacteristic* pCharIR         = nullptr;
BLECharacteristic* pCharStatus     = nullptr;
bool bleClientConnected            = false;

// ===== SISTEMA IR =====
uint32_t buildConfirmedCode(uint8_t cmd) {
  uint8_t invCmd = ~cmd;
  return (uint32_t)0xFF << 24 | (uint32_t)0x00 << 16 | (uint32_t)cmd << 8 | invCmd;
}

void sendSignal(uint8_t cmd, const char* label) {
  uint32_t code = buildConfirmedCode(cmd);
  Serial.printf("[IR] Enviando %s: Comando %d | Código: 0x%08X\n", label, cmd, code);
  irsend.sendNEC(code);
  delay(100);
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
  Serial.printf("[RELAY] Estado: %s\n", relayState ? "ON" : "OFF");
}

// ===== LED INTERNO (feedback WiFi) =====
void blinkLED(int times, int ms = 80) {
  for (int i = 0; i < times; i++) {
    digitalWrite(INTERNAL_LED, HIGH);
    delay(ms);
    digitalWrite(INTERNAL_LED, LOW);
    delay(ms);
  }
}

void updateLEDStatus() {
  if (wifiConnected) {
    // Parpadeo breve cada minuto
    unsigned long now = millis();
    if (now - lastHeartbeat >= WIFI_HEARTBEAT_MS) {
      blinkLED(2, 60); // 2 parpadeos rápidos discretos
      lastHeartbeat = now;
    }
  } else {
    // LED fijo encendido: WiFi fallando
    digitalWrite(INTERNAL_LED, HIGH);
  }
}

// ===== BLE — CALLBACKS =====
// Notifica estado actual al cliente BLE
void notifyBLEStatus() {
  if (!bleEnabled || !bleClientConnected || pCharStatus == nullptr) return;
  String status = String(relayState ? "1" : "0") + String(ledStripState ? "1" : "0");
  pCharStatus->setValue(status.c_str());
  pCharStatus->notify();
}

class BLERelayCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String val = pChar->getValue().c_str();
    Serial.printf("[BLE] Relay cmd: %s\n", val.c_str());
    if (val == "relay:on")  setRelay(true);
    if (val == "relay:off") setRelay(false);
    notifyBLEStatus();
  }
};

class BLEIRCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String val = pChar->getValue().c_str();
    Serial.printf("[BLE] IR cmd: %s\n", val.c_str());
    if (val == "ir:on")  irOn();
    if (val == "ir:off") irOff();
    notifyBLEStatus();
  }
};

class BLEServerCallback : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleClientConnected = true;
    Serial.println("[BLE] Cliente conectado");
  }
  void onDisconnect(BLEServer* pServer) override {
    bleClientConnected = false;
    Serial.println("[BLE] Cliente desconectado — reanunciando...");
    pServer->startAdvertising();
  }
};

// ===== INICIAR BLE =====
void startBLE() {
  if (bleEnabled) return;
  Serial.println("[BLE] Iniciando...");

  BLEDevice::init("ESP32-Luces");
  pBLEServer = BLEDevice::createServer();
  pBLEServer->setCallbacks(new BLEServerCallback());

  BLEService* pService = pBLEServer->createService(BLE_SERVICE_UUID);

  // Característica Relé (escritura)
  pCharRelay = pService->createCharacteristic(
    BLE_CHAR_RELAY_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharRelay->setCallbacks(new BLERelayCallback());

  // Característica IR (escritura)
  pCharIR = pService->createCharacteristic(
    BLE_CHAR_IR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharIR->setCallbacks(new BLEIRCallback());

  // Característica Status (notificación → ESP32 notifica al cliente)
  pCharStatus = pService->createCharacteristic(
    BLE_CHAR_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharStatus->addDescriptor(new BLE2902());

  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();

  bleEnabled = true;
  Serial.println("[BLE] Listo — Nombre: ESP32-Luces");
}

// ===== WIFI — VERIFICACIÓN PERIÓDICA =====
void checkWiFi() {
  unsigned long now = millis();
  if (now - lastWifiCheck < WIFI_CHECK_MS) return;
  lastWifiCheck = now;

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) {
      // Acaba de perder conexión
      wifiConnected = false;
      Serial.println("[WiFi] Conexión perdida — habilitando BLE");
      startBLE();
    }
  } else {
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.printf("[WiFi] Reconectado. IP: %s\n", WiFi.localIP().toString().c_str());
    }
  }
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
    h1 { margin-bottom: 0.5rem; font-size: 1.5rem; color: #a0c4ff; }
    .conn-bar {
      display: flex;
      align-items: center;
      gap: 0.5rem;
      font-size: 0.8rem;
      color: #aaa;
      margin-bottom: 1.5rem;
    }
    .dot {
      width: 10px; height: 10px;
      border-radius: 50%;
      background: #555;
      transition: background 0.3s;
    }
    .dot.ok  { background: #4caf50; }
    .dot.err { background: #e53935; }
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
      display: block; width: 100%;
      padding: 0.8rem; border: none;
      border-radius: 8px; font-size: 1rem;
      cursor: pointer; margin-bottom: 0.6rem;
      transition: opacity 0.2s;
    }
    .btn:active { opacity: 0.7; }
    .btn-on   { background: #4caf50; color: #fff; }
    .btn-off  { background: #e53935; color: #fff; }
    .btn-ble  { background: #7b2fff; color: #fff; }
    .btn-save { background: #0f3460; color: #a0c4ff; border: 1px solid #a0c4ff; }
    .status   { font-size: 0.85rem; color: #aaa; margin-top: 0.5rem; text-align: center; }
    label     { font-size: 0.85rem; color: #aaa; display: block; margin-bottom: 0.3rem; }
    input[type="time"] {
      background: #0f3460; border: 1px solid #a0c4ff;
      color: #eee; border-radius: 6px;
      padding: 0.4rem 0.6rem; font-size: 0.95rem;
      width: 100%; margin-bottom: 0.8rem;
    }
    .toggle-row { display: flex; align-items: center; gap: 0.5rem; margin-bottom: 0.8rem; }
    #ble-card { display: none; }
  </style>
</head>
<body>
  <h1>💡 Control de Luces</h1>

  <!-- INDICADOR DE CONEXIÓN -->
  <div class="conn-bar">
    <div class="dot" id="wifi-dot"></div>
    <span id="wifi-label">Verificando WiFi...</span>
    &nbsp;|&nbsp;
    <div class="dot" id="ble-dot"></div>
    <span id="ble-label">BLE: --</span>
  </div>

  <!-- RELÉ -->
  <div class="card">
    <h2>🔌 Foco (Relé)</h2>
    <button class="btn btn-on"  onclick="wifiCmd('/relay/on',  'relay', 'ON')">Encender</button>
    <button class="btn btn-off" onclick="wifiCmd('/relay/off', 'relay', 'OFF')">Apagar</button>
    <p class="status" id="relay-status">Estado: --</p>
  </div>

  <!-- TIRA LED IR -->
  <div class="card">
    <h2>🌈 Tira LED (IR)</h2>
    <button class="btn btn-on"  onclick="wifiCmd('/ir/on',  'led', 'ON')">Encender</button>
    <button class="btn btn-off" onclick="wifiCmd('/ir/off', 'led', 'OFF')">Apagar</button>
    <p class="status" id="led-status">Estado: --</p>
  </div>

  <!-- PROGRAMACIÓN -->
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

  <!-- BLE (visible solo si WiFi falla) -->
  <div class="card" id="ble-card">
    <h2>📶 Control vía Bluetooth</h2>
    <button class="btn btn-ble" onclick="bleConnect()">Conectar por BLE</button>
    <p class="status" id="ble-status">Sin conectar</p>
    <div id="ble-controls" style="display:none; margin-top:1rem;">
      <button class="btn btn-on"  onclick="bleCmd('relay','relay:on',  'ON')">Foco ON</button>
      <button class="btn btn-off" onclick="bleCmd('relay','relay:off', 'OFF')">Foco OFF</button>
      <button class="btn btn-on"  onclick="bleCmd('led','ir:on',  'ON')" style="margin-top:0.4rem">LED ON</button>
      <button class="btn btn-off" onclick="bleCmd('led','ir:off', 'OFF')">LED OFF</button>
    </div>
  </div>

  <script>
    // ===== WIFI =====
    const PING_INTERVAL = 15000; // Verificar WiFi cada 15s
    let wifiOk = true;

    function wifiCmd(endpoint, device, state) {
      fetch(endpoint, { signal: AbortSignal.timeout(4000) })
        .then(r => { if (r.ok) updateStatus(device, state); })
        .catch(() => showWifiError());
    }

    function updateStatus(device, state) {
      const id = device === 'relay' ? 'relay-status' : 'led-status';
      document.getElementById(id).textContent = 'Estado: ' + state;
    }

    function showWifiError() {
      setWifiIndicator(false);
      document.getElementById('ble-card').style.display = 'block';
    }

    function setWifiIndicator(ok) {
      wifiOk = ok;
      const dot   = document.getElementById('wifi-dot');
      const label = document.getElementById('wifi-label');
      dot.className   = 'dot ' + (ok ? 'ok' : 'err');
      label.textContent = ok ? 'WiFi: Conectado' : 'WiFi: Sin conexión';
      document.getElementById('ble-card').style.display = ok ? 'none' : 'block';
    }

    function ping() {
      fetch('/ping', { signal: AbortSignal.timeout(3000) })
        .then(r => setWifiIndicator(r.ok))
        .catch(() => setWifiIndicator(false));
    }

    setInterval(ping, PING_INTERVAL);

    // ===== PROGRAMACIÓN =====
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

    // ===== BLE (Web Bluetooth API) =====
    const BLE_SERVICE  = '12345678-1234-1234-1234-123456789abc';
    const BLE_RELAY    = '12345678-1234-1234-1234-123456789ab1';
    const BLE_IR       = '12345678-1234-1234-1234-123456789ab2';
    const BLE_STATUS   = '12345678-1234-1234-1234-123456789ab3';

    let bleDevice, bleRelayCh, bleIRCh, bleStatusCh;

    async function bleConnect() {
      const statusEl = document.getElementById('ble-status');
      try {
        statusEl.textContent = 'Buscando dispositivo...';
        bleDevice = await navigator.bluetooth.requestDevice({
          filters: [{ name: 'ESP32-Luces' }],
          optionalServices: [BLE_SERVICE]
        });
        const server  = await bleDevice.gatt.connect();
        const service = await server.getPrimaryService(BLE_SERVICE);
        bleRelayCh  = await service.getCharacteristic(BLE_RELAY);
        bleIRCh     = await service.getCharacteristic(BLE_IR);
        bleStatusCh = await service.getCharacteristic(BLE_STATUS);

        // Suscribirse a notificaciones de estado
        await bleStatusCh.startNotifications();
        bleStatusCh.addEventListener('characteristicvaluechanged', e => {
          const val = new TextDecoder().decode(e.target.value);
          updateStatus('relay', val[0] === '1' ? 'ON' : 'OFF');
          updateStatus('led',   val[1] === '1' ? 'ON' : 'OFF');
        });

        statusEl.textContent = '✅ BLE Conectado';
        document.getElementById('ble-dot').className   = 'dot ok';
        document.getElementById('ble-label').textContent = 'BLE: Conectado';
        document.getElementById('ble-controls').style.display = 'block';

        bleDevice.addEventListener('gattserverdisconnected', () => {
          statusEl.textContent = '❌ BLE Desconectado';
          document.getElementById('ble-dot').className   = 'dot err';
          document.getElementById('ble-label').textContent = 'BLE: Desconectado';
          document.getElementById('ble-controls').style.display = 'none';
        });

      } catch (err) {
        statusEl.textContent = '❌ Error: ' + err.message;
      }
    }

    async function bleCmd(device, cmd, state) {
      if (!bleRelayCh || !bleIRCh) return;
      const encoder = new TextEncoder();
      try {
        if (device === 'relay') await bleRelayCh.writeValue(encoder.encode(cmd));
        if (device === 'led')   await bleIRCh.writeValue(encoder.encode(cmd));
        updateStatus(device, state);
      } catch (err) {
        document.getElementById('ble-status').textContent = '❌ Error BLE: ' + err.message;
      }
    }

    // ===== CARGAR ESTADO INICIAL =====
    fetch('/status').then(r => r.json()).then(data => {
      updateStatus('relay', data.relay ? 'ON' : 'OFF');
      updateStatus('led',   data.led   ? 'ON' : 'OFF');
      document.getElementById('scheduleEnabled').checked = data.scheduleEnabled;
      document.getElementById('onTime').value  = data.onTime;
      document.getElementById('offTime').value = data.offTime;
      setWifiIndicator(true);
    }).catch(() => setWifiIndicator(false));
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
  digitalWrite(INTERNAL_LED, LOW);

  // Configurar IP fija
  IPAddress localIP(192, 168, 100, 16);
  IPAddress gateway(192, 168, 100, 254);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns(8, 8, 8, 8);
  WiFi.config(localIP, gateway, subnet, dns);

  // Intentar conectar WiFi con timeout de 60s
  Serial.printf("Conectando a WiFi: %s\n", WIFI_SSID);
  Serial.println("IP fija: 192.168.100.16");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\n[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin(MDNS_NAME)) {
      Serial.printf("[mDNS] http://%s.local\n", MDNS_NAME);
    }

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.println("[NTP] Hora sincronizada");

    // ===== RUTAS DEL SERVIDOR WEB =====
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
      req->send_P(200, "text/html", HTML_PAGE);
    });
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* req) {
      req->send(200, "text/plain", "PONG");
    });
    server.on("/relay/on", HTTP_GET, [](AsyncWebServerRequest* req) {
      setRelay(true); req->send(200, "text/plain", "OK");
    });
    server.on("/relay/off", HTTP_GET, [](AsyncWebServerRequest* req) {
      setRelay(false); req->send(200, "text/plain", "OK");
    });
    server.on("/ir/on", HTTP_GET, [](AsyncWebServerRequest* req) {
      irOn(); req->send(200, "text/plain", "OK");
    });
    server.on("/ir/off", HTTP_GET, [](AsyncWebServerRequest* req) {
      irOff(); req->send(200, "text/plain", "OK");
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
    Serial.println("[Web] Servidor iniciado");

  } else {
    // WiFi falló — iniciar BLE directamente
    wifiConnected = false;
    Serial.println("\n[WiFi] Timeout — iniciando BLE");
    startBLE();
  }

  Serial.println("=================================\n");
}

// ===== LOOP PRINCIPAL =====
void loop() {
  checkWiFi();
  checkSchedule();
  updateLEDStatus();
  delay(10);
}