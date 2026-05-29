#include <Bluepad32.h>
#include "esp_wifi.h"  // ← para esp_wifi_set_channel
#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include "DFRobotDFPlayerMini.h"
#include <map>
#include <string>
#include <FastLED.h>

HardwareSerial dfSerial(2);
DFRobotDFPlayerMini myDFPlayer;

// ── Estado audio no bloqueante ───────────────────────────────────
bool audioActivo          = false;
unsigned long audioFin    = 0;
int           audioPista  = 0;
#define TRACKS_COUNT 34 // count of tracks on SD

// -- Lógica de Audio No Bloqueante (Seguro para Interrupciones) --
volatile int  pistaPendiente = 0;      // 0 significa ninguna
volatile unsigned long duracionPendiente = 0;

// ── FastLED ──────────────────────────────────────────────────────
#define NUM_LEDS 1
#define PIN_RGB  23
CRGB leds[NUM_LEDS];

// ── Estado RGB ───────────────────────────────────────────────────
volatile bool colorPendiente  = false;
volatile bool efectoPendiente = false;
CRGB          colorNuevo      = CRGB::Black;
unsigned long colorHasta      = 0;

// ── Pines de Hardware ────────────────────────────────────────────
const int motorA1               = 26; //IN4
const int motorA2               = 33;  //IN3
const int motorB1         = 14;  //IN1
const int motorB2         = 27;  //IN2
const int vaporizador     = 4;
const int PIN_EYECCION    = 19;
const int PIN_LED_TRASERO = 13;
int sound_volume = 25;

// ── Calibración de giro ──────────────────────────────────────────
const unsigned long MS_POR_90_GRADOS = 400;
const unsigned long MS_POR_45_GRADOS = MS_POR_90_GRADOS / 2;
const int VELOCIDAD_GIRO_EXACTO      = 180;

// ── Estado global ────────────────────────────────────────────────
GamepadPtr    mandoPrincipal       = nullptr;
bool          rumbleActive         = false;
unsigned long rumbleEnd            = 0;
unsigned long lastDebug            = 0;
bool          modoDualJoystick     = false;
bool          giroExactoActivo     = false;
unsigned long giroExactoFin        = 0;
int           giroExactoDireccion  = 0;
bool          enParqueo            = true;
volatile bool eyeccionDetectada    = false;
String grantedPow = "NONE";

// ── Efectos ──────────────────────────────────────────────────────
unsigned long grantEffectDuration = 10000;
unsigned long efectoFin              = 0;
String        efectoActual           = "NONE";
bool          controlesInvertidos    = false;
bool          motorBloqueado         = false;
float         multiplicadorVelocidad = 1.0;

// ── ESP-NOW ──────────────────────────────────────────────────────
#define GRANT_CMD "GRANT:"
#define APPLY_CMD "APPLY:"
#define ESPNOW_MAX_RETRY 12
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myMac[6];
    
// ── Peers conocidos — máximo 6 ───────────────────────────────────
#define MAX_PEERS 6
#define MAX_FALLOS 20  // Límite de fallos permitidos

struct PeerEntry {
    uint8_t mac[6];
    int   fallos;
    char  nombre[16];
};

PeerEntry peers[MAX_PEERS] = {
    {{0x1C, 0xC3, 0xAB, 0xC3, 0xD0, 0xE8}, 0,  "CARRO_1"},
    {{0x24, 0x6F, 0x28, 0x96, 0x03, 0x08}, 0,  "CARRO_2"},
    {{0X1C, 0XC3, 0XAB, 0XF9, 0X2C, 0XE0}, 0,  "CARRO_3"},
    {{0x1C, 0xC3, 0xAB, 0xFA, 0x0C, 0x5C}, 0,  "CARRO NARANJA"},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 99, ""},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 99, ""},
};

// ── Colores ──────────────────────────────────────────────────────
const std::map<std::string, CRGB> colorMap = {
    {"OFF",     CRGB(0,   0,   0)},
    {"RED",     CRGB(50,  0,   0)},
    {"GREEN",   CRGB(0,   50,  0)},
    {"BLUE",    CRGB(0,   0,   50)},
    {"YELLOW",  CRGB(50,  50,  0)},
    {"CYAN",    CRGB(0,   50,  50)},
    {"MAGENTA", CRGB(50,  0,   50)},
    {"WHITE",   CRGB(50,  50,  50)},
    {"ORANGE",  CRGB(50,  20,  0)},
    {"PURPLE",  CRGB(25,  0,   25)}
};

