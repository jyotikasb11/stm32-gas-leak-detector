#include <WiFi.h>
#include <HTTPClient.h>

// ---- EDIT THESE THREE LINES ----
const char* ssid        = "YOUR_SSID";
const char* password    = "YOUR_PASSWORD";
String apiKey            = "API_KEY";
// ---------------------------------

const char* server = "http://api.thingspeak.com/update";

HardwareSerial STM32Serial(2);  // UART2 on ESP32: RX=GPIO16, TX=GPIO17

String inputBuffer = "";
unsigned long lastUploadTime = 0;
const unsigned long UPLOAD_INTERVAL = 16000; // ThingSpeak free tier needs >=15s between updates

int latestGasValue = 0;
String latestStatus = "Safe";
int latestBaseline = 0;
int latestDelta = 0;
int latestBuzzer = 0;
bool newDataAvailable = false;

void setup() {
  Serial.begin(115200);               // USB debug monitor
  STM32Serial.begin(9600, SERIAL_8N1, 16, 17); // Link to STM32 (RX=16, TX=17)

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // --- Read whatever the STM32 is sending ---
  while (STM32Serial.available()) {
    char c = STM32Serial.read();
    if (c == '\n') {
      parseLine(inputBuffer);
      inputBuffer = "";
    } else {
      inputBuffer += c;
    }
  }

  // --- Push the latest reading to ThingSpeak, respecting the rate limit ---
  if (newDataAvailable && (millis() - lastUploadTime >= UPLOAD_INTERVAL)) {
    uploadToThingSpeak(latestGasValue, latestStatus, latestBaseline, latestDelta, latestBuzzer);
    lastUploadTime = millis();
    newDataAvailable = false;
  }
}

void parseLine(String line) {
  line.trim();

  // Expected format: gasValue,status,baseline,delta,buzzerState
  int idx1 = line.indexOf(',');
  int idx2 = line.indexOf(',', idx1 + 1);
  int idx3 = line.indexOf(',', idx2 + 1);
  int idx4 = line.indexOf(',', idx3 + 1);

  if (idx1 > 0 && idx2 > 0 && idx3 > 0 && idx4 > 0) {
    latestGasValue = line.substring(0, idx1).toInt();
    latestStatus   = line.substring(idx1 + 1, idx2);
    latestBaseline = line.substring(idx2 + 1, idx3).toInt();
    latestDelta    = line.substring(idx3 + 1, idx4).toInt();
    latestBuzzer   = line.substring(idx4 + 1).toInt();
    newDataAvailable = true;

    Serial.print("Received -> Gas: ");
    Serial.print(latestGasValue);
    Serial.print(" Status: ");
    Serial.print(latestStatus);
    Serial.print(" Base: ");
    Serial.print(latestBaseline);
    Serial.print(" Delta: ");
    Serial.print(latestDelta);
    Serial.print(" Buzzer: ");
    Serial.println(latestBuzzer);
  }
}

void uploadToThingSpeak(int gasValue, String status, int baseline, int delta, int buzzer) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping upload.");
    return;
  }

  HTTPClient http;
  String statusValue = (status == "LEAK") ? "1" : "0";

  String url = String(server) + "?api_key=" + apiKey +
               "&field1=" + String(gasValue) +
               "&field2=" + statusValue +
               "&field3=" + String(baseline) +
               "&field4=" + String(delta) +
               "&field5=" + String(buzzer);

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode > 0) {
    Serial.print("ThingSpeak response: ");
    Serial.println(httpCode);
  } else {
    Serial.print("Upload failed, error: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}