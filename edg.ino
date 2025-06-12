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

// Pines I2C para RTC DS1307
#define SDA_PIN 21
#define SCL_PIN 22

// Pines SPI para SD
#define SD_CS_PIN 5   
#define SPI_MOSI 23
#define SPI_MISO 19
#define SPI_SCK  18

// Configuración Wi-Fi
#define WIFI_SSID "RUIZ"
#define WIFI_PASSWORD "9705426613"
#define WIFI_CHANNEL_ESPNOW 8
#define WIFI_CHANNEL_WIFI 6

// Telegram
#define BOT_TOKEN "7295924287:AAEw7dNGkatAhy12nt7EyufmRW50N5U_wS4"
const String CHAT_ID = "1232016244";
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// Datos NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -18000;
const int daylightOffset_sec = 0;

// Estructuras
typedef struct struct_datosSensores {
  float temperatura;
  float humedad;
  int luz;
  float calidad_aire;
  int tierra_h;
} struct_datosSensores;
struct_datosSensores datos;

typedef struct {
  const char* tipo;
  float valor;
} MensajeAlerta;

typedef struct __attribute__((packed)) {
  uint8_t led;
} ComandoAct;


//  RTC y colas
RTC_DS1307 rtc;
QueueHandle_t colaDatos;
QueueHandle_t colaComparar;
QueueHandle_t queueEnvio;
QueueHandle_t queueAlertas;

// Umbrales
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

//--------------------------------------------------------------------------------
// Dirección MAC del nodo destino (ej. actuador)
uint8_t macDestino[] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};
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

//------------------------------------------------------------------------------------------------------------------------
// Activar Wi-Fi en canal 6
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

//--------------------------------------------------------------------------------------------------------------------------
// Volver a modo ESP-NOW en canal 8
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

//-----------------------------------------------------------------------------------------------------------------------
// Sincronización NTP
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

//----------------------------------------------------------------------------------------------------------------------
// Tarea RTC
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

//-----------------------------------------------------------------------------------------------------------------------
// Timestamp desde RTC
String getTimestamp() {
  DateTime now = rtc.now();
  char timestamp[25];
  snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  return String(timestamp);
}

//------------------------------------------------------------------------------------------------------------------------
// Callback ESP-NOW
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == sizeof(struct_datosSensores)) {
    struct_datosSensores recibidos;
    memcpy(&recibidos, data, sizeof(struct_datosSensores));
    xQueueSend(colaDatos, &recibidos, portMAX_DELAY);
    xQueueSend(colaComparar, &recibidos, portMAX_DELAY);
  }
}

//------------------------------------------------------------------------------------------------------------------------
// Crear carpeta y escribir en archivo
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