struct PendingMsg {
    uint8_t mac[6];
    char    msg[32];
    int     intentos;
    bool    confirmado;
};

#define MAX_PENDING 8
PendingMsg pendingQueue[MAX_PENDING];
int        pendingCount = 0;

// ── Prototipos ───────────────────────────────────────────────────
void processGamepad(GamepadPtr gp);
void processModoSingleJoystick(int throttle, int steer);
void processMododualJoystick(int throttle, int steer);
void iniciarGiroExacto(int direccion);
void ejecutarGiroExacto();
void detener();
void blink(int times);
void setupAudio();
void reproducirEyeccion();
void onConnectedGamepad(GamepadPtr gp);
void onDisconnectedGamepad(GamepadPtr gp);
void sendColor(CRGB color);
CRGB getColor(const std::string& colorName);
void sendBroadcast(String mensaje);
void throwPow();
void resetEfectos();
void playAudio(int pista, unsigned long duracionMs);
void checkPistasPendientes();

// ── FastLED helpers ──────────────────────────────────────────────
CRGB getColor(const std::string& colorName) {
    auto it = colorMap.find(colorName);
    return (it != colorMap.end()) ? it->second : CRGB::Black;
}

void sendColor(CRGB color) {
    leds[0] = color;
    FastLED.show();
}

// ── Efectos helpers ──────────────────────────────────────────────
void resetEfectos() {
    efectoActual           = "NONE";
    controlesInvertidos    = false;
    motorBloqueado         = false;
    multiplicadorVelocidad = 1.0;
    Serial.println("[EFECTO] Efectos reseteados — control normal");
}

void procesarEfecto(const char* nombreEfecto) {
    resetEfectos(); // Limpia todo y pone multiplicadorVelocidad = 1.0
    efectoActual = nombreEfecto;
    efectoFin    = millis() + grantEffectDuration;

    if      (efectoActual == "RED")    motorBloqueado         = true;
    else if (efectoActual == "YELLOW") controlesInvertidos    = true;
    else if (efectoActual == "BLUE") {
        // Dejamos que OnDataRecv asigne el valor específico de la red justo después de salir de aquí
    }

    Serial.printf("[EFECTO] Aplicando: %s por %lu ms\n", nombreEfecto, grantEffectDuration);
}

