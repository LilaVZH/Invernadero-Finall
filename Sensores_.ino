#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <DHT.h>

// Pines y configuración
#define DHTPIN 25  // Sensor DHT11
#define LUZPIN 32  // Sensor de luz
#define MQPIN 33  // Sensor MQ-135
#define HWPIN 34  // Sensor HW-080
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

// Dirección MAC del receptor (Nodo 2)
uint8_t peerAddress[] = {0xEC, 0x64, 0xC9, 0x5E, 0x03, 0xB8};

// Estructura de datos a enviar
typedef struct struct_envio {
  float temperatura;
  float humedad;
  int luz;
  float calidad_aire;
  int tierra_h;
} struct_envio;

struct_envio datosEnviar;
esp_now_peer_info_t peerInfo;

// Variables globales protegidas por mutex
float temperatura = 0.0;
float humedad = 0.0;
int luz = 0;
int calidad_aire = 0;
int tierra_h = 0;

// Rango de temperaturas posibles en Popayán
float tempMin = 12.0, tempMax = 30.0;

SemaphoreHandle_t tempMutex, humMutex, luzMutex, aireMutex, tierraMutex;

// Callback de envío
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("📤 Estado de envío: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Éxito" : "Fallo");
}

// Tarea que lee sensores cada 3 segundos
void LecturaSensores(void *parameter) {

//Creamos arreglos para cada una de las valiables en donde guardamos las lecturas válidas en caso de que los sensores fallen
static float tempArray[3] = {0}, humArray[3] = {0}, luzArray[3] = {0}, aireArray[3] = {0}, tierraArray[3] = {0};
static int index = 0; 
int validReadings = 0, failedReadings = 0;

  while (true) {
    // Lectura de los sensores 
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    int luzVal = analogRead(LUZPIN);
    float aire = analogRead(MQPIN);
    int tierra = analogRead(HWPIN);

    // Validamos si las lecturas del DHT son válidas
    bool valid = !isnan(temp) && !isnan(hum);

    //Guardamos las lecturas válidas de los sensores en los arreglos en caso de falla
    if (valid) {
      tempArray[index] = temp;
      humArray[index] = hum;
      luzArray[index] = luz;
      aireArray[index] = aire;
      tierraArray[index] = tierra;

      // Hacemos que el index de los vectores solo lleguen hasta 3 
      index = (index +1) %3;
      if (validReadings < 3) validReadings++;
      failedReadings = 0;

      // Actualizamos el valor de las variables protegidas por el mutex
      // Tempertaura
      if (xSemaphoreTake(tempMutex, portMAX_DELAY)){
        if (temp >= tempMin && temp <= tempMax)
        temperatura = temp;
        xSemaphoreGive(tempMutex);
      }
      // Humedad
      if (xSemaphoreTake(humMutex, portMAX_DELAY)){
        humedad = hum;
        xSemaphoreGive(humMutex);
      }
      // Luz
      if (xSemaphoreTake(luzMutex, portMAX_DELAY)){
        luz = luzVal;
        xSemaphoreGive(luzMutex);
      }
      // Aire
      if (xSemaphoreTake(aireMutex, portMAX_DELAY)){
        calidad_aire = aire;
        xSemaphoreGive(aireMutex);
      }
      // Humedad de la tierra 
      if (xSemaphoreTake(tierraMutex, portMAX_DELAY)){
        tierra_h = map(tierra, 0, 4095, 100, 0); // 0% = totalmente seco, 100% = totalmente mojado
        xSemaphoreGive(tierraMutex);
      }
      //Imprime los datos en la consola solo para las pruebas 
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
     
    }

    // En caso de falla sacamos el promedio de los valores válidos guardados en el vector 
    else if (validReadings == 3) {
      temperatura = (tempArray[0] + tempArray[1] + tempArray[2]) / 3.0;
      humedad =  (humArray[0] + humArray[1] + humArray[2]) / 3.0;
      luz = (luzArray[0] + luzArray[1] + luzArray[2]) / 3.0;
      calidad_aire = (aireArray[0] + aireArray[1] + aireArray[2]) / 3.0;
      tierra_h = (tierraArray[0] + tierraArray[1] + tierraArray[2]) /3.0;
      failedReadings++;
    }
      vTaskDelay(pdMS_TO_TICKS(3000));  // 3 segundos
  }
}

// Tarea que envía los datos una vez y entra en deep sleep por 12 segundos
void EnvioDatosSen(void *parameter) {
  vTaskDelay(pdMS_TO_TICKS(25000));  // Espera inicial de 25 segundos

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

  if (xSemaphoreTake(tierraMutex,pdMS_TO_TICKS(100))) {
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

  vTaskDelay(pdMS_TO_TICKS(200));  // pequeña espera para asegurar envío

  Serial.println("😴 Entrando en Deep Sleep por 12 segundos...");
  esp_sleep_enable_timer_wakeup(12 * 1000000ULL);  // 12 segundos
  esp_deep_sleep_start();
}

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

  // Crear mutex para proteger acceso a variables compartidas
  tempMutex = xSemaphoreCreateMutex();
  humMutex = xSemaphoreCreateMutex();
  luzMutex = xSemaphoreCreateMutex();
  aireMutex = xSemaphoreCreateMutex();
  tierraMutex = xSemaphoreCreateMutex();

  // Crear tareas
  xTaskCreatePinnedToCore(LecturaSensores, "LecturaSensores", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(EnvioDatosSen, "EnvioDatosSen", 4096, NULL, 1, NULL, 1);
}

void loop() {}
