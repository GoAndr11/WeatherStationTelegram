#ifdef ESP32
#include "WiFi.h"
#include "EEPROM.h"
#else
#include "ESP8266WiFi.h"
#include "EEPROM.h"
#endif

#include "WiFiClientSecure.h"
#include "UniversalTelegramBot.h"
#include "ArduinoJson.h"
#include "Adafruit_BME280.h"
#include "Adafruit_Sensor.h"

// Вкажіть ваші дані Wi-Fi
const char* ssid = "SSID";
const char* password = "Password";
// Використовуйте @myidbot для отримання ID користувача
#define CHAT_ID "userChat_ID"
// Вкажіть токен бота
#define BOTtoken "You_bot_token"

#ifdef ESP8266
X509List cert(TELEGRAM_CERTIFICATE_ROOT);
#endif

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// Перевіряє нові повідомлення кожну секунду
int botRequestDelay = 1000;
unsigned long lastTimeBotRan;

// Підключення до датчика BME280
Adafruit_BME280 bme;

struct TempData {
  float maxTemp;
  float minTemp;
  char maxTime[20];
  char minTime[20];
  float temps[30];  // Для зберігання температур за 30 днів
  unsigned long timestamps[30];  // Часові мітки для 30 днів
  int dayIndex;  // Індекс для циклічного оновлення температур
  bool isInitialized;  // Прапор для перевірки ініціалізації
};

// Дані температури, вологості та тиску
TempData daily, weekly, monthly;

const int EEPROM_SIZE = sizeof(TempData) * 3;
unsigned long lastTempCheck = 0;
const int tempCheckInterval = 10000;  // Перевірка температури кожні 10 секунд
unsigned long lastEEPROMsave = 0;
const int eepromSaveInterval = 3600000;  // Збереження в EEPROM раз на годину (3600 секунд)

void saveTempData(int address, TempData &data) {
  EEPROM.put(address, data);
  EEPROM.commit();
}

void loadTempData(int address, TempData &data) {
  EEPROM.get(address, data);
  if (!data.isInitialized || isnan(data.maxTemp) || isnan(data.minTemp)) {
    // Ініціалізація за замовчуванням
    data.maxTemp = -1000.0;
    data.minTemp = 1000.0;
    strcpy(data.maxTime, "");
    strcpy(data.minTime, "");
    data.dayIndex = 0;
    for (int i = 0; i < 30; i++) {
      data.temps[i] = 0.0;
      data.timestamps[i] = 0;
    }
    data.isInitialized = true;
    saveTempData(address, data);  // Зберігаємо ініціалізовані дані
  }
}

String getTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "";
  }
  char timeString[20];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeString);
}

void updateTempData(TempData &data, int address) {
  float temperature = bme.readTemperature();
  String currentTime = getTime();
  bool shouldSave = false;

  // Оновлення температури за 30 днів
  data.temps[data.dayIndex] = temperature;
  data.timestamps[data.dayIndex] = millis();  // Поточний час
  data.dayIndex = (data.dayIndex + 1) % 30;  // Перехід до наступного дня (циклічно)

  // Оновлення максимальних та мінімальних температур
  if (temperature > data.maxTemp) {
    data.maxTemp = temperature;
    currentTime.toCharArray(data.maxTime, 20);
    shouldSave = true;  // Зміна максимуму - зберігаємо
  }
  if (temperature < data.minTemp) {
    data.minTemp = temperature;
    currentTime.toCharArray(data.minTime, 20);
    shouldSave = true;  // Зміна мінімуму - зберігаємо
  }

  // Збереження в EEPROM лише при зміні або раз на годину
  if (shouldSave || (millis() - lastEEPROMsave > eepromSaveInterval)) {
    saveTempData(address, data);
    lastEEPROMsave = millis();
  }
}