// ── ESP-NOW ──────────────────────────────────────────────────────
bool starts_with(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

void OnDataRecv(const uint8_t* mac, const uint8_t* incomingData, int len) {
    char buffer[64];
    int safeLen = min(len, (int)sizeof(buffer)-1);
    memcpy(buffer, incomingData, safeLen);
    buffer[safeLen] = '\0';

    Serial.printf("[ESPNOW] Texto Recibido: %s\n", buffer);

    if (starts_with(buffer, GRANT_CMD)) {
        const char* payload = buffer + strlen(GRANT_CMD); // e.g. "B:10000:0.20"
        
        // Guardamos el poder TAL CUAL para ser lanzado en throwPow más adelante
        grantedPow = String(payload); 

        // --- PARSEO LOCAL PARA LOS LEDS Y AUDIO DEL PROPIO CARRO ---
        char payloadCopy[64];
        strncpy(payloadCopy, payload, sizeof(payloadCopy));
        payloadCopy[sizeof(payloadCopy)-1] = '\0';

        char* tok = strtok(payloadCopy, ":");
        char colorLetter = (tok != NULL && tok[0] != '\0') ? tok[0] : 'X';
        tok = strtok(NULL, ":");
        unsigned long timeMs = (tok != NULL) ? strtoul(tok, NULL, 10) : grantEffectDuration;
        tok = strtok(NULL, ":");
        float mult = (tok != NULL) ? atof(tok) : 1.0f;

        grantEffectDuration = timeMs;

        // Mapeo visual para el LED de este carro
        const char* colorName = "OFF";
        if      (colorLetter == 'R') colorName = "RED";
        else if (colorLetter == 'G') colorName = "GREEN";
        else if (colorLetter == 'B') colorName = "BLUE";
        else if (colorLetter == 'Y') colorName = "YELLOW";

        colorNuevo = getColor(colorName);
        colorHasta = millis() + timeMs;
        colorPendiente = true;
        efectoPendiente = false;

        // Si la torre nos dio BLUE, aplicamos el multiplicador a nosotros mismos de inmediato
        if (colorLetter == 'B') {
            multiplicadorVelocidad = mult;
        }

        pistaPendiente = 2;
        duracionPendiente = 5000;

        Serial.printf("[ESPNOW] Poder Otorgado : %s \n", grantedPow.c_str());
    }
   else if (starts_with(buffer, APPLY_CMD)) {
        const char* payload = buffer + strlen(APPLY_CMD); // e.g. "G:10000:0.45"

        // Parseamos de forma segura el efecto recibido
        char payloadCopy[64];
        strncpy(payloadCopy, payload, sizeof(payloadCopy));
        payloadCopy[sizeof(payloadCopy)-1] = '\0';

        char* tok = strtok(payloadCopy, ":");
        char letraEfecto = (tok != NULL && tok[0] != '\0') ? tok[0] : 'X';
        tok = strtok(NULL, ":");
        unsigned long tiempoMs = (tok != NULL) ? strtoul(tok, NULL, 10) : grantEffectDuration;
        tok = strtok(NULL, ":");
        float mult = (tok != NULL) ? atof(tok) : 1.0f;

        // Traducimos la letra al nombre del efecto/color real
        const char* nombreEfecto = "NONE";
        if      (letraEfecto == 'R') nombreEfecto = "RED";
        else if (letraEfecto == 'G') nombreEfecto = "GREEN";
        else if (letraEfecto == 'B') nombreEfecto = "BLUE";
        else if (letraEfecto == 'Y') nombreEfecto = "YELLOW";

        // Si es un efecto válido, actualizamos la duración global y lo procesamos
        if (strcmp(nombreEfecto, "NONE") != 0) {
            grantEffectDuration = tiempoMs;
            
            // 1. Primero procesamos el efecto (esto limpia y resetea a 1.0)
            procesarEfecto(nombreEfecto);
            
            // 2. AHORA SÍ: Si el efecto es BLUE, sobreescribimos con el multiplicador real enviado
            if (letraEfecto == 'B') {
                multiplicadorVelocidad = mult;
            }

            Serial.printf("[ESPNOW] Aplicando Efecto Real Parsed: %s (Mult Real Aplicado: %.2f)\n", nombreEfecto, multiplicadorVelocidad);
        } else {
            Serial.printf("[ESPNOW] Letra de efecto desconocida: %c\n", letraEfecto);
        }
    } else {
        Serial.printf("[ESPNOW] Comando no reconocido: %s\n", buffer);
    }
}

// ── Agregar mensaje a la cola ────────────────────────────────────
void enqueue(const uint8_t* mac, const char* msg) {
    if (pendingCount >= MAX_PENDING) {
        Serial.println("[QUEUE] Cola llena — descartando mensaje");
        return;
    }
    PendingMsg& p = pendingQueue[pendingCount++];
    memcpy(p.mac, mac, 6);
    strncpy(p.msg, msg, sizeof(p.msg) - 1);
    p.msg[sizeof(p.msg) - 1] = '\0';
    p.intentos    = 0;
    p.confirmado  = false;
    Serial.printf("[QUEUE] Encolado: %s — peers en cola: %d\n", msg, pendingCount);
}

// ── Remover mensaje confirmado de la cola ────────────────────────
void dequeue(const uint8_t* mac) {
    for (int i = 0; i < pendingCount; i++) {
        if (memcmp(pendingQueue[i].mac, mac, 6) == 0) {
            Serial.printf("[QUEUE] Confirmado y removido para MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            for (int j = i; j < pendingCount - 1; j++) {
                pendingQueue[j] = pendingQueue[j + 1];
            }
            pendingCount--;
            return;
        }
    }
}

// ── Callback de envío — confirma o mantiene en cola ──────────────
void OnDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        // PERDONAR: Si el mensaje llegó, reseteamos sus fallos a 0
        for (int i = 0; i < MAX_PEERS; i++) {
            if (memcmp(peers[i].mac, mac_addr, 6) == 0) {
                peers[i].fallos = 0; 
                break;
            }
        }
        Serial.printf("[ESPNOW] Entregado a %02X:%02X:%02X:%02X:%02X:%02X\n", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        dequeue(mac_addr);
    }
}

// ── Registrar peer temporal si no existe ─────────────────────────
void ensurePeer(const uint8_t* mac) {
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, mac, 6);
        peerInfo.channel = 1;
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
    }
}

