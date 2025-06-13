/**
 * @file sensor_envio.ino
 * @brief Lectura de sensores ambientales y envío de datos mediante ESP-NOW.
 * 
 * Este programa utiliza un ESP32 para leer datos de sensores (temperatura, humedad,
 * luz, calidad del aire y humedad del suelo) y enviarlos a otro dispositivo mediante ESP-NOW.
 * Se emplea multitarea con FreeRTOS, protección de variables con mutex y modo deep sleep.
 */

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <DHT.h>

/// Definición de pines y tipo de sensor
#define DHTPIN 25  ///< Pin del sensor DHT11
#define LUZPIN 32  ///< Pin del sensor de luz
#define MQPIN 33   ///< Pin del sensor MQ-135
#define HWPIN 34   ///< Pin del sensor de humedad del suelo
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

/// Dirección MAC del nodo receptor
uint8_t peerAddress[] = {0xEC, 0x64, 0xC9, 0x5E, 0x03, 0xB8};

/**
 * @struct struct_envio
 * @brief Estructura para almacenar y enviar los datos leídos.
 */
typedef struct struct_envio {
  float temperatura;
  float humedad;
  int luz;
  float calidad_aire;
  int tierra_h;
} struct_envio;

struct_envio datosEnviar;
esp_now_peer_info_t peerInfo;

/// Variables globales protegidas por mutex
float temperatura = 0.0;
float humedad = 0.0;
int luz = 0;
int calidad_aire = 0;
int tierra_h = 0;

/// Rango de temperaturas válidas para Popayán
float tempMin = 12.0, tempMax = 30.0;

/// Mutex para proteger acceso a variables compartidas
SemaphoreHandle_t tempMutex, humMutex, luzMutex, aireMutex, tierraMutex;

/**
 * @brief Callback que indica el estado del envío ESP-NOW.
 * @param mac_addr Dirección MAC del receptor.
 * @param status Estado del envío.
 */
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("📤 Estado de envío: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Éxito" : "Fallo");
}

/**
 * @brief Tarea que lee sensores cada 3 segundos y actualiza las variables protegidas por mutex.
 * 
 * Usa buffers circulares para almacenar los últimos valores válidos en caso de lecturas erróneas.
 * Imprime los datos por consola para verificación durante pruebas.
 * 
 * @param parameter No se utiliza.
 */