//-----------------------------------------------------------------------------------------------------------------------
// Tarea para crear archivo y escribir datos
void GuardarSD(void *parameter) {
  struct_datosSensores datos;

  while (true) {
    xQueueReceive(colaDatos, &datos, portMAX_DELAY);

    String timestamp = getTimestamp();
    String fecha = timestamp.substring(0,10); // formato: "AAAA-MM-DD"
    String hora = timestamp.substring(11,13) + "-" + timestamp.substring(14,16); // formato: "HH-MM"

    String nombreCarpeta = "/" + fecha;
    String nombreArchivo = "/" + fecha + "/" + hora + ".txt";

    
      if (!SD.exists(nombreCarpeta)) {
        SD.mkdir(nombreCarpeta);
      }

      // Verificar si el archivo existe (para saber si hay que agregar la cabecera)
      bool nuevoArchivo = !SD.exists(nombreArchivo);

      File archivo = SD.open(nombreArchivo, FILE_APPEND);
      if (archivo) {
        archivo.print(timestamp); archivo.print(",");
        archivo.print(datos.temperatura); archivo.print(",");
        archivo.print(datos.humedad); archivo.print(",");
        archivo.print(datos.luz); archivo.print(",");
        archivo.print(datos.calidad_aire); archivo.print(",");
        archivo.print(datos.tierra_h); archivo.print(",");
        archivo.close();
        Serial.println("✅ Datos guardados en SD");
      } else {
        Serial.println("❌ Error al abrir el archivo en la SD");
      }

    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}

//----------------------------------------------------------------------------------------------------------------------
// Comparar datos con umbrales
void CompararDatos(void *parameter) {
  struct_datosSensores datos;
  MensajeAlerta alerta;
  ComandoAct actuador;

  while (true) {
    if (xQueueReceive(colaComparar, &datos, portMAX_DELAY) == pdPASS) {
      int horaActual = rtc.now().hour();

      // Inicializamos los actuadores en 0
      actuador.led = 0;

      if (horaActual >= hora_InicioLuz && horaActual < hora_FinLuz) {
      // Tiempo en que debe haber luz
        if (datos.luz < UMBRAL_LUZ) {
          // Hay poca luz cuando debería haber mucha → ALERTA
          alerta = {"luz", (float)datos.luz};
          xQueueSend(queueAlertas, &alerta, pdMS_TO_TICKS(1000));
          actuador.led = 1; // Se enciende el led si hay poca luz en el horario en el que debe haber suficiente luz
        }
      } else { // Tiempo en que debe estar oscuro
        if (datos.luz > UMBRAL_LUZ) {
          // Hay luz cuando debería estar oscuro → ALERTA
          alerta = {"luz", (float)datos.luz};
          xQueueSend(queueAlertas, &alerta, pdMS_TO_TICKS(1000));
        }
      }
      if (datos.humedad > UMBRAL_HUMEDAD) {
        alerta = {"humedad", datos.humedad};
        xQueueSend(queueAlertas, &alerta, pdMS_TO_TICKS(1000));
      }
      if (datos.temperatura > UMBRAL_TEMP) {
        alerta = {"temperatura", datos.temperatura};
        xQueueSend(queueAlertas, &alerta, pdMS_TO_TICKS(1000));
      }
      if (datos.calidad_aire > UMBRAL_AIRE) {
        alerta = {"calidad_aire", datos.calidad_aire};
        xQueueSend(queueAlertas, &alerta, pdMS_TO_TICKS(1000));
      }
      if (datos.tierra_h < UMBRAL_TIEMIN || datos.tierra_h > UMBRAL_TIEMAX) {
        alerta = {"tierra_h", datos.tierra_h};
        xQueueSend(queueAlertas, &alerta, pdMS_TO_TICKS(1000));
      }
      xQueueSend(queueEnvio, &actuador, pdMS_TO_TICKS(3000));
    }
  }
}

//---------------------------------------------------------------------------------
//Enviar comandos a los Actuadores nodo 3
void EnviarAct (void *parameter) {
  ComandoAct actuador;

  while (true) {
    if (xQueueReceive(queueEnvio, &actuador, portMAX_DELAY) == pdPASS) {
      esp_err_t result = esp_now_send(macDestino, (uint8_t *)&actuador, sizeof(actuador));
      if (result == ESP_OK) {
        Serial.println("📤 Comando enviado por ESP-NOW");
      } else {
        Serial.println("❌ Error al enviar comando por ESP-NOW");
      }
    }
  }
}

//--------------------------------------------------------------------------------
// Enviar alerta por Telegram
void enviarAlertaTelegram(const String &mensaje) {
  esp_now_deinit();
  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(1000));

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL_WIFI, WIFI_SECOND_CHAN_NONE);
  Serial.println("🔁 Conectando a WiFi para enviar alerta...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - inicio < 10000)) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n📶 WiFi conectado. Enviando mensaje...");
    client.setInsecure();
    if (bot.sendMessage(CHAT_ID, mensaje, "")) {
      Serial.println("📤 Mensaje enviado");
    } else {
      Serial.println("❌ Error al enviar mensaje");
    }
  } else {
    Serial.println("\n🚫 Falló la conexión WiFi");
  }

  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(500));
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error al reiniciar ESP-NOW");
  } else {
    esp_now_register_recv_cb(OnDataRecv);
    Serial.println("✅ ESP-NOW reactivado tras envío");
  }
}

// Comandos de Telegram
void ComandoTelegram() {
  esp_now_deinit();
  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(1000));
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL_WIFI, WIFI_SECOND_CHAN_NONE);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (WiFi.status() == WL_CONNECTED) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      for (int i = 0; i < numNewMessages; i++) {
        String chat_id = bot.messages[i].chat_id;
        String text = bot.messages[i].text;
        String respuesta;

        if (text.startsWith("/t ")) {
          UMBRAL_TEMP = text.substring(3).toFloat();
          respuesta = "🌡️ Umbral de temperatura actualizado";
        } else if (text.startsWith("/h ")) {
          UMBRAL_HUMEDAD = text.substring(3).toFloat();
          respuesta = "💧 Umbral de humedad actualizado";
        } else if (text.startsWith("/l ")) {
          UMBRAL_LUZ = text.substring(3).toInt();
          respuesta = "💡 Umbral de luz actualizado";
        } else if (text.startsWith("/a ")) {
          UMBRAL_AIRE = text.substring(3).toFloat();
          respuesta = "🫁 Umbral de calidad del aire actualizado";
        } else if (text.startsWith("/htx ")) {
          UMBRAL_TIEMAX = text.substring(5).toFloat();
          respuesta = "🌱 Umbral de tierra MAX actualizado";
        } else if (text.startsWith("/htn ")) {
          UMBRAL_TIEMIN = text.substring(5).toFloat();
          respuesta = "🌱 Umbral de tierra MIN actualizado";
        } else if (text.startsWith("/um")) {
          respuesta = "📊 *Umbrales actuales:*\n";
          respuesta += "🌡️ Temp: " + String(UMBRAL_TEMP) + " °C\n";
          respuesta += "💧 Humedad: " + String(UMBRAL_HUMEDAD) + " %\n";
          respuesta += "💡 Luz: " + String(UMBRAL_LUZ) + "\n";
          respuesta += "🫁 Aire: " + String(UMBRAL_AIRE) + " ppm\n";
          respuesta += "🌱 Humedad de la tierra: " + String(UMBRAL_TIEMIN) + " % - " + String(UMBRAL_TIEMAX) + " %";
        } else if (text.startsWith("/op")) {
          respuesta = "🤖 *Opciones:*\n";
          respuesta += "/t <valor>   → Cambiar umbral de temperatura\n";
          respuesta += "/h <valor>   → Cambiar umbral de humedad\n";
          respuesta += "/l <valor>   → Cambiar umbral de luz\n";
          respuesta += "/a <valor>   → Cambiar umbral de calidad del aire\n";
          respuesta += "/htn <valor> → Cambiar humedad de tierra mínima\n";
          respuesta += "/htx <valor> → Cambiar humedad de tierra máxima\n";
          respuesta += "/um         → Mostrar umbrales actuales";
        } else {
          respuesta = "❓ Comando no reconocido. Usa /op para ver opciones";
        }

        bot.sendMessage(chat_id, respuesta, "");
      }
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
  }

  // Restaurar ESP-NOW
  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(500));
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
    Serial.println("✅ ESP-NOW restaurado tras comandos Telegram");
  } else {
    Serial.println("❌ Error al restaurar ESP-NOW");
  }
}

