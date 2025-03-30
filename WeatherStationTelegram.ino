#ifdef ESP32
#include "WiFi.h"
#include "LittleFS.h"
#else
#include "ESP8266WiFi.h"
#include "LittleFS.h"
#endif

#include "WiFiClientSecure.h"
#include "UniversalTelegramBot.h"
#include "ArduinoJson.h"
#include "Adafruit_BME280.h"
#include "Adafruit_Sensor.h"

// Указати ваші дані Wi-Fi
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
  char lastReset[11];  // Останній день/тиждень/місяць скидання (YYYY-MM-DD)
  bool isInitialized;
};

TempData daily, weekly, monthly;

unsigned long lastTempCheck = 0;
const int tempCheckInterval = 60000;  // Перевірка температури кожну хвилину (60 секунд)
unsigned long lastSave = 0;
const int saveInterval = 3600000;  // Збереження раз на годину (3600 секунд)
unsigned long lastTimeSync = 0;       // Для періодичної ресинхронізації часу

void saveTempData(int address, TempData &data) {
  String filename = "/tempdata_" + String(address) + ".bin";
  File file = LittleFS.open(filename, "w");
  if (!file) {
    Serial.println("Не вдалося відкрити файл для запису: " + filename);
    return;
  }
  file.write((uint8_t*)&data, sizeof(TempData));
  file.close();
}

void loadTempData(int address, TempData &data) {
  String filename = "/tempdata_" + String(address) + ".bin";
  File file = LittleFS.open(filename, "r");
  if (!file) {
    // Файл не існує, ініціалізуємо за замовчуванням
    data.maxTemp = -1000.0;
    data.minTemp = 1000.0;
    strcpy(data.maxTime, "");
    strcpy(data.minTime, "");
    strcpy(data.lastReset, "");
    data.isInitialized = true;
    saveTempData(address, data);
  } else {
    file.read((uint8_t*)&data, sizeof(TempData));
    file.close();
    if (!data.isInitialized || isnan(data.maxTemp) || isnan(data.minTemp)) {
      // Дані пошкоджені, ініціалізуємо
      data.maxTemp = -1000.0;
      data.minTemp = 1000.0;
      strcpy(data.maxTime, "");
      strcpy(data.minTime, "");
      strcpy(data.lastReset, "");
      data.isInitialized = true;
      saveTempData(address, data);
    }
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

bool isNewPeriod(int address, const String& currentDay, const String& lastReset) {
  if (lastReset == "") return true;  // Якщо ще не було скидання

  time_t now = time(nullptr);
  struct tm currentTime;
  localtime_r(&now, &currentTime);

  struct tm resetTime;
  sscanf(lastReset.c_str(), "%d-%d-%d", &resetTime.tm_year, &resetTime.tm_mon, &resetTime.tm_mday);
  resetTime.tm_year -= 1900;  // Рік від 1900
  resetTime.tm_mon -= 1;      // Місяць від 0
  resetTime.tm_hour = 0;
  resetTime.tm_min = 0;
  resetTime.tm_sec = 0;

  time_t resetTimestamp = mktime(&resetTime);
  double diffSeconds = difftime(now, resetTimestamp);

  if (address == 0) {  // Daily
    return diffSeconds >= 24 * 3600;  // Новий день
  } else if (address == sizeof(TempData)) {  // Weekly
    return diffSeconds >= 7 * 24 * 3600;  // Новий тиждень
  } else {  // Monthly
    return diffSeconds >= 30 * 24 * 3600;  // Новий місяць (приблизно)
  }
}

void updateTempData(TempData &data, int address) {
  float temperature = bme.readTemperature();
  String currentTime = getTime();
  String currentDay = currentTime.substring(0, 10);
  bool shouldSave = false;

  // Перевірка на новий період і скидання
  if (isNewPeriod(address, currentDay, String(data.lastReset))) {
    data.maxTemp = -1000.0;
    data.minTemp = 1000.0;
    strcpy(data.maxTime, "");
    strcpy(data.minTime, "");
    currentDay.toCharArray(data.lastReset, 11);
    shouldSave = true;
  }

  // Оновлення максимуму та мінімуму
  if (temperature > data.maxTemp) {
    data.maxTemp = temperature;
    currentTime.toCharArray(data.maxTime, 20);
    shouldSave = true;
  }
  if (temperature < data.minTemp) {
    data.minTemp = temperature;
    currentTime.toCharArray(data.minTime, 20);
    shouldSave = true;
  }

  if (shouldSave || (millis() - lastSave > saveInterval)) {
    saveTempData(address, data);
    lastSave = millis();
  }
}

String getReadings() {
  float temperature = bme.readTemperature();
  float humidity = bme.readHumidity();
  float pressure = bme.readPressure() / 100.0F;

  String temperatureEmoji = "🌡️ Температура повітря: *" + String(temperature) + "* ºC";
  if (temperature <= 0) {
    temperatureEmoji += " ❄️";
  }
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

  // Налаштування Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

#ifdef ESP32
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
#endif
#ifdef ESP8266
  client.setTrustAnchors(&cert);
#endif

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Підключення до Wi-Fi...");
  }

  // Налаштування часу з урахуванням часового поясу України
  configTime(0, 0, "0.ua.pool.ntp.org", "pool.ntp.org"); // UTC без зміщення в коді

  // Вказуємо часовий пояс України з автоматичним DST
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1); // Східноєвропейський час (EET) з DST
  tzset(); // Застосовуємо налаштування часового поясу

  // Чекаємо синхронізації часу
  Serial.println("Очікування синхронізації часу...");
  time_t now = time(nullptr);
  while (now < 8 * 3600) { // Чекаємо, поки час стане валідним
    delay(500);
    now = time(nullptr);
  }
  Serial.println("Час синхронізовано!");

  // Ініціалізація BME280
  if (!bme.begin(0x76)) {
    Serial.println("Не вдалося підключити датчик!");
    while (true);
  }

  // Ініціалізація LittleFS
  if (!LittleFS.begin()) {
    Serial.println("Не вдалося ініціалізувати LittleFS!");
    while (true);
  }

  loadTempData(0, daily);
  loadTempData(sizeof(TempData), weekly);
  loadTempData(sizeof(TempData) * 2, monthly);
}

void loop() {
  // Періодична ресинхронізація часу (раз на добу)
  if (millis() - lastTimeSync > 24 * 3600 * 1000) {
    configTime(0, 0, "0.ua.pool.ntp.org", "pool.ntp.org");
    lastTimeSync = millis();
    Serial.println("Час ресинхронізовано!");
  }

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
