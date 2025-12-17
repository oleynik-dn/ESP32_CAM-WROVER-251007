#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "driver/dac.h"

void connectWiFi(bool blockUntilConnected = false);

// WiFi настройки
const char* ssid = "ИмяСети";
const char* password = "ПарольСети";

// OpenAI API
const char* openai_api_key = "ключ"; // <-- подставьте ваш ключ
const char* openai_url = "https://api.openai.com/v1/audio/speech";

// Пин кнопки
const int BUTTON_PIN = 0; // GPIO0 (BOOT кнопка)

// ЦАП пин (GPIO25 или GPIO26)
const int DAC_PIN = 25; // DAC1 - GPIO25

// Аудио буфер
uint8_t *audioBuffer = NULL;
size_t audioBufferSize = 0;

// Текст для озвучивания
const char* text_to_speech = "Солнце медленно опускается за горизонт, окрашивая небо в глубокие оттенки золота, розового и пурпурного.";

// MP3 декодирование (простой декодер)
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputBuffer.h"

// Класс для вывода в ЦАП (упрощённый)
class AudioOutputDAC : public AudioOutput {
  public:
    float gain = 0.5;
    AudioOutputDAC(int pin) {
      dacPin = pin;
      dac_output_enable((dac_channel_t)(pin - 25));
    }
    virtual ~AudioOutputDAC() override {}
    virtual bool begin() override { return true; }
    virtual bool ConsumeSample(int16_t sample[2]) override {
      int16_t mono = (sample[0] + sample[1]) / 2;
      mono = (int16_t)(mono * gain);
      uint8_t dacValue = (mono >> 8) + 128;
      dac_output_voltage((dac_channel_t)(dacPin - 25), dacValue);
      return true;
    }
    virtual bool stop() override {
      dac_output_voltage((dac_channel_t)(dacPin - 25), 128);
      return true;
    }
  private:
    int dacPin;
};

// Класс для чтения из буфера памяти
class AudioFileSourceMemory : public AudioFileSource {
  public:
    AudioFileSourceMemory(uint8_t *buf, size_t sz) {
      buffer = buf; size = sz; pos = 0;
    }
    virtual ~AudioFileSourceMemory() override {}
    virtual uint32_t read(void *data, uint32_t len) override {
      if (pos >= size) return 0;
      uint32_t toRead = (pos + len > size) ? (size - pos) : len;
      memcpy(data, buffer + pos, toRead);
      pos += toRead;
      return toRead;
    }
    virtual bool seek(int32_t p, int dir) override {
      if (dir == SEEK_SET) pos = p;
      else if (dir == SEEK_CUR) pos += p;
      else if (dir == SEEK_END) pos = size + p;
      if (pos > size) pos = size;
      return true;
    }
    virtual bool close() override { return true; }
    virtual bool isOpen() override { return true; }
    virtual uint32_t getSize() override { return size; }
    virtual uint32_t getPos() override { return pos; }
  private:
    uint8_t *buffer;
    size_t size;
    size_t pos;
};

AudioGeneratorMP3 *mp3 = NULL;
AudioOutputDAC *out = NULL;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32 OpenAI TTS (debug) ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Инициализация ЦАП
  dac_output_enable((dac_channel_t)(DAC_PIN - 25));
  dac_output_voltage((dac_channel_t)(DAC_PIN - 25), 128);
  Serial.printf("ЦАП инициализирован на GPIO%d\n", DAC_PIN);

  connectWiFi(true); // блокируем пока не подключимся (debug-friendly)

  Serial.println("Готов! Нажмите кнопку для генерации и воспроизведения аудио.");
}

void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("\n=== Кнопка нажата ===");
    delay(300);
    if (generateAudio()) {
      playAudio();
      cleanupMemory();
    } else {
      Serial.println("generateAudio вернул false.");
    }
    Serial.println("Готов к следующему нажатию.\n");
  }
  delay(50);
}

void connectWiFi(bool blockUntilConnected) {
  Serial.print("Подключение к WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi подключен!");
    Serial.print("IP адрес: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nОшибка подключения к WiFi!");
    Serial.printf("Статус WiFi: %d\n", WiFi.status());
    if (blockUntilConnected) {
      Serial.println("Буду пытаться подключиться в фоне каждые 5 секунд...");
      // Нелинейная блокировка: периодические попытки
      while (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        delay(500);
        WiFi.begin(ssid, password);
        for (int i=0;i<20 && WiFi.status()!=WL_CONNECTED;i++){ delay(250); Serial.print("."); }
        Serial.println();
        Serial.printf("Статус: %d\n", WiFi.status());
        if (WiFi.status() == WL_CONNECTED) {
          Serial.print("IP: "); Serial.println(WiFi.localIP());
          break;
        }
        delay(5000);
      }
    }
  }
}