void LecturaSensores(void *parameter) {
  static float tempArray[3] = {0}, humArray[3] = {0}, luzArray[3] = {0}, aireArray[3] = {0}, tierraArray[3] = {0};
  static int index = 0;
  int validReadings = 0, failedReadings = 0;

  while (true) {
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    int luzVal = analogRead(LUZPIN);
    float aire = analogRead(MQPIN);
    int tierra = analogRead(HWPIN);

    bool valid = !isnan(temp) && !isnan(hum);

    if (valid) {
      tempArray[index] = temp;
      humArray[index] = hum;
      luzArray[index] = luz;
      aireArray[index] = aire;
      tierraArray[index] = tierra;

      index = (index + 1) % 3;
      if (validReadings < 3) validReadings++;
      failedReadings = 0;

      if (xSemaphoreTake(tempMutex, portMAX_DELAY)) {
        if (temp >= tempMin && temp <= tempMax)
          temperatura = temp;
        xSemaphoreGive(tempMutex);
      }
      if (xSemaphoreTake(humMutex, portMAX_DELAY)) {
        humedad = hum;
        xSemaphoreGive(humMutex);
      }
      if (xSemaphoreTake(luzMutex, portMAX_DELAY)) {
        luz = luzVal;
        xSemaphoreGive(luzMutex);
      }
      if (xSemaphoreTake(aireMutex, portMAX_DELAY)) {
        calidad_aire = aire;
        xSemaphoreGive(aireMutex);
      }
      if (xSemaphoreTake(tierraMutex, portMAX_DELAY)) {
        tierra_h = map(tierra, 0, 4095, 100, 0);
        xSemaphoreGive(tierraMutex);
      }

      Serial.print("🌡Temp: ");
      Serial.print(temp);
      Serial.print("°C|💧Hum: ");
      Serial.print(hum);
      Serial.print("% |💡Luz: ");
      Serial.print(luzVal);
      Serial.print("|🌫️ Aire: ");
      Serial.println(aire);
      Serial.println("|🌱 HTierra: ");
      Serial.print(tierra_h);
      Serial.println(" %");
    } else if (validReadings == 3) {
      temperatura = (tempArray[0] + tempArray[1] + tempArray[2]) / 3.0;
      humedad = (humArray[0] + humArray[1] + humArray[2]) / 3.0;
      luz = (luzArray[0] + luzArray[1] + luzArray[2]) / 3.0;
      calidad_aire = (aireArray[0] + aireArray[1] + aireArray[2]) / 3.0;
      tierra_h = (tierraArray[0] + tierraArray[1] + tierraArray[2]) / 3.0;
      failedReadings++;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

/**
 * @brief Tarea que toma los datos actualizados, los envía por ESP-NOW y entra en deep sleep.
 * 
 * @param parameter No se utiliza.
 */
void EnvioDatosSen(void *parameter) {
  vTaskDelay(pdMS_TO_TICKS(25000));

  if (xSemaphoreTake(tempMutex, pdMS_TO_TICKS(100))) {
    datosEnviar.temperatura = temperatura;
    xSemaphoreGive(tempMutex);
  }
  if (xSemaphoreTake(humMutex, pdMS_TO_TICKS(100))) {
    datosEnviar.humedad = humedad;
    xSemaphoreGive(humMutex);
  }
  if (xSemaphoreTake(luzMutex, pdMS_TO_TICKS(100))) {
    datosEnviar.luz = luz;
    xSemaphoreGive(luzMutex);
  }
  if (xSemaphoreTake(aireMutex, pdMS_TO_TICKS(100))) {
    datosEnviar.calidad_aire = calidad_aire;
    xSemaphoreGive(aireMutex);
  }
  if (xSemaphoreTake(tierraMutex, pdMS_TO_TICKS(100))) {
    datosEnviar.tierra_h = tierra_h;
    xSemaphoreGive(tierraMutex);
  }

  esp_err_t result = esp_now_send(peerAddress, (uint8_t *)&datosEnviar, sizeof(datosEnviar));
  if (result == ESP_OK) {
    Serial.println("✅ Datos enviados correctamente");
  } else {
    Serial.print("❌ Error al enviar datos: ");
    Serial.println(result);
  }

  vTaskDelay(pdMS_TO_TICKS(200));

  Serial.println("😴 Entrando en Deep Sleep por 12 segundos...");
  esp_sleep_enable_timer_wakeup(12 * 1000000ULL);
  esp_deep_sleep_start();
}

/**
 * @brief Configuración inicial del sistema: sensores, Wi-Fi, ESP-NOW y tareas FreeRTOS.
 */
void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(LUZPIN, INPUT);
  pinMode(MQPIN, INPUT);
  pinMode(HWPIN, INPUT);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(8, WIFI_SECOND_CHAN_NONE);
  Serial.print("🆔 MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error al iniciar ESP-NOW");
    return;
  }

  esp_now_register_send_cb(OnDataSent);

  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 8;
  peerInfo.encrypt = false;

  if (!esp_now_add_peer(&peerInfo)) {
    Serial.println("✅ Peer agregado correctamente");
  } else {
    Serial.println("❌ Error al agregar peer");
  }

  tempMutex = xSemaphoreCreateMutex();
  humMutex = xSemaphoreCreateMutex();
  luzMutex = xSemaphoreCreateMutex();
  aireMutex = xSemaphoreCreateMutex();
  tierraMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(LecturaSensores, "LecturaSensores", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(EnvioDatosSen, "EnvioDatosSen", 4096, NULL, 1, NULL, 1);
}

/**
 * @brief Bucle principal (vacío, ya que se usa multitarea).
 */
void loop() {}