// ── Procesar cola — llamar desde el loop ─────────────────────────
void processQueue() {
    if (pendingCount == 0) return;

    static unsigned long lastRetry = 0;
    if (millis() - lastRetry < 60) return;
    lastRetry = millis();

    for (int i = 0; i < pendingCount; i++) {
        PendingMsg& p = pendingQueue[i];

        if (p.intentos >= ESPNOW_MAX_RETRY) {
            // CASTIGAR: Si descartamos el mensaje, aumentamos el contador de fallos del peer
            for (int j = 0; j < MAX_PEERS; j++) {
                if (memcmp(peers[j].mac, p.mac, 6) == 0) {
                    peers[j].fallos++;
                    Serial.printf("[PEER] %s falló. Total fallos: %d\n", peers[j].nombre, peers[j].fallos);
                    break;
                }
            }
            
            // Remover de la cola
            for (int j = i; j < pendingCount - 1; j++) {
                pendingQueue[j] = pendingQueue[j + 1];
            }
            pendingCount--;
            i--;
            continue;
        }

        ensurePeer(p.mac);
        esp_now_send(p.mac, (uint8_t*)p.msg, strlen(p.msg));
        p.intentos++;
    }
}

// ── Gestión Segura de Audio ──────────────────────────────────────
void playAudio(int pista, unsigned long duracionMs) {
    // Ya no llama al hardware, solo levanta la bandera
    pistaPendiente = pista;
    duracionPendiente = duracionMs;
}

void checkPistasPendientes() {
    // Procesa peticiones de forma segura en el hilo principal
    if (pistaPendiente != 0) {
        int p = pistaPendiente;
        unsigned long d = duracionPendiente;
        
        pistaPendiente = 0; 
        duracionPendiente = 0;

        myDFPlayer.stop();
        delay(100); 
        myDFPlayer.play(p);
        
        audioActivo = true;
        audioFin    = millis() + d;
        audioPista  = p;
        Serial.printf("[AUDIO] Ejecutando pista: %d\n", p);
    }
}

void checkAudio() {
    if (!audioActivo) return;
    
    if (millis() > audioFin) {
        audioActivo = false;
        audioPista = 0; 
        Serial.println("[AUDIO] Temporizador de software completado");
    }
}

// ── throwPow — encola para cada peer conocido ────────────────────
void throwPow() {
    if (grantedPow == "NONE") {
        if (audioPista < 10) 
            playAudio(1, 700); 
        return;
    }

    // Construimos el mensaje final usando el bloque exacto que guardamos de la torre
    char msg[32];
    snprintf(msg, sizeof(msg), "APPLY:%s", grantedPow.c_str()); // Ej: APPLY:B:10000:0.20

    int encolados = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].fallos >= MAX_FALLOS) continue;
        if (memcmp(peers[i].mac, myMac, 6) == 0) continue; // No nos atacamos a nosotros mismos

        enqueue(peers[i].mac, msg);
        encolados++;
    }

    Serial.printf("[POWER] Retransmitiendo Ataque: %s a %d carros\n", msg, encolados);

    // Consumimos el poder y limpiamos estados
    grantedPow      = "NONE";
    colorPendiente  = false;
    efectoPendiente = false;
    
    int pistaAleatoria = random(10, TRACKS_COUNT); 
    playAudio(pistaAleatoria, 10000); 

    if (mandoPrincipal && mandoPrincipal->isConnected()) {
        mandoPrincipal->setRumble(0xff, 0xff);
        rumbleActive = true;
        rumbleEnd    = millis() + 500;
    }
}

// ── Setup ESP-NOW ────────────────────────────────────────────────
void setupEspNow() {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[ESPNOW] Canal fijado: %d\n", WiFi.channel());

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] Error al inicializar");
        return;
    }
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
    WiFi.macAddress(myMac);
    Serial.printf("[ESPNOW] Listo — MAC: %s\n", WiFi.macAddress().c_str());
}

// ── Setup RGB ────────────────────────────────────────────────────
void setupRGBLed() {
    FastLED.addLeds<WS2812B, PIN_RGB, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(60);
    Serial.println("[RGB] FastLED listo");
}

// ── Audio ────────────────────────────────────────────────────────
void setupAudio() {
  Serial.print(F("[AUDIO] Initializing DFPlayer ... "));
  dfSerial.begin(9600, SERIAL_8N1, 16, 17);
  if (!myDFPlayer.begin(dfSerial, /*isACK = */false, /*doReset = */true)) { 
    Serial.println(F(" Please recheck the connection or insert SD"));
    while(true){
      delay(0); 
    }
  }
  delay(1000);
  Serial.println(F("DFPlayer Mini online."));
  myDFPlayer.volume(sound_volume); 
  delay(100);
}

