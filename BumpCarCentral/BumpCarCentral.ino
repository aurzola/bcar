#include <Wire.h>
#include <Adafruit_PN532.h>
#include <esp_now.h>
#include <WiFi.h>
#include <map>
#include <Ticker.h>

Ticker grantingTimer;

#define I2C_SDA 5
#define I2C_SCL 18

Adafruit_PN532 nfc(-1, -1);

struct GRB { uint8_t g, r, b; };

const std::map<std::string, GRB> colorMap = {
    {"OFF",    {0,   0,   0}},
    {"RED",    {0,   50,  0}},
    {"GREEN",  {50,  0,   0}},
    {"BLUE",   {0,   0,   50}},
    {"YELLOW", {30,  30,  0}}
};

const int pinR = 25;
const int pinG = 26;
const int pinB = 27;
const int freq = 5000;
const int resolution = 8; // 8 bits (0-255)

// ── Declaraciones adelantadas ────────────────────────────────────
void setupLedRGB();
GRB  getColor(const std::string& colorName);

void sendColor(GRB color) {
  ledcWrite(pinR, color.r);
  ledcWrite(pinG, color.g);
  ledcWrite(pinB, color.b);
}

void setupLedRGB() {
  // Configurar canales LEDC
  ledcAttach(pinR, freq, resolution);
  ledcAttach(pinG, freq, resolution);
  ledcAttach(pinB, freq, resolution);
}

GRB getColor(const std::string& colorName) {
  auto it = colorMap.find(colorName);
  if (it != colorMap.end()) return it->second;
  return colorMap.at("OFF");
}

// ── Power Granting ───────────────────────────────────────────────
volatile bool timeoutReached = false;
bool grantingNow = false;
unsigned long grantingDuration = 35000;
unsigned long nextGrantingTime = 0;
const char* powColor = "NONE";

void scheduleNextGrantingWindow() {
  int maxWait = 20000;
  int minWait= 10000;
  nextGrantingTime = millis() + random(minWait, maxWait);
  Serial.print("Next granting window in ");
  Serial.print((nextGrantingTime - millis()) / 1000);
  Serial.println(" seconds");
}

const char* getRandomColorName() {
  std::vector<std::string> colorNames;
  for (const auto& pair : colorMap)
    if (pair.first != "OFF")
      colorNames.push_back(pair.first);
  return colorNames[random(0, colorNames.size())].c_str();
}

// ── Blink corregido: alterna color y OFF ─────────────────────────
void blink(int times) {
  for (int i = 0; i < times; i++) {
    sendColor(getColor(powColor));   // ON
    delay(200);
    sendColor(getColor("OFF"));      // ← OFF, antes estaba fijo en color
    delay(200);
  }
  // Restaurar el color activo después del blink
  if (grantingNow) sendColor(getColor(powColor));
}

void IRAM_ATTR onTimeout() {
  timeoutReached = true;
}

void closeGrantingWindow() {
  grantingNow = false;
  sendColor(getColor("OFF"));
  Serial.println("Granting window CLOSED");
  scheduleNextGrantingWindow();
}

void openGrantingWindow() {
  grantingNow = true;
  timeoutReached = false;
  powColor = getRandomColorName();
  sendColor(getColor(powColor));
  Serial.println("Granting window OPENED");
  grantingTimer.once_ms(grantingDuration, onTimeout);
}

// ── ESP-NOW ──────────────────────────────────────────────────────
void OnDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  Serial.println(status == ESP_NOW_SEND_SUCCESS
    ? "Delivery Success" : "Delivery Fail");
}

boolean addEspNowPeer(uint8_t* targetAddress) {
  esp_now_peer_info_t existingPeer;
  if (esp_now_get_peer(targetAddress, &existingPeer) == ESP_OK)
    return true;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, targetAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return false;
  }
  return true;
}

void grantPower(uint8_t* uid) {
  Serial.printf("GRANTING. PW Color: %s\n", powColor);
  if (!addEspNowPeer(uid)) return;

  const unsigned long GRANT_TIME = 10000;
  const float MULTI_VELOCIDAD = 0.45f;

  // Map color name to single letter
  char letter = 'X';
  if (strcmp(powColor, "RED") == 0) letter = 'R';
  else if (strcmp(powColor, "GREEN") == 0) letter = 'G';
  else if (strcmp(powColor, "BLUE") == 0) letter = 'B';
  else if (strcmp(powColor, "YELLOW") == 0) letter = 'Y';

  char mult_buf[8];
  dtostrf(MULTI_VELOCIDAD, 1, 2, mult_buf);

  char msg[32];
  snprintf(msg, sizeof(msg), "GRANT:%c:%lu:%s", letter, GRANT_TIME, mult_buf);

  for (int i = 0; i < 4; i++) {
    esp_err_t result = esp_now_send(uid, (uint8_t*)msg, strlen(msg));
    Serial.println(result == ESP_OK ? "Sent OK" : "Send error");
    delay(25); // Da tiempo a la antena receptora a cambiar a WiFi
  }

  closeGrantingWindow();
}

// ── NFC ──────────────────────────────────────────────────────────
bool readNFCData(uint8_t* output) {
  uint8_t uid[7] = {0};
  uint8_t uidLength;
  uint8_t keya[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  uint8_t data[16];

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
    if (nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keya)) {
      if (nfc.mifareclassic_ReadDataBlock(4, data)) {
        for (int i = 0; i < 6; i++) output[i] = data[i];
        blink(3);
        return true;
      }
    }
  }
  return false;
}

// ── Setup / Loop ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(350);

  setupLedRGB();
  Serial.println("=== BumpCar Power Central ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 not detected!");
    while (1);
  }
  nfc.SAMConfig();
  Serial.println("PN532 ready.");

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb((esp_now_send_cb_t)OnDataSent);

  randomSeed(analogRead(0));
  scheduleNextGrantingWindow();
}

void loop() {
  unsigned long currentTime = millis();

  if (timeoutReached) {
    timeoutReached = false;
    closeGrantingWindow();
  }

  if (!grantingNow && currentTime >= nextGrantingTime) {
    openGrantingWindow();
  }

  if (grantingNow) {
    uint8_t dataOut[6];
    if (readNFCData(dataOut)) {
      grantingTimer.detach();
      grantPower(dataOut);
    }
  }
}