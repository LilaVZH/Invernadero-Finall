#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>

#define PIN_LED 19

//-----------------------------------------------------------------------------
// Estructura recibida
typedef struct __attribute__((packed)) {
  uint8_t led;
} ComandoAct;
ComandoAct actRec;

// Semáforo
SemaphoreHandle_t actMutex;

//------------------------------------------------------------------------------
// Función de recepción ESP-NOW
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == sizeof(ComandoAct)) {
    if (xSemaphoreTake(actMutex, (TickType_t)10) == pdTRUE) {
      memcpy(&actRec, data, sizeof(ComandoAct));
      xSemaphoreGive(actMutex);
    }
  }
}

//-------------------------------------------------------------------------------
// Tarea para controlar el actuador
void ControlActuador(void *pvParameters) {
  ComandoAct recibido;

  while (true) {
    if (xSemaphoreTake(actMutex, portMAX_DELAY) == pdTRUE) {
      memcpy(&recibido, &actRec, sizeof(ComandoAct));
      xSemaphoreGive(actMutex);

      if (recibido.led == 1) {
        digitalWrite(PIN_LED, HIGH);
        Serial.println("💡 LED ENCENDIDO");
      } else {
        digitalWrite(PIN_LED, LOW);
        Serial.println("💤 LED APAGADO -> entrando en deep sleep");

        // Configurar para despertar por ESP-NOW
        esp_wifi_set_promiscuous(true);  // Requerido para recibir en deep sleep
        esp_sleep_enable_wifi_wakeup();  // Habilita el wakeup por WiFi (ESP-NOW)

        delay(100); // Asegurar que el mensaje se imprima
        esp_deep_sleep_start(); // Entrar en deep sleep
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));  // Pequeña espera
  }
}

//------------------------------------------------------------------------------
// Setup
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Inicializar Wi-Fi en modo estación
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE); // Desactiva ahorro de energía de WiFi para ESP-NOW

  // Inicializar ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error inicializando ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);

  // Crear semáforo y tarea 
  actMutex = xSemaphoreCreateMutex();
  xTaskCreate(ControlActuador, "ControlActuador", 4096, NULL, 1, NULL);
}

//------------------------------------------------------------------------------
void loop() {}