bool generateAudio() {
  Serial.printf("Проверка WiFi статуса: %d\n", WiFi.status());
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi не подключен. Попытка переподключения...");
    connectWiFi(true);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Не удалось подключиться к WiFi — прерываю запрос.");
      return false;
    }
  }

  Serial.println("Генерация аудио через OpenAI API...");
  WiFiClientSecure client;
  client.setInsecure(); // без проверки сертификата (debug)

  HTTPClient http;
  Serial.println("Инициализация http.begin...");
  bool begun = http.begin(client, openai_url);
  Serial.printf("http.begin returned: %d\n", begun ? 1 : 0);
  if (!begun) {
    Serial.println("http.begin не удался.");
    return false;
  }

  http.setTimeout(20000); // 20s
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + openai_api_key);

  // Формирование JSON запроса (больший буфер)
  StaticJsonDocument<1024> doc;
  doc["model"] = "tts-1";
  doc["input"] = text_to_speech;
  doc["voice"] = "alloy";
  doc["response_format"] = "mp3";

  String jsonRequest;
  serializeJson(doc, jsonRequest);

  Serial.println("Отправка запроса...");
  Serial.printf("URL: %s\n", openai_url);
  Serial.printf("Payload size: %d\n", jsonRequest.length());

  int httpCode = -999;
  // Выполняем POST и ловим исключения
  httpCode = http.POST((uint8_t*)jsonRequest.c_str(), jsonRequest.length());

  Serial.printf("HTTP return code: %d\n", httpCode);
  Serial.printf("HTTPClient error string: %s\n", http.errorToString(httpCode).c_str());

  if (httpCode == HTTP_CODE_OK) {
    Serial.println("Аудио получено успешно!");
    WiFiClient *stream = http.getStreamPtr();

    const size_t CHUNK_SIZE = 4096;
    const size_t MAX_AUDIO_SIZE = 256000; // 256 KB
    uint8_t *tempBuffer = (uint8_t*)ps_malloc(MAX_AUDIO_SIZE);
    if (!tempBuffer) {
      Serial.println("Ошибка выделения временной памяти!");
      http.end();
      return false;
    }

    audioBufferSize = 0;
    unsigned long lastReadMillis = millis();
    Serial.print("Загрузка: ");

    // Читаем пока есть данные или пока соединение открыто
    while ((http.connected() || stream->available()) && audioBufferSize < MAX_AUDIO_SIZE) {
      size_t available = stream->available();
      if (available) {
        size_t toRead = min(available, CHUNK_SIZE);
        toRead = min(toRead, MAX_AUDIO_SIZE - audioBufferSize);
        int bytesRead = stream->readBytes(tempBuffer + audioBufferSize, toRead);
        if (bytesRead <= 0) break;
        audioBufferSize += bytesRead;
        Serial.print(".");
        lastReadMillis = millis();
      } else {
        // таймаут чтения
        if (millis() - lastReadMillis > 20000) {
          Serial.println("\nТаймаут чтения данных от сервера (20s).");
          break;
        }
        delay(10);
      }
    }

    Serial.println();
    Serial.printf("Загружено: %u байт\n", (unsigned)audioBufferSize);

    if (audioBufferSize == 0) {
      // Попробуем прочитать тело ответа (если есть) для диагностирования
      String body = http.getString();
      if (body.length() > 0) {
        Serial.println("Тело ответа от сервера:");
        Serial.println(body);
      } else {
        Serial.println("Не удалось загрузить аудио данные и тело ответа пустое.");
      }
      free(tempBuffer);
      http.end();
      return false;
    }

    // Выделяем финальный буфер точного размера
    audioBuffer = (uint8_t*)ps_malloc(audioBufferSize);
    if (!audioBuffer) {
      Serial.println("Ошибка выделения финальной памяти!");
      free(tempBuffer);
      http.end();
      return false;
    }
    memcpy(audioBuffer, tempBuffer, audioBufferSize);
    free(tempBuffer);
    http.end();
    return true;

  } else {
    // Подробнее по ошибке: если код отрицательный — это сетевые ошибки
    if (httpCode <= 0) {
      Serial.printf("Сетевая/локальная ошибка: %s\n", http.errorToString(httpCode).c_str());
      // Если -1, часто означает: соединение не установлено / DNS / TLS
      if (httpCode == -1) {
        Serial.println("HTTP -1: возможно не удалось установить TLS/соединение или DNS. Проверьте WiFi и доступность хоста.");
      }
    } else {
      String payload = http.getString();
      Serial.println("Ответ сервера: ");
      Serial.println(payload);
    }
    http.end();
    return false;
  }
}

void playAudio() {
  if (audioBuffer == NULL || audioBufferSize == 0) {
    Serial.println("Нет аудио для воспроизведения!");
    return;
  }

  Serial.println("Воспроизведение аудио через ЦАП...");

  AudioFileSourceMemory *source = new AudioFileSourceMemory(audioBuffer, audioBufferSize);
  out = new AudioOutputDAC(DAC_PIN);
  out->gain = 0.02;
  mp3 = new AudioGeneratorMP3();

  if (!mp3->begin(source, out)) {
    Serial.println("mp3->begin() вернул false");
  }

  while (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
    }
    yield();
  }

  Serial.println("Воспроизведение завершено");

  dac_output_voltage((dac_channel_t)(DAC_PIN - 25), 128);

  delete mp3; mp3 = NULL;
  delete out; out = NULL;
  delete source;
}

void cleanupMemory() {
  Serial.println("Очистка памяти...");
  if (audioBuffer != NULL) {
    free(audioBuffer);
    audioBuffer = NULL;
    audioBufferSize = 0;
  }
  Serial.println("Память очищена");
  Serial.printf("Свободная heap память: %u байт\n", (unsigned)ESP.getFreeHeap());
  Serial.printf("Свободная PSRAM: %u байт\n", (unsigned)ESP.getFreePsram());
}