void reproducirEyeccion() {
    detener();             
    delay(200);            
    playAudio(3, 1000);    // Seguro, solo encola tras estabilizar voltaje
}

// ── Parqueo ──────────────────────────────────────────────────────
void setParqueo(bool parqueado) {
    if (enParqueo == parqueado) return;
    enParqueo = parqueado;
    if (parqueado) {
        digitalWrite(PIN_LED_TRASERO, HIGH);
        Serial.println("[PARQUEO] Activado");
    } else {
        digitalWrite(PIN_LED_TRASERO, LOW);
        Serial.println("[PARQUEO] Desactivado");
    }
}

// ── LEDs ─────────────────────────────────────────────────────────
// ── Reemplaza por completo esta sección en tu código ─────────────────────────
void updateLEDs() {
    unsigned long now = millis();
    static unsigned long lastBlinkPendiente = 0;
    static bool          statePendiente     = false;
    static unsigned long lastBlink          = 0;
    static bool          ledState           = false;
    static unsigned long lastEfectoBlink    = 0;
    static bool          efectoBlinkState   = false;

    // --- AUTO-RESET DE EFECTO EXPIRADO ---
    // Si el tiempo del efecto ya pasó, lo limpiamos antes de evaluar prioridades
    if (efectoActual != "NONE" && now >= efectoFin) {
        resetEfectos();
    }

    // Prioridad 0 — Efecto activo (Parpadeo dinámico por tipo de efecto)
    if (efectoActual != "NONE") {
        unsigned long intervalo = 500;  // Por defecto

        if      (efectoActual == "RED")    intervalo = 150;  // Blink rápido — Peligro
        else if (efectoActual == "YELLOW") intervalo = 300;  // Blink medio — Confusión
        else if (efectoActual == "BLUE")   intervalo = 600;  // Blink lento — Lentitud
        else if (efectoActual == "GREEN")  intervalo = 400;  // Blink medio-rápido

        if (now - lastEfectoBlink > intervalo) {
            lastEfectoBlink = now;
            efectoBlinkState = !efectoBlinkState;
            
            // Forzamos la conversión limpia de String a std::string para el mapa de colores
            std::string efectoStr(efectoActual.c_str());
            sendColor(efectoBlinkState ? getColor(efectoStr) : getColor("OFF"));
        }
        return; // Salimos para que no interfieran las otras prioridades
    }

    // Resetear el estado interno si no hay efecto
    efectoBlinkState = false;

    // Prioridad 1 — Poder otorgado pendiente (Parpadeo rápido de notificación)
    if (colorPendiente) {
        if (now - lastBlinkPendiente > 200) {
            lastBlinkPendiente = now;
            statePendiente     = !statePendiente;
            sendColor(statePendiente ? colorNuevo : getColor("OFF"));
        }
        // Dejar de parpadear cuando expire colorHasta
        if (now > colorHasta) {
            colorPendiente  = false;
            efectoPendiente = false;
            sendColor(getColor("OFF"));
        }
        return;
    }

    // Prioridad 2 — Temporizador de color fijo (Si aplica)
    if (colorHasta > 0) {
        if (now < colorHasta) return;
        colorHasta = 0;
        sendColor(getColor("OFF"));
        return;
    }

    // Prioridad 3 — Parqueo (Parpadeo amarillo 500ms)
    if (enParqueo) {
        if (now - lastBlink > 500) {
            lastBlink = now;
            ledState  = !ledState;
            sendColor(ledState ? getColor("YELLOW") : getColor("OFF"));
        }
        return;
    }

    // Prioridad 4 — En movimiento sin efectos activos
    sendColor(getColor("OFF"));
}

// ── Blink arranque ───────────────────────────────────────────────
void blink(int times) {
    for (int i = 0; i < times; i++) {
        sendColor(getColor("BLUE"));
        delay(200);
        sendColor(getColor("OFF"));
        delay(200);
    }
    delay(500);
}

