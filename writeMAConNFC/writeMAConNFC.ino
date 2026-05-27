#include <Wire.h>
#include <Adafruit_PN532.h>
#include <esp_mac.h>  // Librería nativa del ESP32 para leer la MAC

#define I2C_SDA 5
#define I2C_SCL 18

Adafruit_PN532 nfc(-1, -1);

uint8_t dataToWrite[16] = {0}; 

void setup(void) {
  Serial.begin(115200);
  
  // 1. Obtener la MAC real del ESP32 (6 bytes)
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA); 
  
  // 2. Llenar el buffer de 16 bytes con la MAC (el resto queda en 0)
  memset(dataToWrite, 0, sizeof(dataToWrite));
  memcpy(dataToWrite, mac, 6);

  Serial.print("MAC preparada para escribir: ");
  for(int i=0; i<6; i++) {
    Serial.printf("%02X", mac[i]);
    if(i<5) Serial.print(":");
  }
  Serial.println();

  // 3. Iniciar I2C y NFC
  Wire.begin(I2C_SDA, I2C_SCL);
  nfc.begin();
  
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.print("Didn't find PN53x board");
    while (1);
  }
  nfc.SAMConfig();
  Serial.println("Esperando tarjeta...");
}

void loop(void) {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 }; 
  uint8_t uidLength;

  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);

  if (success) {
    uint8_t keya[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    // Autenticar Bloque 4
    success = nfc.mifareclassic_AuthenticateBlock(uid, uidLength, 4, 0, keya);

    if (success) {
      Serial.println("Autenticado. Escribiendo MAC...");
      
      // Escribir los datos
      if (nfc.mifareclassic_WriteDataBlock(4, dataToWrite)) {
        Serial.println("¡Escritura exitosa!");
        
        // Verificación
        uint8_t readBuffer[16];
        if (nfc.mifareclassic_ReadDataBlock(4, readBuffer)) {
          Serial.print("Verificación en tarjeta: ");
          for (uint8_t i = 0; i < 6; i++) {
            Serial.printf(" 0x%02X", readBuffer[i]);
          }
          Serial.println();
        }
      } else {
        Serial.println("Error al escribir.");
      }
    } else {
      Serial.println("Error de autenticación.");
    }
    delay(5000);
    Serial.println("\nEsperando otra tarjeta...");
  }
}