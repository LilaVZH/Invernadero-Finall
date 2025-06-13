//=============================================================================
// 📡 ESP32 - Receptor ESP-NOW con control de actuador y deep sleep
//
// Este código configura un ESP32 para recibir comandos por ESP-NOW.
// Al recibir un valor '1', enciende un LED. Al recibir un '0', apaga el LED 
// y entra en modo deep sleep, despertando automáticamente al recibir un nuevo
// mensaje por ESP-NOW.
//
// - Pin del LED: GPIO 19
// - Comunicación: ESP-NOW
// - Sleep: Deep Sleep con wakeup por WiFi (ESP-NOW)
//
// Autor: [Tu Nombre]
// Fecha: [Fecha del proyecto]
//=============================================================================


//=============================================================================
// 🧩 Librerías necesarias
//=============================================================================
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>


//=============================================================================
// ⚙️ Definiciones y variables globales
//=============================================================================
#define PIN_LED 19  // Pin donde está conectado el LED

// Estructura del comando recibido
typedef struct __attribute__((packed)) {
  uint8_t led;  // 1: Encender LED, 0: Apagar LED y dormir
} ComandoAct;

ComandoAct actRec;  // Variable global para almacenar el último comando

// Semáforo para proteger acceso a la variable compartida
SemaphoreHandle_t actMutex;


//=============================================================================
// 📥 Callback de recepción por ESP-NOW
//=============================================================================
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == sizeof(ComandoAct)) {
    if (xSemaphoreTake(actMutex, (TickType_t)10) == pdTRUE) {
      memcpy(&actRec, data, sizeof(ComandoAct));
      xSemaphoreGive(actMutex);
    }
  }
}


//=============================================================================
// 💡 Tarea para controlar el actuador (LED)
//=============================================================================
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

        // Configurar para recibir en modo deep sleep por WiFi
        esp_wifi_set_promiscuous(true);      // Requerido para recepción en sleep
        esp_sleep_enable_wifi_wakeup();      // Wakeup por recepción WiFi (ESP-NOW)

        delay(100);                          // Permite imprimir el mensaje
        esp_deep_sleep_start();              // Entra en deep sleep
      }
    }

    // Pequeño retardo antes de volver a revisar
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}


//=============================================================================
// 🚀 Configuración inicial (setup)
//=============================================================================
void setup() {
  Serial.begin(115200);

  // Inicializar pin del LED
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Inicializar Wi-Fi en modo estación (STA)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // Evita conexiones previas
  esp_wifi_set_ps(WIFI_PS_NONE);  // Desactiva el ahorro de energía del WiFi

  // Inicializar ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error inicializando ESP-NOW");
    return;
  }

  // Registrar función de recepción
  esp_now_register_recv_cb(OnDataRecv);

  // Crear semáforo y tarea para control del actuador
  actMutex = xSemaphoreCreateMutex();
  xTaskCreate(ControlActuador, "ControlActuador", 4096, NULL, 1, NULL);
}


//=============================================================================
// 🔁 Bucle principal (loop)
//=============================================================================
void loop() {
  // No se usa, todo el trabajo lo hacen tareas FreeRTOS
}