// ── Setup ────────────────────────────────────────────────────────
void setup() { 
    pinMode(motorA1, OUTPUT); digitalWrite(motorA1, LOW);
    pinMode(motorA2, OUTPUT); digitalWrite(motorA2, LOW);
    pinMode(motorB1, OUTPUT); digitalWrite(motorB1, LOW);
    pinMode(motorB2, OUTPUT); digitalWrite(motorB2, LOW);
    
    setupRGBLed();    
    // 2. ESPERA DE ESTABILIZACIÓN
    // Le damos segundos para que el voltaje se recupere tras el chispazo del switch
    
    delay(2500); 
    Serial.begin(115200);
    
    Serial.println("\n=== SISTEMA ESTABILIZADO ===");
    randomSeed(analogRead(34));
    Serial.println("\n=== BOOT ===");

    pinMode(vaporizador,      OUTPUT);
    pinMode(PIN_EYECCION,     INPUT_PULLUP);
    pinMode(PIN_LED_TRASERO,  OUTPUT);
    Serial.println("[PINES] Configurados OK");

    blink(1);
    BP32.setup(&onConnectedGamepad, &onDisconnectedGamepad);
    Serial.println("[BP32] Listo — esperando mando");
    blink(2);
    setupEspNow();
    blink(3);
    setupAudio();
    
    digitalWrite(PIN_LED_TRASERO, HIGH);
    enParqueo = true;
   
}

// ── Eyección polling ─────────────────────────────────────────────
void checkEyeccion() {
    static unsigned long lastCheck   = 0;
    static unsigned long lastTrigger = 0;
    if (millis() - lastCheck < 2) return;
    lastCheck = millis();
    if (digitalRead(PIN_EYECCION) == LOW) {
        if (millis() - lastTrigger > 500) {
            eyeccionDetectada = true;
            lastTrigger       = millis();
            Serial.println("[EYECCION] Pulso detectado");
        }
    }
}

// ── Loop ─────────────────────────────────────────────────────────
void loop() {
    BP32.update();
    checkEyeccion();
    
    // Procesa peticiones de audio generadas desde callbacks/funciones
    checkPistasPendientes(); 
    checkAudio();  
    
    updateLEDs();
    processQueue();
    
    // Eyección
    if (eyeccionDetectada) {
        eyeccionDetectada = false;
        setParqueo(true);
        Serial.println("[EYECCION] Piloto expulsado");
        if (mandoPrincipal && mandoPrincipal->isConnected()) {
            mandoPrincipal->setRumble(0xff, 0xff);
            rumbleActive = true;
            rumbleEnd    = millis() + 2000;
        }
        reproducirEyeccion();
    }

    // Rumble
    if (rumbleActive && millis() > rumbleEnd) {
        if (mandoPrincipal) mandoPrincipal->setRumble(0, 0);
        rumbleActive = false;
        Serial.println("[MANDO] Rumble apagado");
    }

    // Giro exacto
    if (giroExactoActivo) ejecutarGiroExacto();

    // Mando cada 20ms
    static unsigned long lastProcess = 0;
    if (millis() - lastProcess >= 20) {
        lastProcess = millis();
        if (mandoPrincipal && mandoPrincipal->isConnected() && !giroExactoActivo) {
            processGamepad(mandoPrincipal);
        }
    }

    // Debug cada 2 segundos
    if (millis() - lastDebug > 2000) {
        lastDebug = millis();
        Serial.printf("[LOOP] Mando:%s Modo:%s Parqueo:%s Efecto:%s Poder:%s\n",
            mandoPrincipal ? "SI" : "NO",
            modoDualJoystick ? "DUAL" : "SINGLE",
            enParqueo ? "SI" : "NO",
            efectoActual.c_str(),
            grantedPow
            );
    }
}

// ── Bluepad32 callbacks ──────────────────────────────────────────
void onConnectedGamepad(GamepadPtr gp) {
    Serial.println("[BT] Conexión recibida");
    if (mandoPrincipal != nullptr) {
        Serial.println("[BT] Rechazado — ya hay un mando activo");
        gp->disconnect();
        return;
    }
    mandoPrincipal = gp;
    Serial.printf("[BT] Modelo: %s conectado de forma exitosa.\n", gp->getModelName());
    
    // Modo inicial por defecto al conectar
    modoDualJoystick = true; 
}

void onDisconnectedGamepad(GamepadPtr gp) {
    Serial.println("[BT] Mando desconectado");
    if (mandoPrincipal == gp) {
        mandoPrincipal = nullptr;
        detener();
        setParqueo(true);
        Serial.println("[BT] Mando liberado — carro detenido");
    }
}
 //Configurar modo mando
 bool startPreviamentePresionado = false;

