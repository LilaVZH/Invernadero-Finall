/**
 * @file sensor_logger_esp32.ino
 * @brief Sistema de recolección, almacenamiento y monitoreo de datos ambientales con ESP32.
 *
 * Este programa realiza la lectura de sensores desde nodos remotos mediante ESP-NOW,
 * sincroniza la hora con un servidor NTP, almacena los datos en una tarjeta SD,
 * y envía alertas vía Telegram cuando los umbrales definidos son superados.
 *
 * @author TuNombre
 * @date 2025
 */

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"

/** Pines I2C para el RTC DS1307 */
#define SDA_PIN 21
#define SCL_PIN 22

/** Pines SPI para la tarjeta SD */
#define SD_CS_PIN 5
#define SPI_MOSI 23
#define SPI_MISO 19
#define SPI_SCK 18

/** Configuración Wi-Fi */
#define WIFI_SSID "RUIZ"
#define WIFI_PASSWORD "9705426613"
#define WIFI_CHANNEL_ESPNOW 8
#define WIFI_CHANNEL_WIFI 6

/** Configuración de Telegram */
#define BOT_TOKEN "7295924287:AAEw7dNGkatAhy12nt7EyufmRW50N5U_wS4"
const String CHAT_ID = "1232016244";
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

/** Datos del servidor NTP */
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -18000;
const int daylightOffset_sec = 0;

/** Estructura para almacenar datos de sensores */
typedef struct struct_datosSensores {
  float temperatura;
  float humedad;
  int luz;
  float calidad_aire;
  int tierra_h;
} struct_datosSensores;
struct_datosSensores datos;

/** Estructura para mensajes de alerta */
typedef struct {
  const char* tipo;
  float valor;
} MensajeAlerta;

/** Estructura para comandos al actuador */
typedef struct __attribute__((packed)) {
  uint8_t led;
} ComandoAct;

/** Variables del RTC y colas */
RTC_DS1307 rtc;
QueueHandle_t colaDatos;
QueueHandle_t colaComparar;
QueueHandle_t queueEnvio;
QueueHandle_t queueAlertas;

/** Umbrales definidos */
volatile int UMBRAL_LUZ = 1500;
volatile float UMBRAL_HUMEDAD = 70.0;
volatile float UMBRAL_TEMP = 25.0;
volatile float UMBRAL_AIRE = 1200;
volatile float UMBRAL_TIEMAX = 70.0;
volatile float UMBRAL_TIEMIN = 50.0;
int hora_InicioLuz = 6;
int hora_FinLuz = 18;

int ID_NODO = 1;
int estado = 3;
int RSSI = -70;

/**
 * @brief Dirección MAC del nodo destino (actuador)
 */
uint8_t macDestino[] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};

/**
 * @brief Registra un peer para comunicación ESP-NOW
 */
void registrarPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, macDestino, 6);
  peerInfo.channel = WIFI_CHANNEL_ESPNOW;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(macDestino)) {
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      Serial.println("✅ Peer registrado correctamente");
    } else {
      Serial.println("❌ Error al registrar peer");
    }
  }
}

/**
 * @brief Activa la Wi-Fi en canal 6
 * @return true si la conexión fue exitosa, false en caso contrario
 */
bool activarWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL_WIFI, WIFI_SECOND_CHAN_NONE);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  for (int i = 0; i < 15 && WiFi.status() != WL_CONNECTED; i++) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  return WiFi.status() == WL_CONNECTED;
}

/**
 * @brief Cambia el modo Wi-Fi a ESP-NOW en canal 8
 */
void desactivarWiFiYActivarESPNow() {
  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(100));
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error al reiniciar ESP-NOW");
  } else {
    esp_now_register_recv_cb(OnDataRecv);
    Serial.println("✅ ESP-NOW reactivado en canal 8");
  }
}

/**
 * @brief Sincroniza el RTC con un servidor NTP
 */
void sincronizarRTC() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("⌛ Esperando hora por NTP...");

  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println("❌ Fallo al obtener hora NTP");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  rtc.adjust(DateTime(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  ));

  Serial.println("✅ RTC actualizado");
}

/**
 * @brief Tarea de sincronización del RTC
 */
void SincronizarRTC(void* parameter) {
  if (activarWiFi()) {
    Serial.println("\n✅ Wi-Fi conectado");
    sincronizarRTC();
  } else {
    Serial.println("\n❌ Error: No se pudo conectar a Wi-Fi");
  }
  desactivarWiFiYActivarESPNow();
  vTaskDelete(NULL);
}

/**
 * @brief Obtiene el timestamp actual desde el RTC
 * @return Cadena de tiempo con formato YYYY-MM-DD HH:MM:SS
 */
String getTimestamp() {
  DateTime now = rtc.now();
  char timestamp[25];
  snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  return String(timestamp);
}

/**
 * @brief Callback para recepción de datos vía ESP-NOW
 */
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == sizeof(struct_datosSensores)) {
    struct_datosSensores recibidos;
    memcpy(&recibidos, data, sizeof(struct_datosSensores));
    xQueueSend(colaDatos, &recibidos, portMAX_DELAY);
    xQueueSend(colaComparar, &recibidos, portMAX_DELAY);
  }
}

/**
 * @brief Crea carpeta y guarda datos en archivo SD
 * @param folder Carpeta destino
 * @param filename Nombre del archivo
 * @param data Cadena de datos a almacenar
 */
void appendDataToFile(String folder, String filename, String data) {
  String path = "/" + folder;
  if (!SD.exists(path)) {
    SD.mkdir(path);
  }
  path += "/" + filename;

  bool nuevoArchivo = !SD.exists(path);
  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("❌ Error al abrir archivo para escribir");
    return;
  }
  if (nuevoArchivo) {
    file.println("timestamp,ID_NODO,RSSI,temperatura,humedad,luz,calidad_aire,estado");
  }
  file.println(data);
  file.close();
  Serial.println("✅ Datos guardados en: " + path);
}