// Tarea principal de Telegram
void Telegram(void* parameter) {
  MensajeAlerta alerta;

  while (true) {
    ComandoTelegram();

    if (xQueueReceive(queueAlertas, &alerta, portMAX_DELAY) == pdPASS) {
String mensaje;

      if (strcmp(alerta.tipo, "temperatura") == 0) {
        mensaje = "🚨 Alerta de temperatura 🌡️:\n";
        mensaje += "Valor actual: " + String(alerta.valor, 2) + "°C\n";
        mensaje += "Umbral: " + String(UMBRAL_TEMP, 2) + "°C";
      } else if (strcmp(alerta.tipo, "humedad") == 0) {
        mensaje = "🚨 Alerta de humedad 💧:\n";
        mensaje += "Valor actual: " + String(alerta.valor, 2) + "%\n";
        mensaje += "Umbral: " + String(UMBRAL_HUMEDAD, 2) + "%";
      } else if (strcmp(alerta.tipo, "luz") == 0) {
        mensaje = "🚨 Alerta de luz baja 💡:\n";
        mensaje += "Valor actual: " + String(alerta.valor, 2) + "\n";
        mensaje += "Umbral: " + String(UMBRAL_LUZ);
      } else if (strcmp(alerta.tipo, "calidad_aire") == 0) {
        mensaje = "🚨 Alerta de calidad del aire 🌫:\n";
        mensaje += "Valor actual: " + String(alerta.valor, 2) + " ppm\n";
        mensaje += "Umbral: " + String(UMBRAL_AIRE, 2) + " ppm";
      } else if (strcmp(alerta.tipo, "tierra_h") == 0) {
        mensaje = "🚨 Alerta de humedad de tierra 🌱:\n";
        mensaje += "Valor actual: " + String(alerta.valor, 2) + " %\n";
        mensaje += "Umbral mínimo: " + String(UMBRAL_TIEMIN, 2) + " %\n";
        mensaje += "Umbral máximo: " + String(UMBRAL_TIEMAX, 2) + " %";
      }
      enviarAlertaTelegram(mensaje);
    }
  }
}

//---------------------------------------------------------------------------------
// Setup
void setup() {
  
  Serial.begin(115200);

  // Inicializar SD
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS_PIN);
  if (!SD.begin(5)) {
    Serial.println("❌ Error al inicializar la tarjeta SD");
  
    return;
  }
  Serial.println("✅ Tarjeta SD inicializada");

   // RTC
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!rtc.begin()) {
    Serial.println("❌ RTC no detectado");
    while (true);
  }

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL_ESPNOW, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error ESP-NOW");
    while (true);
  }

  esp_now_register_recv_cb(OnDataRecv);

  // 👉 Verificación de conexión Wi-Fi
  if (activarWiFi()) {
    Serial.println("✅ Conectado a Wi-Fi correctamente al inicio");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("❌ No se pudo conectar a Wi-Fi al inicio");
  }
  desactivarWiFiYActivarESPNow();  // Restaurar ESP-NOW

  colaDatos = xQueueCreate(10, sizeof(struct_datosSensores));
  colaComparar = xQueueCreate(10, sizeof(struct_datosSensores));
  queueAlertas = xQueueCreate(10, sizeof(MensajeAlerta));
  queueEnvio = xQueueCreate(5, sizeof(ComandoAct));
  
  xTaskCreate(SincronizarRTC, "RTC", 4096, NULL, 1, NULL);
  xTaskCreate(GuardarSD, "GuardarSD", 4096, NULL, 1, NULL);
  xTaskCreate(CompararDatos, "Comparador", 4096, NULL, 1, NULL);
  xTaskCreate(Telegram, "Telegram", 6144, NULL, 1, NULL);
  xTaskCreate(EnviarAct, "EnviarAct", 4096, NULL, 1, NULL);
}

void loop() {
  // Vacío
}