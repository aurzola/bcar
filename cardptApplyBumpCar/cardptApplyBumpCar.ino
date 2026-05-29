#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define APPLY_CMD "APPLY:"
#define GRAY 0x7BEF 

// Configuración de reintentos de fondo
#define MAX_INTENTOS 5
#define REENTRADA_INTERVALO 1500 

#define DURACION 10000
#define MULTIPLICADOR 0.45

// Estructuras de peers
#define MAX_PEERS 6
struct PeerEntry {
    uint8_t mac[6];
    int   fallos;
    char  nombre[16];
};

PeerEntry peers[MAX_PEERS] = {
    {{0x1C, 0xC3, 0xAB, 0xC3, 0xD0, 0xE8}, 0,  "CARRO_1"},
    {{0x24, 0x6F, 0x28, 0x96, 0x03, 0x08}, 0,  "CARRO_2"},
    {{0x1C, 0xC3, 0xAB, 0xF9, 0x2C, 0xE0}, 0,  "CARRO_3"},
    {{0x1C, 0xC3, 0xAB, 0xFA, 0x0C, 0x5C}, 0,  "CARRO NARANJA"},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 99, ""},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 99, ""},
};

struct PendingMsg {
    uint8_t mac[6];
    char    msg[48]; // Incrementado a 48 para evitar truncamiento del nuevo formato
    int     intentos;
    bool    confirmado;
    uint32_t ultimoIntentoTiempo;
};

#define MAX_PENDING 8
PendingMsg pendingQueue[MAX_PENDING];
int        pendingCount = 0;

// UI Control
const int NUM_EFFECTS = 4;
const char* efectos[NUM_EFFECTS] = {"RED", "YELLOW", "BLUE", "GREEN"};
int currentIndex = 0;

void drawUI() {
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(WHITE);
    M5Cardputer.Display.setTextSize(2);
    
    M5Cardputer.Display.setCursor(10, 15);
    M5Cardputer.Display.print("EFECTO A ENVIAR:");
    
    for (int i = 0; i < NUM_EFFECTS; i++) {
        M5Cardputer.Display.setCursor(20, 45 + (i * 22));
        if (i == currentIndex) {
            M5Cardputer.Display.setTextColor(GREEN);
            M5Cardputer.Display.print("> ");
        } else {
            M5Cardputer.Display.setTextColor(GRAY);
            M5Cardputer.Display.print("  ");
        }
        M5Cardputer.Display.print(efectos[i]);
    }
}

// Función auxiliar que busca en la cola por MAC y remueve el mensaje
void dequeue(const uint8_t* mac_addr) {
    for (int i = 0; i < pendingCount; i++) {
        if (memcmp(pendingQueue[i].mac, mac_addr, 6) == 0) {
            Serial.printf("[COLA] Mensaje removido con éxito para %02X:%02X:%02X\n", mac_addr[3], mac_addr[4], mac_addr[5]);
            // Desplazar los elementos restantes hacia atrás
            for (int j = i; j < pendingCount - 1; j++) {
                pendingQueue[j] = pendingQueue[j + 1];
            }
            pendingCount--;
            break; 
        }
    }
}

// Callback de envío
void OnDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        // PERDONAR: Si el mensaje llegó, reseteamos sus fallos a 0
        for (int i = 0; i < MAX_PEERS; i++) {
            if (memcmp(peers[i].mac, mac_addr, 6) == 0) {
                peers[i].fallos = 0; 
                Serial.printf("[ESPNOW] ¡Éxito! Errores reseteados para %s\n", peers[i].nombre);
                break;
            }
        }
        dequeue(mac_addr); 
    } else {
        Serial.printf("[ESPNOW] Fallo de ACK de hardware desde %02X:%02X:%02X\n", mac_addr[3], mac_addr[4], mac_addr[5]);
    }
}

// Agendar comando en la cola con el nuevo formato
void queueEffectCommand(const char* efecto) {
    char payload[48];
    // Formato modificado: APPLY:[EFECTO]:10000:0.20
    snprintf(payload, sizeof(payload), "%s%s:%d:%.2f", APPLY_CMD, efecto, DURACION, MULTIPLICADOR);
    
    Serial.printf("[UI] Generando comando: %s\n", payload);

    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].fallos == 99 || (peers[i].mac[0] == 0 && peers[i].mac[5] == 0)) {
            continue;
        }

        if (pendingCount >= MAX_PENDING) {
            Serial.println("[COLA] ¡Error! Cola llena, no se pudo agendar para todos los dispositivos.");
            break; 
        }

        memcpy(pendingQueue[pendingCount].mac, peers[i].mac, 6);
        strncpy(pendingQueue[pendingCount].msg, payload, sizeof(pendingQueue[pendingCount].msg));
        pendingQueue[pendingCount].intentos = 0;
        pendingQueue[pendingCount].confirmado = false;
        pendingQueue[pendingCount].ultimoIntentoTiempo = 0; 
        
        Serial.printf("[COLA] Agendado para %s [%02X:%02X:%02X]\n", peers[i].nombre, peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
        pendingCount++;
    }
}