void chequearConfigMando(GamepadPtr gp) {
    if (gp == nullptr) return;

    // Puedes probar cambiando 0x04 por el número que use tu control (ej: 1, 2, 4 u 8)
    // O usar la constante si el compilador ya no te da error.
    bool startPresionado = (gp->miscButtons() & 0x04) || (gp->miscButtons() & 0x02); 

    if (startPresionado) {
        if (!startPreviamentePresionado) {
            // ¡El botón START acaba de ser presionado! Alternamos el modo:
            modoDualJoystick = !modoDualJoystick; 
            
            if (!modoDualJoystick) { // Pasó a Modo SINGLE
                Serial.println("[BT] Cambiado a: Modo SINGLE activado");
                gp->setRumble(0xff, 0xff); // Vibración fuerte
                rumbleActive = true;
                rumbleEnd    = millis() + 1000;
            } else { // Pasó a Modo DUAL
                Serial.println("[BT] Cambiado a: Modo DUAL activado");
                gp->setRumble(0xc0, 0x40); // Vibración suave
                rumbleActive = true;
                rumbleEnd    = millis() + 500;
            }
            startPreviamentePresionado = true; // Bloqueamos hasta que suelte el botón
        }
    } else {
        startPreviamentePresionado = false; // Se libera cuando el usuario suelta el botón
    }
}

 // ── Volume control with triggers ─────────────────────────────
 void setVolume(GamepadPtr gp){
    static bool l2Anterior = false;
    static bool r2Anterior = false;
    bool l2Ahora = gp->l2();  // left trigger  = volume down
    bool r2Ahora = gp->r2();  // right trigger = volume up

    if (l2Ahora && !l2Anterior) {
        myDFPlayer.volumeDown();
        Serial.println("[AUDIO] Volumen -1");
    }
    if (r2Ahora && !r2Anterior) {
        myDFPlayer.volumeUp();
        Serial.println("[AUDIO] Volumen +1");
    }
    l2Anterior = l2Ahora;
    r2Anterior = r2Ahora;
 }

// ── Dispatcher ───────────────────────────────────────────────────
void processGamepad(GamepadPtr gp) {
    setVolume(gp);
    if (millis() > efectoFin && efectoActual != "NONE") resetEfectos();

    if (motorBloqueado) { detener(); return; }

    if (gp->l1() || gp->r1() || abs(gp->axisY()) > 50 || abs(gp->axisX()) > 50) {
        setParqueo(false);
    }

    static bool l1Anterior = false;
    static bool r1Anterior = false;
    bool l1Ahora = gp->l1();
    bool r1Ahora = gp->r1();

    if (l1Ahora && !l1Anterior) { iniciarGiroExacto(1);  l1Anterior = l1Ahora; r1Anterior = r1Ahora; return; }
    if (r1Ahora && !r1Anterior) { iniciarGiroExacto(-1); l1Anterior = l1Ahora; r1Anterior = r1Ahora; return; }
    
    l1Anterior = l1Ahora;
    r1Anterior = r1Ahora;

    int throttle = modoDualJoystick ? -(gp->axisY())  : -(gp->axisY());
    int steer    = modoDualJoystick ? -(gp->axisRX()) : -(gp->axisX());

    if (controlesInvertidos) { throttle = -throttle; steer = -steer; }

    throttle = (int)(throttle * multiplicadorVelocidad);
    steer    = (int)(steer    * multiplicadorVelocidad);

    bool efectoGreen = (efectoActual == "GREEN");
    chequearConfigMando(mandoPrincipal);
    if (modoDualJoystick) processMododualJoystick(throttle, steer);
    else                  processModoSingleJoystick(throttle, steer);

    if (efectoGreen) {
        analogWrite(motorA1, 0);
        analogWrite(motorA2, 0);
    }

    digitalWrite(vaporizador, gp->a() ? HIGH : LOW);
    if (gp->a()) Serial.println("[VAPOR] Activado");

    if (gp->b()) throwPow();
}

