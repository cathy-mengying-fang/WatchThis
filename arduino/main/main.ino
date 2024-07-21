#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include "esp_camera.h"
#include "FS.h"
// #include "SD.h"
#include "SPI.h"
#include "WiFi.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Base64_t.h"

#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"

const char *ssid = "YOUR WIFI SSID";
const char *password = "YOUR WIFI PASSWORD";

#define CAMERA_MODEL_XIAO_ESP32S3  // Has PSRAM
#define TOUCH_INT D7

#include "camera_pins.h"

// Width and height of round display
const int camera_width = 240;
const int camera_height = 240;
bool camera_sign = false;

TFT_eSPI tft = TFT_eSPI();
const int centerX = 120;
const int centerY = 120;
const int radius = 240; 

#define MAX_INPUT_SIZE 256
char chatgpt_token[MAX_INPUT_SIZE] = "YOUR OPENAI API KEY";
char user_question[MAX_INPUT_SIZE] = "What is this?";

const char *imageFile_prefix = "data:image/jpeg;base64,";
const char *payload_begin = "{\"model\": \"gpt-4o\", \"messages\": [{\"role\": \"user\", \"content\": [{\"type\":\"text\",\"text\":\"";
const char *payload_mid = " Short Answer only\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"";
const char *payload_end = "\"}}]}],\"max_tokens\": 1000}";
uint32_t metadataOffset = 0;

#define PAYLOAD_SIZE 300000
char *payload;

AsyncWebServer server(80);

bool display_is_pressed(void) {
  if (digitalRead(TOUCH_INT) != LOW) {
    delay(15);
    if (digitalRead(TOUCH_INT) != LOW)
      return false;
  }
  return true;
}
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println();
  Serial.println(WiFi.localIP());
}

void textWrap(const char *string, int32_t x, int32_t y, int maxlen){
  int len = strlen(string);
  char part1[100];
  char part2[100];
  if (len > maxlen){
    strncpy(part1, string, maxlen);
    part1[maxlen] = '\0'; 
    tft.drawString(part1,x,y);
    strncpy(part2, string + maxlen, len - maxlen);
    part2[len - maxlen] = '\0';
    tft.drawString(part2,x,y+10);
  }
  else{
    tft.drawString(string,x,y);
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  initWiFi();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = SPIFFS.open("/index.html").readString();
    Serial.println("web: ");
    Serial.write(user_question, strlen(user_question));
    Serial.println(String(user_question));
    html.replace("%current_prompt%", String(user_question));
    request->send(200, "text/html", html);
  });

  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/style.css", "text/css");
  });
  
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send(204); // No Content response
  });

  server.on("/prompt", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("text")) {
      String prompt_input = request->getParam("text")->value();
      strncpy(user_question, prompt_input.c_str(), sizeof(user_question) - 1);
      user_question[sizeof(user_question) - 1] = '\0';

      memset(payload, 0, PAYLOAD_SIZE * sizeof(char));

      strcpy(payload, payload_begin);
      strcat(payload, user_question);
      strcat(payload, payload_mid);
      strcat(payload, imageFile_prefix);
    
      metadataOffset = strlen(payload);
      Serial.write(user_question, strlen(user_question));
      request->send(200, "text/html", "Received prompt: " + prompt_input + ". <a href='/'>Go back</a>");
    } else {
      request->send(200, "text/html", "No string received. <a href='/'>Go back</a>");
    }
  });
  // Start server
  server.begin();

  
  // Camera pinout
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  
  config.frame_size = FRAMESIZE_240X240;
  
  config.pixel_format = PIXFORMAT_RGB565;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
  Serial.println("Camera ready");
  camera_sign = true;  // Camera initialization check passes

  // Display initialization
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_WHITE);
  tft.setTextDatum(4);

  payload = (char *)ps_malloc(PAYLOAD_SIZE * sizeof(char));
  if (payload == 0) {
    Serial.println("ERROR: can't allocated PSRAM to payload variable");
  }
  memset(payload, 0, PAYLOAD_SIZE * sizeof(char));

  strcpy(payload, payload_begin);
  strcat(payload, user_question);
  strcat(payload, payload_mid);
  strcat(payload, imageFile_prefix);

  metadataOffset = strlen(payload);
}

void loop() {
  if (camera_sign) {

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Failed to get camera frame buffer");
      return;
    }

    if (display_is_pressed()) {
      size_t _jpg_buf_len;
      uint8_t *_jpg_buf;
      bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);

      memset(payload + metadataOffset, 0, PAYLOAD_SIZE * sizeof(char) - metadataOffset);
      int encodedLength = Base64.encodedLength(_jpg_buf_len);

      if ((encodedLength + metadataOffset) > PAYLOAD_SIZE) {
        Serial.println("ERROR: exceeded allocated payload size");
        Serial.print("  Encoded length would be:\t");
        Serial.println(encodedLength);
        while (1);
      }

      Base64.encode(payload + metadataOffset, (char *)_jpg_buf, _jpg_buf_len);

      strcat(payload, payload_end);

      Serial.println("printing sizeof char: ");
      Serial.println(strlen(payload));

      HTTPClient https;
      Serial.print("[HTTPS] begin...\n");
      if (https.begin("https://api.openai.com/v1/chat/completions")) {

        https.addHeader("Content-Type", "application/json");
        String token_key = String("Bearer ") + chatgpt_token;
        https.addHeader("Authorization", token_key);

        https.setTimeout(20000);  //to compensate for openai speed
        Serial.print("\n[HTTPS] POST...\n");

        // start connection and send HTTP header
        int httpCode = 0;
        do {
          httpCode = https.POST((uint8_t *)payload, strlen(payload));
          Serial.println(httpCode);
          Serial.println(https.errorToString(httpCode).c_str());
        } while (httpCode == -11);
        delay(500);
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
          String payload_response = https.getString();
          Serial.println(payload_response);

          StaticJsonDocument<1500> doc;
          DeserializationError error = deserializeJson(doc, payload_response);
          if (error) {
            Serial.print("deserializeJson() failed: ");
            Serial.println(error.c_str());
            return;
          }
          JsonObject choices_0 = doc["choices"][0];
          const char *gpt_text = choices_0["message"]["content"];

          textWrap(gpt_text,centerX,centerY,37); // display response in the middle of the screen
          
          delay(3000);

        } else {
          Serial.printf("[HTTPS] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
        }
        https.end();
      } else {
        Serial.printf("[HTTPS] Unable to connect\n");
      }
    }

    // Decode JPEG images
    uint8_t *buf = fb->buf;
    uint32_t len = fb->len;
    tft.startWrite();
    tft.setAddrWindow(0, 0, camera_width, camera_height);
    tft.pushColors(buf, len);
    textWrap(user_question,centerX,200,27); // display current prompt on the bottom of the screen
    tft.endWrite();

    // Release image buffer
    esp_camera_fb_return(fb);

    delay(10);
  }
}
