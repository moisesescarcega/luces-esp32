#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

// ===== CONFIGURACIÓN DE PINES =====
const uint16_t kIrLed = 4;           // Diodo IR (LED tira)
const int BUTTON_PIN = 19;           // Botón físico (para IR)
const int PIR_SENSOR_PIN = 5;        // Sensor PIR (para Relé)
const int RELAY_PIN = 12;            // Módulo Relé (para foco)
const int INTERNAL_LED = 2;          // LED interno (feedback visual)

// ===== CONFIGURACIÓN DE TIEMPOS =====
const unsigned long PIR_DEBOUNCE_MS = 2400;    // Debounce entre detecciones PIR
const unsigned long PIR_GESTURE_WINDOW = 2500; // Ventana para contar gestos (ms)
const unsigned long BUTTON_DEBOUNCE_MS = 200;  // Debounce del botón
const unsigned long CLICK_DELAY = 400;         // Ventana de clics múltiples (IR)

// ===== VARIABLES DE ESTADO =====
IRsend irsend(kIrLed);
unsigned long lastButtonPress = 0;
unsigned long lastPIRDetection = 0;
unsigned long firstGestureTime = 0;
int gestureCount = 0;
int clickCount = 0;
bool relayState = false;  // false = OFF, true = ON

// ===== SISTEMA IR (BOTÓN + TIRA LED) =====
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

void handleButtonPress() {
  int buttonState = digitalRead(BUTTON_PIN);
  
  if (buttonState == LOW) {
    unsigned long now = millis();
    
    // Antirrebote simple
    if (now - lastButtonPress > BUTTON_DEBOUNCE_MS) {
      clickCount++;
      lastButtonPress = now;
      Serial.printf("[BUTTON] Clic detectado... (Total: %d)\n", clickCount);
    }
    
    // Esperar a que suelte el botón
    while(digitalRead(BUTTON_PIN) == LOW);
  }
  
  // Lógica de clics múltiples después de la ventana de tiempo
  if (clickCount > 0 && (millis() - lastButtonPress > CLICK_DELAY)) {
    if (clickCount == 1) {
      Serial.println("[BUTTON] 1 clic → Enviando IR ENCENDIDO");
      sendSignal(161, "ENCENDIDO");
      sendSignal(162, "ENCENDIDO");
      sendSignal(224, "ENCENDIDO");
    } 
    else if (clickCount >= 2) {
      Serial.println("[BUTTON] 2+ clics → Enviando IR APAGADO");
      sendSignal(226, "APAGADO");
    }
    clickCount = 0; // Reiniciar contador
  }
}

// ===== SISTEMA PIR + RELÉ (SENSOR DE MOVIMIENTO + FOCO) =====
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  digitalWrite(INTERNAL_LED, HIGH);
  delay(100);
  digitalWrite(INTERNAL_LED, LOW);
  Serial.printf("[RELAY] Estado: %s\n", relayState ? "ON" : "OFF");
}

void handlePIRDetection() {
  unsigned long now = millis();

  // Debouncing: ignorar señal PIR dentro de PIR_DEBOUNCE_MS
  if (now - lastPIRDetection < PIR_DEBOUNCE_MS) {
    return;
  }

  // Solo registrar gesto si el PIR acaba de activarse (flanco de subida)
  if (digitalRead(PIR_SENSOR_PIN) == LOW) {
    return;
  }

  lastPIRDetection = now;
  gestureCount++;

  // Registrar tiempo del primer gesto
  if (gestureCount == 1) {
    firstGestureTime = now;
    Serial.println("[PIR] Primer gesto detectado, esperando segundo...");
  } else {
    Serial.printf("[PIR] Gesto %d detectado\n", gestureCount);
  }
}

void evaluateGestures() {
  if (gestureCount == 0) return;

  unsigned long now = millis();

  // Si la ventana de tiempo expiró, tomar decisión
  if (now - firstGestureTime >= PIR_GESTURE_WINDOW) {
    if (gestureCount == 1) {
      Serial.println("[PIR] 1 gesto → ENCENDIENDO relé");
      setRelay(true);
    } else if (gestureCount >= 2) {
      Serial.println("[PIR] 2 gestos → APAGANDO relé");
      setRelay(false);
    }
    gestureCount = 0;
    firstGestureTime = 0;
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n===== INICIALIZANDO SISTEMA =====");
  Serial.println("Sistema IR (Botón → Tira LED)");
  Serial.println("Sistema PIR (Sensor → Relé/Foco)");
  
  // Configurar pines
  irsend.begin();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(PIR_SENSOR_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(INTERNAL_LED, OUTPUT);
  
  // Estado inicial
  digitalWrite(RELAY_PIN, LOW);  // Relé apagado al inicio
  relayState = false;
  
  Serial.printf("\nConfiguración:\n");
  Serial.printf("  IR LED (Tira): GPIO %d\n", kIrLed);
  Serial.printf("  Botón: GPIO %d\n", BUTTON_PIN);
  Serial.printf("  PIR Sensor: GPIO %d\n", PIR_SENSOR_PIN);
  Serial.printf("  Relé: GPIO %d\n", RELAY_PIN);
  Serial.printf("  LED Interno: GPIO %d\n", INTERNAL_LED);
  Serial.printf("\nTiempos:\n");
  Serial.printf("  PIR Debounce: %lu ms\n", PIR_DEBOUNCE_MS);
  Serial.printf("  PIR Ventana de gestos: %lu ms\n", PIR_GESTURE_WINDOW);
  Serial.println("\n¡Listo para pruebas!\n");
}

// ===== LOOP PRINCIPAL =====
void loop() {
  // Sistema 1: Detección del botón (IR)
  handleButtonPress();

  // Sistema 2: Detección del sensor PIR (Relé)
  handlePIRDetection();
  evaluateGestures();

  delay(10);  // Pequeña pausa para no saturar CPU
}