// ── Modo Single ──────────────────────────────────────────────────
void processModoSingleJoystick(int throttle, int steer) {
    if (abs(throttle) < 50) throttle = 0;
    if (abs(steer)    < 50) steer    = 0;
    if (throttle != 0 || steer != 0) setParqueo(false);

    if (throttle == 0 && steer != 0) {
        int turnSpeed = map(abs(steer), 50, 512, 50, 200);
        if (steer > 0) {
            analogWrite(motorA1, turnSpeed); analogWrite(motorA2, 0);
            analogWrite(motorB1, 0);         analogWrite(motorB2, turnSpeed);
        } else {
            analogWrite(motorA1, 0);         analogWrite(motorA2, turnSpeed);
            analogWrite(motorB1, turnSpeed); analogWrite(motorB2, 0);
        }
        return;
    }
    if (throttle == 0 && steer == 0) { detener(); return; }

    int speed      = map(abs(throttle), 50, 512, 50, 255);
    int turnOffset = map(abs(steer),     0, 512,  0, speed);
    int leftSpeed  = speed;
    int rightSpeed = speed;
    if      (steer > 0) rightSpeed = constrain(speed - turnOffset, 0, 255);
    else if (steer < 0) leftSpeed  = constrain(speed - turnOffset, 0, 255);

    if (throttle > 0) {
        analogWrite(motorA1, leftSpeed);  analogWrite(motorA2, 0);
        analogWrite(motorB1, rightSpeed); analogWrite(motorB2, 0);
    } else {
        analogWrite(motorA1, 0);          analogWrite(motorA2, leftSpeed);
        analogWrite(motorB1, 0);          analogWrite(motorB2, rightSpeed);
    }
}

// ── Modo Dual ────────────────────────────────────────────────────
void processMododualJoystick(int throttle, int steer) {
    if (abs(throttle) < 50) throttle = 0;
    if (abs(steer)    < 50) steer    = 0;
    if (throttle != 0 || steer != 0) setParqueo(false);

    if (throttle == 0 && steer != 0) {
        int turnSpeed = map(abs(steer), 50, 512, 50, 200);
        if (steer > 0) {
            analogWrite(motorA1, turnSpeed); analogWrite(motorA2, 0);
            analogWrite(motorB1, 0);         analogWrite(motorB2, turnSpeed);
        } else {
            analogWrite(motorA1, 0);         analogWrite(motorA2, turnSpeed);
            analogWrite(motorB1, turnSpeed); analogWrite(motorB2, 0);
        }
        return;
    }
    if (throttle == 0 && steer == 0) { detener(); return; }

    int speed      = map(abs(throttle), 50, 512, 50, 255);
    int turnOffset = map(abs(steer),     0, 512,  0, speed);
    int leftSpeed  = speed;
    int rightSpeed = speed;
    if      (steer > 0) rightSpeed = constrain(speed - turnOffset, 0, 255);
    else if (steer < 0) leftSpeed  = constrain(speed - turnOffset, 0, 255);

    if (throttle > 0) {
        analogWrite(motorA1, leftSpeed);  analogWrite(motorA2, 0);
        analogWrite(motorB1, rightSpeed); analogWrite(motorB2, 0);
    } else {
        analogWrite(motorA1, 0);          analogWrite(motorA2, leftSpeed);
        analogWrite(motorB1, 0);          analogWrite(motorB2, rightSpeed);
    }
}

// ── Giro exacto ──────────────────────────────────────────────────
void iniciarGiroExacto(int direccion) {
    if (giroExactoActivo) return;
    giroExactoActivo    = true;
    giroExactoFin       = millis() + MS_POR_45_GRADOS;
    giroExactoDireccion = direccion;
    Serial.printf("[GIRO] Iniciando 45° %s por %lu ms\n",
        direccion > 0 ? "derecha" : "izquierda", MS_POR_45_GRADOS);
}

void ejecutarGiroExacto() {
    if (!giroExactoActivo) return;
    if (millis() < giroExactoFin) {
        static unsigned long lastGiro = 0;
        if (millis() - lastGiro < 20) return;
        lastGiro = millis();
        if (giroExactoDireccion > 0) {
            analogWrite(motorA1, VELOCIDAD_GIRO_EXACTO); analogWrite(motorA2, 0);
            analogWrite(motorB1, 0); analogWrite(motorB2, VELOCIDAD_GIRO_EXACTO);
        } else {
            analogWrite(motorA1, 0); analogWrite(motorA2, VELOCIDAD_GIRO_EXACTO);
            analogWrite(motorB1, VELOCIDAD_GIRO_EXACTO); analogWrite(motorB2, 0);
        }
    } else {
        detener();
        giroExactoActivo = false;
        Serial.println("[GIRO] Completado");
    }
}

// ── Detener ──────────────────────────────────────────────────────
void detener() {
    analogWrite(motorA1, 0); analogWrite(motorA2, 0);
    analogWrite(motorB1, 0); analogWrite(motorB2, 0);
}