String getReadings() {
  float temperature = bme.readTemperature();
  float humidity = bme.readHumidity();
  float pressure = bme.readPressure() / 100.0F; // Перетворення тиску в гПа

  // Якщо температура <= 0, додаємо сніжинку ❄️
  String temperatureEmoji = "🌡️ Температура повітря: *" + String(temperature) + "* ºC";
  if (temperature <= 0) {
    temperatureEmoji += " ❄️";  // Додаємо сніжинку
  }
  // Форматування повідомлення
  String message = temperatureEmoji + "\n";
  message += "💧 Вологість повітря: *" + String(humidity) + "* %\n";
  message += "🌬️ Атмосферний тиск: *" + String(pressure) + "* гПа (" + String(pressure * 0.750062) + " мм рт. ст.)\n";

  return message;
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    if (text == "/start") {
      String welcome = "👋 Вітаю, " + from_name + ".\n";
      welcome += "Використовуйте наступні команди для отримання даних:\n\n";
      welcome += "🌡️ /temp - поточні показники температури, вологості та тиску повітря\n";
      welcome += "📊 /tempd - найвища та найнижча температура повітря за день\n";
      welcome += "📊 /tempw - найвища та найнижча температура повітря за останні 7 днів\n";
      welcome += "📊 /tempm - найвища та найнижча температура повітря за останні 30 днів\n";
      
      // Додаємо команди очищення тільки для адміністратора
      if (chat_id == CHAT_ID) {
        welcome += "🧹 /cleartempd - очистити дані за день (адміністратор)\n";
        welcome += "🧹 /cleartempw - очистити дані за тиждень (адміністратор)\n";
        welcome += "🧹 /cleartempm - очистити дані за місяць (адміністратор)";
      }
      
      bot.sendMessage(chat_id, welcome, "");
    }

    if (text == "/temp") {
      String readings = getReadings();
      bot.sendMessage(chat_id, "📊 Показники станом на зараз:\n" + readings, "Markdown");
    }

    if (text == "/tempd") {
      String response = "⬆️ Найвища зафіксована температура за день: *" + String(daily.maxTemp) + "* ºC\n";
      response += "📌 Зафіксовано: " + String(daily.maxTime) + "\n\n";
      response += "⬇️ Найнижча зафіксована температура за день: *" + String(daily.minTemp) + "* ºC\n";
      response += "📌 Зафіксовано: " + String(daily.minTime);
      bot.sendMessage(chat_id, response, "Markdown");
    }

    if (text == "/tempw") {
      String response = "⬆️ Найвища зафіксована температура за останні 7 днів: *" + String(weekly.maxTemp) + "* ºC\n";
      response += "📌 Зафіксовано: " + String(weekly.maxTime) + "\n\n";
      response += "⬇️ Найнижча зафіксована температура за останні 7 днів: *" + String(weekly.minTemp) + "* ºC\n";
      response += "📌 Зафіксовано: " + String(weekly.minTime);
      bot.sendMessage(chat_id, response, "Markdown");
    }

    if (text == "/tempm") {
      String response = "⬆️ Найвища зафіксована температура за останні 30 днів: *" + String(monthly.maxTemp) + "* ºC\n";
      response += "📌 Зафіксовано: " + String(monthly.maxTime) + "\n\n";
      response += "⬇️ Найнижча зафіксована температура за останні 30 днів: *" + String(monthly.minTemp) + "* ºC\n";
      response += "📌 Зафіксовано: " + String(monthly.minTime);
      bot.sendMessage(chat_id, response, "Markdown");
    }

    if (chat_id == CHAT_ID) {
      if (text == "/cleartempd") {
        daily.maxTemp = -1000.0;
        daily.minTemp = 1000.0;
        strcpy(daily.maxTime, "");
        strcpy(daily.minTime, "");
        saveTempData(0, daily);
        bot.sendMessage(chat_id, "🧹 Дані за день очищено.", "");
      }

      if (text == "/cleartempw") {
        weekly.maxTemp = -1000.0;
        weekly.minTemp = 1000.0;
        strcpy(weekly.maxTime, "");
        strcpy(weekly.minTime, "");
        saveTempData(sizeof(TempData), weekly);
        bot.sendMessage(chat_id, "🧹 Дані за тиждень очищено.", "");
      }

      if (text == "/cleartempm") {
        monthly.maxTemp = -1000.0;
        monthly.minTemp = 1000.0;
        strcpy(monthly.maxTime, "");
        strcpy(monthly.minTime, "");
        saveTempData(sizeof(TempData) * 2, monthly);
        bot.sendMessage(chat_id, "🧹 Дані за місяць очищено.", "");
      }
    } else if (text.startsWith("/clear")) {
      bot.sendMessage(chat_id, "❌ Ви не маєте доступу до цієї команди 🙈", "");
    }
  }
}

void setup() {
  Serial.begin(115200);

#ifdef ESP8266
  configTime(2 * 3600, 0, "0.ua.pool.ntp.org"); // Отримуємо час через NTP
  client.setTrustAnchors(&cert); // Сертифікат для Telegram
#endif

  // Ініціалізація датчика
  if (!bme.begin(0x76)) {
    Serial.println("Не вдалося підключити датчик!");
    while (true);
  }

  // Підключення до Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

#ifdef ESP32
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT); // Додаємо сертифікат для Telegram
#endif

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Підключення до Wi-Fi...");
  }

  EEPROM.begin(EEPROM_SIZE);
  loadTempData(0, daily);
  loadTempData(sizeof(TempData), weekly);
  loadTempData(sizeof(TempData) * 2, monthly);
}

void loop() {
  if (millis() > lastTempCheck + tempCheckInterval) {
    updateTempData(daily, 0);
    updateTempData(weekly, sizeof(TempData));
    updateTempData(monthly, sizeof(TempData) * 2);
    lastTempCheck = millis();
  }

  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  while (numNewMessages) {
    handleNewMessages(numNewMessages);
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}