// Procesamiento de la cola con logs
void procesarColaMensajes() {
    uint32_t ahora = millis();
    int i = 0;

    while (i < pendingCount) {
        // Si agotó los intentos máximos sin éxito
        if (pendingQueue[i].intentos >= MAX_INTENTOS) {
            for(int p=0; p < MAX_PEERS; p++) {
                if(memcmp(peers[p].mac, pendingQueue[i].mac, 6) == 0) {
                    peers[p].fallos++;
                    Serial.printf("[COLA] Descartado por reintentos agotados: %s. Fallos totales: %d\n", peers[p].nombre, peers[p].fallos);
                    break;
                }
            }
            // Lo sacamos de la cola
            for (int j = i; j < pendingCount - 1; j++) {
                pendingQueue[j] = pendingQueue[j + 1];
            }
            pendingCount--;
            continue;
        }

        // Intervalo de reenvío controlado por tiempo
        if (ahora - pendingQueue[i].ultimoIntentoTiempo >= REENTRADA_INTERVALO) {
            pendingQueue[i].intentos++;
            pendingQueue[i].ultimoIntentoTiempo = ahora;
            
            Serial.printf("[ENVÍO] Intentando enviar a %02X:%02X:%02X (Intento %d/%d): %s\n", 
                          pendingQueue[i].mac[3], pendingQueue[i].mac[4], pendingQueue[i].mac[5], 
                          pendingQueue[i].intentos, MAX_INTENTOS, pendingQueue[i].msg);
            
            esp_err_t result = esp_now_send(pendingQueue[i].mac, (uint8_t *)pendingQueue[i].msg, strlen(pendingQueue[i].msg) + 1);
            if (result != ESP_OK) {
                Serial.printf("[ESPNOW] Error de envío de inmediato en stack (Código: %d)\n", result);
            }
        }
        
        i++; 
    }
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    
    Serial.begin(115200);
    Serial.println("[SISTEMA] M5Cardputer Iniciando...");
    
    WiFi.mode(WIFI_STA);
    
    // Forzar Canal 1
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); 
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[SISTEMA] ¡Error crítico! No se pudo inicializar ESP-NOW.");
        M5Cardputer.Display.fillScreen(RED);
        while (1) delay(100);
    }

    esp_now_register_send_cb((esp_now_send_cb_t)OnDataSent);

    // Registrar Peers en el stack de hardware
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].fallos == 99) continue;
        
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(esp_now_peer_info_t));
        memcpy(peerInfo.peer_addr, peers[i].mac, 6);
        peerInfo.channel = 1; 
        peerInfo.encrypt = false;
        
        if(esp_now_add_peer(&peerInfo) == ESP_OK) {
            Serial.printf("[SISTEMA] Peer registrado: %s\n", peers[i].nombre);
        } else {
            Serial.printf("[SISTEMA] Fallo al registrar peer: %s\n", peers[i].nombre);
        }
    }

    drawUI();
    Serial.println("[SISTEMA] Listo. Esperando interacción.");
}

void loop() {
    M5Cardputer.update();

    if (M5Cardputer.Keyboard.isPressed()) {
        if (M5Cardputer.Keyboard.isKeyPressed(';')) { // Flecha Arriba
            currentIndex = (currentIndex == 0) ? NUM_EFFECTS - 1 : currentIndex - 1;
            drawUI();
            while(M5Cardputer.Keyboard.isKeyPressed(';')) { M5Cardputer.update(); }
        }
        else if (M5Cardputer.Keyboard.isKeyPressed('.')) { // Flecha Abajo
            currentIndex = (currentIndex == NUM_EFFECTS - 1) ? 0 : currentIndex + 1;
            drawUI();
            while(M5Cardputer.Keyboard.isKeyPressed('.')) { M5Cardputer.update(); }
        }
        else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
            queueEffectCommand(efectos[currentIndex]);
            while(M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) { M5Cardputer.update(); }
        }
    }

    procesarColaMensajes();
    delay(30);
}