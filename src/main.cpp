#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

const uint16_t kIrLed = 4;
const int BUTTON_PIN = 19;
const int INTERNAL_LED = 2;
IRsend irsend(kIrLed);

unsigned long lastButtonPress = 0;
int clickCount = 0;
const int CLICK_DELAY = 400;

uint32_t buildConfirmedCode(uint8_t cmd) {
  uint8_t invCmd = ~cmd;
  return (uint32_t)0xFF << 24 | (uint32_t)0x00 << 16 | (uint32_t)cmd << 8 | invCmd;
}

void sendSignal(uint8_t cmd, const char* label) {
  uint32_t code = buildConfirmedCode(cmd);
  digitalWrite(INTERNAL_LED, HIGH);
  Serial.printf("Enviando %s: Comando %d | Código: 0x%08X\n", label, cmd, code);
  irsend.sendNEC(code);
  delay(100);
  digitalWrite(INTERNAL_LED, LOW);
}

void setup() {
  irsend.begin();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(INTERNAL_LED, OUTPUT);
  Serial.begin(115200);
  Serial.println("Controlador IR Listo: 1 clic = ON, 2 clics = OFF");
}

void loop() {
  int buttonState = digitalRead(BUTTON_PIN);

  if (buttonState == LOW) {
    unsigned long now = millis();
    // Antirrebote simple
    if (now - lastButtonPress > 200) {
      clickCount++;
      lastButtonPress = now;
      Serial.println("Clic detectado...");
    }
    // Esperar a que suelte el botón para no contar clics falsos
    while(digitalRead(BUTTON_PIN) == LOW); 
  }

  // Lógica de decisión después de la ventana de tiempo
  if (clickCount > 0 && (millis() - lastButtonPress > CLICK_DELAY)) {
    if (clickCount == 1) {
      sendSignal(161, "ENCENDIDO");
      sendSignal(162, "ENCENDIDO");
      sendSignal(224, "ENCENDIDO");
    } 
    else if (clickCount >= 2) {
      sendSignal(226, "APAGADO");
    }
    
    clickCount = 0; // Reiniciar contador
  }
}