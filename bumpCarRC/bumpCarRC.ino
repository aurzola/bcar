// ════════════════════════════════════════════════════════════════
//  CARRO RC — Firmware v2
//  Mejoras: funciones duplicadas unificadas, parseo extraído,
//  control de motores centralizado, bugs de modo dual corregidos.
// ════════════════════════════════════════════════════════════════

#include <Bluepad32.h>
#include "esp_wifi.h"
#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include "DFRobotDFPlayerMini.h"
#include <map>
#include <string>
#include <FastLED.h>

// ── Hardware serial para DFPlayer ────────────────────────────────
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini myDFPlayer;

// ── Pines ────────────────────────────────────────────────────────
const int PIN_MOTOR_A1    = 26;   // IN4
const int PIN_MOTOR_A2    = 33;   // IN3
const int PIN_MOTOR_B1    = 14;   // IN1
const int PIN_MOTOR_B2    = 27;   // IN2
const int PIN_VAPORIZADOR = 4;
const int PIN_EYECCION    = 19;
const int PIN_LED_TRASERO = 13;
const int PIN_RGB         = 23;

// ── Constantes de audio ──────────────────────────────────────────
#define TRACKS_COUNT   34
int sound_volume = 25;

// ── Constantes de giro exacto ────────────────────────────────────
const unsigned long MS_POR_90_GRADOS    = 400;
const unsigned long MS_POR_45_GRADOS    = MS_POR_90_GRADOS / 2;
const int           VELOCIDAD_GIRO      = 180;

// ── FastLED ──────────────────────────────────────────────────────
#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

// ── Colores disponibles ──────────────────────────────────────────
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

// ── Estado de audio (no bloqueante) ─────────────────────────────
bool          audioActivo     = false;
unsigned long audioFin        = 0;
int           audioPista      = 0;

volatile int           pistaPendiente    = 0;   // 0 = ninguna
volatile unsigned long duracionPendiente = 0;

// ── Estado RGB ───────────────────────────────────────────────────
volatile bool colorPendiente  = false;
volatile bool efectoPendiente = false;
CRGB          colorNuevo      = CRGB::Black;
unsigned long colorHasta      = 0;

// ── Estado global del carro ──────────────────────────────────────
GamepadPtr    mandoPrincipal      = nullptr;
bool          rumbleActive        = false;
unsigned long rumbleEnd           = 0;
unsigned long lastDebug           = 0;
bool          modoDualJoystick    = false;
bool          giroExactoActivo    = false;
unsigned long giroExactoFin       = 0;
int           giroExactoDireccion = 0;
bool          enParqueo           = true;
volatile bool eyeccionDetectada   = false;
String        grantedPow          = "NONE";

// ── Efectos activos ──────────────────────────────────────────────
unsigned long grantEffectDuration  = 10000;
unsigned long efectoFin            = 0;
String        efectoActual         = "NONE";
bool          controlesInvertidos  = false;
bool          motorBloqueado       = false;
float         multiplicadorVelocidad = 1.0;

// ── ESP-NOW ──────────────────────────────────────────────────────
#define GRANT_CMD        "GRANT:"
#define APPLY_CMD        "APPLY:"
#define ESPNOW_MAX_RETRY 12
#define MAX_FALLOS       20

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t myMac[6];

// ── Peers conocidos ──────────────────────────────────────────────
#define MAX_PEERS 6

struct PeerEntry {
    uint8_t mac[6];
    int     fallos;
    char    nombre[16];
};

PeerEntry peers[MAX_PEERS] = {
    {{0x1C, 0xC3, 0xAB, 0xC3, 0xD0, 0xE8}, 0, "CARRO_1"},
    {{0x24, 0x6F, 0x28, 0x96, 0x03, 0x08}, 0, "CARRO_2"},
    {{0x1C, 0xC3, 0xAB, 0xF9, 0x2C, 0xE0}, 0, "CARRO_3"},
    {{0x1C, 0xC3, 0xAB, 0xFA, 0x0C, 0x5C}, 0, "CARRO NARANJA"},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 99, ""},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 99, ""},
};

// ── Cola de mensajes pendientes ──────────────────────────────────
#define MAX_PENDING 8

struct PendingMsg {
    uint8_t mac[6];
    char    msg[32];
    int     intentos;
    bool    confirmado;
};

PendingMsg pendingQueue[MAX_PENDING];
int        pendingCount = 0;


// ════════════════════════════════════════════════════════════════
//  HELPERS BÁSICOS
// ════════════════════════════════════════════════════════════════

bool starts_with(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

// ── LED RGB ──────────────────────────────────────────────────────
CRGB getColor(const std::string& name) {
    auto it = colorMap.find(name);
    return (it != colorMap.end()) ? it->second : CRGB::Black;
}

void sendColor(CRGB color) {
    leds[0] = color;
    FastLED.show();
}

// Traduce letra de protocolo ('R','G','B','Y') al nombre de color
const char* letraAColor(char letra) {
    switch (letra) {
        case 'R': return "RED";
        case 'G': return "GREEN";
        case 'B': return "BLUE";
        case 'Y': return "YELLOW";
        default:  return "OFF";
    }
}

// ── Motores — punto único de escritura ──────────────────────────
//   lado izquierdo = motorA, lado derecho = motorB
void setMotores(int velIzq, int velDer, bool adelante) {
    if (adelante) {
        analogWrite(PIN_MOTOR_A1, velIzq); analogWrite(PIN_MOTOR_A2, 0);
        analogWrite(PIN_MOTOR_B1, velDer); analogWrite(PIN_MOTOR_B2, 0);
    } else {
        analogWrite(PIN_MOTOR_A1, 0); analogWrite(PIN_MOTOR_A2, velIzq);
        analogWrite(PIN_MOTOR_B1, 0); analogWrite(PIN_MOTOR_B2, velDer);
    }
}

void detener() {
    analogWrite(PIN_MOTOR_A1, 0); analogWrite(PIN_MOTOR_A2, 0);
    analogWrite(PIN_MOTOR_B1, 0); analogWrite(PIN_MOTOR_B2, 0);
}

// ── Giro en sitio (izq negativo, der positivo) ───────────────────
void girarEnSitio(int velGiro, int direccion) {
    if (direccion > 0) {   // derecha: rueda izq adelante, rueda der atrás
        analogWrite(PIN_MOTOR_A1, velGiro); analogWrite(PIN_MOTOR_A2, 0);
        analogWrite(PIN_MOTOR_B1, 0);       analogWrite(PIN_MOTOR_B2, velGiro);
    } else {               // izquierda
        analogWrite(PIN_MOTOR_A1, 0);       analogWrite(PIN_MOTOR_A2, velGiro);
        analogWrite(PIN_MOTOR_B1, velGiro); analogWrite(PIN_MOTOR_B2, 0);
    }
}


// ════════════════════════════════════════════════════════════════
//  AUDIO (no bloqueante)
// ════════════════════════════════════════════════════════════════

// Encola una pista: el hardware se accede desde el loop principal
void playAudio(int pista, unsigned long duracionMs) {
    pistaPendiente    = pista;
    duracionPendiente = duracionMs;
}

void checkPistasPendientes() {
    if (pistaPendiente == 0) return;

    // Lectura atómica de los volátiles
    int           pista    = pistaPendiente;
    unsigned long duracion = duracionPendiente;
    pistaPendiente    = 0;
    duracionPendiente = 0;

    myDFPlayer.stop();
    delay(100);
    myDFPlayer.play(pista);

    audioActivo = true;
    audioFin    = millis() + duracion;
    audioPista  = pista;
    Serial.printf("[AUDIO] Reproduciendo pista %d\n", pista);
}

void checkAudio() {
    if (!audioActivo) return;
    if (millis() > audioFin) {
        audioActivo = false;
        audioPista  = 0;
        Serial.println("[AUDIO] Pista finalizada");
    }
}

void reproducirEyeccion() {
    detener();
    delay(200);
    playAudio(3, 1000);
}


// ════════════════════════════════════════════════════════════════
//  EFECTOS / ESTADO
// ════════════════════════════════════════════════════════════════

void resetEfectos() {
    efectoActual           = "NONE";
    controlesInvertidos    = false;
    motorBloqueado         = false;
    multiplicadorVelocidad = 1.0;
    Serial.println("[EFECTO] Reseteados — control normal");
}

void procesarEfecto(const char* nombreEfecto) {
    resetEfectos();
    efectoActual = nombreEfecto;
    efectoFin    = millis() + grantEffectDuration;

    if      (strcmp(nombreEfecto, "RED")    == 0) motorBloqueado      = true;
    else if (strcmp(nombreEfecto, "YELLOW") == 0) controlesInvertidos = true;
    // GREEN y BLUE tienen lógica adicional gestionada por el llamador

    Serial.printf("[EFECTO] Aplicando: %s por %lu ms\n", nombreEfecto, grantEffectDuration);
}


// ════════════════════════════════════════════════════════════════
//  PARQUEO Y LED TRASERO
// ════════════════════════════════════════════════════════════════

void setParqueo(bool parqueado) {
    if (enParqueo == parqueado) return;
    enParqueo = parqueado;
    digitalWrite(PIN_LED_TRASERO, parqueado ? HIGH : LOW);
    Serial.printf("[PARQUEO] %s\n", parqueado ? "Activado" : "Desactivado");
}


// ════════════════════════════════════════════════════════════════
//  LED RGB — ACTUALIZACIÓN NO BLOQUEANTE
// ════════════════════════════════════════════════════════════════

void updateLEDs() {
    unsigned long now = millis();

    static unsigned long lastBlinkPendiente = 0;
    static bool          statePendiente     = false;
    static unsigned long lastBlink          = 0;
    static bool          ledState           = false;
    static unsigned long lastEfectoBlink    = 0;
    static bool          efectoBlinkState   = false;

    // Auto-reset de efecto expirado
    if (efectoActual != "NONE" && now >= efectoFin) {
        resetEfectos();
    }

    // Prioridad 0 — Efecto activo (parpadeo dinámico)
    if (efectoActual != "NONE") {
        unsigned long intervalo = 500;
        if      (efectoActual == "RED")    intervalo = 150;
        else if (efectoActual == "YELLOW") intervalo = 300;
        else if (efectoActual == "BLUE")   intervalo = 600;
        else if (efectoActual == "GREEN")  intervalo = 400;

        if (now - lastEfectoBlink > intervalo) {
            lastEfectoBlink  = now;
            efectoBlinkState = !efectoBlinkState;
            sendColor(efectoBlinkState
                ? getColor(std::string(efectoActual.c_str()))
                : getColor("OFF"));
        }
        return;
    }

    efectoBlinkState = false;

    // Prioridad 1 — Poder otorgado pendiente (parpadeo rápido)
    if (colorPendiente) {
        if (now - lastBlinkPendiente > 200) {
            lastBlinkPendiente = now;
            statePendiente     = !statePendiente;
            sendColor(statePendiente ? colorNuevo : getColor("OFF"));
        }
        if (now > colorHasta) {
            colorPendiente  = false;
            efectoPendiente = false;
            sendColor(getColor("OFF"));
        }
        return;
    }

    // Prioridad 2 — Color fijo con temporizador
    if (colorHasta > 0) {
        if (now < colorHasta) return;
        colorHasta = 0;
        sendColor(getColor("OFF"));
        return;
    }

    // Prioridad 3 — Parqueo (parpadeo amarillo 500 ms)
    if (enParqueo) {
        if (now - lastBlink > 500) {
            lastBlink = now;
            ledState  = !ledState;
            sendColor(ledState ? getColor("YELLOW") : getColor("OFF"));
        }
        return;
    }

    // Prioridad 4 — Circulando sin efectos
    sendColor(getColor("OFF"));
}

// Blink de arranque (bloqueante, solo en setup)
void blink(int times) {
    for (int i = 0; i < times; i++) {
        sendColor(getColor("BLUE")); delay(200);
        sendColor(getColor("OFF"));  delay(200);
    }
    delay(500);
}


// ════════════════════════════════════════════════════════════════
//  ESP-NOW — COLA DE MENSAJES
// ════════════════════════════════════════════════════════════════

void enqueue(const uint8_t* mac, const char* msg) {
    if (pendingCount >= MAX_PENDING) {
        Serial.println("[QUEUE] Cola llena — descartando");
        return;
    }
    PendingMsg& p = pendingQueue[pendingCount++];
    memcpy(p.mac, mac, 6);
    strncpy(p.msg, msg, sizeof(p.msg) - 1);
    p.msg[sizeof(p.msg) - 1] = '\0';
    p.intentos   = 0;
    p.confirmado = false;
    Serial.printf("[QUEUE] Encolado: %s (%d en cola)\n", msg, pendingCount);
}

void dequeue(const uint8_t* mac) {
    for (int i = 0; i < pendingCount; i++) {
        if (memcmp(pendingQueue[i].mac, mac, 6) != 0) continue;
        Serial.printf("[QUEUE] Confirmado para %02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        for (int j = i; j < pendingCount - 1; j++)
            pendingQueue[j] = pendingQueue[j + 1];
        pendingCount--;
        return;
    }
}

void ensurePeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t info;
    memset(&info, 0, sizeof(info));
    memcpy(info.peer_addr, mac, 6);
    info.channel = 1;
    info.encrypt = false;
    esp_now_add_peer(&info);
}

void processQueue() {
    if (pendingCount == 0) return;
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry < 60) return;
    lastRetry = millis();

    for (int i = 0; i < pendingCount; i++) {
        PendingMsg& p = pendingQueue[i];

        if (p.intentos >= ESPNOW_MAX_RETRY) {
            // Penalizar al peer correspondiente
            for (int j = 0; j < MAX_PEERS; j++) {
                if (memcmp(peers[j].mac, p.mac, 6) != 0) continue;
                peers[j].fallos++;
                Serial.printf("[PEER] %s falló — total fallos: %d\n",
                    peers[j].nombre, peers[j].fallos);
                break;
            }
            for (int j = i; j < pendingCount - 1; j++)
                pendingQueue[j] = pendingQueue[j + 1];
            pendingCount--;
            i--;
            continue;
        }

        ensurePeer(p.mac);
        esp_now_send(p.mac, (uint8_t*)p.msg, strlen(p.msg));
        p.intentos++;
    }
}

// ── Callbacks ESP-NOW ────────────────────────────────────────────
void OnDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) return;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (memcmp(peers[i].mac, mac, 6) != 0) continue;
        peers[i].fallos = 0;
        break;
    }
    Serial.printf("[ESPNOW] Entregado a %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    dequeue(mac);
}

// Parsea "LETRA:MS:MULTIPLICADOR" de forma segura
void parsearPayload(const char* payload,
                    char& letra, unsigned long& ms, float& mult) {
    char buf[64];
    strncpy(buf, payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* tok = strtok(buf, ":");
    letra = (tok && tok[0]) ? tok[0] : 'X';

    tok = strtok(NULL, ":");
    ms  = tok ? strtoul(tok, NULL, 10) : grantEffectDuration;

    tok  = strtok(NULL, ":");
    mult = tok ? atof(tok) : 1.0f;
}

void procesarGrant(const char* payload) {
    char letra; unsigned long ms; float mult;
    parsearPayload(payload, letra, ms, mult);

    grantedPow          = String(payload);
    grantEffectDuration = ms;

    // LED local de notificación
    colorNuevo     = getColor(letraAColor(letra));
    colorHasta     = millis() + ms;
    colorPendiente = true;

    // Si es BLUE, aplicamos el multiplicador ya al carro receptor
    if (letra == 'B') multiplicadorVelocidad = mult;

    pistaPendiente    = 2;
    duracionPendiente = 5000;

    Serial.printf("[GRANT] Poder otorgado: %s\n", grantedPow.c_str());
}

void procesarApply(const char* payload) {
    char letra; unsigned long ms; float mult;
    parsearPayload(payload, letra, ms, mult);

    const char* nombreEfecto = letraAColor(letra);
    if (strcmp(nombreEfecto, "OFF") == 0) {
        Serial.printf("[APPLY] Letra desconocida: %c\n", letra);
        return;
    }

    grantEffectDuration = ms;
    procesarEfecto(nombreEfecto);

    if (letra == 'B') multiplicadorVelocidad = mult;

    Serial.printf("[APPLY] Efecto: %s — mult: %.2f\n", nombreEfecto, multiplicadorVelocidad);
}

void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char buffer[64];
    int  safeLen = min(len, (int)sizeof(buffer) - 1);
    memcpy(buffer, data, safeLen);
    buffer[safeLen] = '\0';

    Serial.printf("[ESPNOW] Recibido: %s\n", buffer);

    if      (starts_with(buffer, GRANT_CMD)) procesarGrant(buffer + strlen(GRANT_CMD));
    else if (starts_with(buffer, APPLY_CMD)) procesarApply(buffer + strlen(APPLY_CMD));
    else    Serial.printf("[ESPNOW] Comando no reconocido: %s\n", buffer);
}


// ════════════════════════════════════════════════════════════════
//  LANZAR PODER (throwPow)
// ════════════════════════════════════════════════════════════════

void throwPow() {
    if (grantedPow == "NONE") {
        if (audioPista < 10) playAudio(1, 700);
        return;
    }

    char msg[32];
    snprintf(msg, sizeof(msg), "APPLY:%s", grantedPow.c_str());

    int encolados = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].fallos >= MAX_FALLOS) continue;
        if (memcmp(peers[i].mac, myMac, 6) == 0) continue; // No atacarse a sí mismo
        enqueue(peers[i].mac, msg);
        encolados++;
    }
    Serial.printf("[POWER] Ataque: %s → %d carros\n", msg, encolados);

    grantedPow     = "NONE";
    colorPendiente = false;

    playAudio(random(10, TRACKS_COUNT), 10000);

    if (mandoPrincipal && mandoPrincipal->isConnected()) {
        mandoPrincipal->setRumble(0xFF, 0xFF);
        rumbleActive = true;
        rumbleEnd    = millis() + 500;
    }
}


// ════════════════════════════════════════════════════════════════
//  CONTROL DE MOVIMIENTO
// ════════════════════════════════════════════════════════════════

// Función unificada para Single y Dual (eran idénticas en la versión original)
void procesarMovimiento(int throttle, int steer) {
    if (abs(throttle) < 50) throttle = 0;
    if (abs(steer)    < 50) steer    = 0;
    if (throttle != 0 || steer != 0) setParqueo(false);

    if (throttle == 0 && steer == 0) { detener(); return; }

    // Giro en sitio cuando no hay throttle
    if (throttle == 0) {
        int vel = map(abs(steer), 50, 512, 50, 200);
        girarEnSitio(vel, steer > 0 ? 1 : -1);
        return;
    }

    int speed      = map(abs(throttle), 50, 512, 50, 255);
    int turnOffset = map(abs(steer),     0, 512,  0, speed);
    int velIzq     = speed;
    int velDer     = speed;
    if      (steer > 0) velDer = constrain(speed - turnOffset, 0, 255);
    else if (steer < 0) velIzq = constrain(speed - turnOffset, 0, 255);

    setMotores(velIzq, velDer, throttle > 0);
}

// ── Giro exacto ──────────────────────────────────────────────────
void iniciarGiroExacto(int direccion) {
    if (giroExactoActivo) return;
    giroExactoActivo    = true;
    giroExactoFin       = millis() + MS_POR_45_GRADOS;
    giroExactoDireccion = direccion;
    Serial.printf("[GIRO] 45° %s por %lu ms\n",
        direccion > 0 ? "derecha" : "izquierda", MS_POR_45_GRADOS);
}

void ejecutarGiroExacto() {
    if (!giroExactoActivo) return;

    static unsigned long lastGiro = 0;
    if (millis() - lastGiro < 20) return;
    lastGiro = millis();

    if (millis() < giroExactoFin) {
        girarEnSitio(VELOCIDAD_GIRO, giroExactoDireccion);
    } else {
        detener();
        giroExactoActivo = false;
        Serial.println("[GIRO] Completado");
    }
}


// ════════════════════════════════════════════════════════════════
//  GAMEPAD — PROCESAMIENTO
// ════════════════════════════════════════════════════════════════

bool startPreviamentePresionado = false;

void chequearConfigMando(GamepadPtr gp) {
    if (!gp) return;
    bool startPresionado = (gp->miscButtons() & 0x04) || (gp->miscButtons() & 0x02);

    if (startPresionado && !startPreviamentePresionado) {
        modoDualJoystick = !modoDualJoystick;
        if (modoDualJoystick) {
            Serial.println("[BT] Modo DUAL activado");
            gp->setRumble(0xC0, 0x40);
            rumbleEnd = millis() + 500;
        } else {
            Serial.println("[BT] Modo SINGLE activado");
            gp->setRumble(0xFF, 0xFF);
            rumbleEnd = millis() + 1000;
        }
        rumbleActive = true;
    }
    startPreviamentePresionado = startPresionado;
}

void setVolume(GamepadPtr gp) {
    static bool l2Ant = false, r2Ant = false;
    bool l2 = gp->l2(), r2 = gp->r2();
    if (l2 && !l2Ant) { myDFPlayer.volumeDown(); Serial.println("[AUDIO] Vol -1"); }
    if (r2 && !r2Ant) { myDFPlayer.volumeUp();   Serial.println("[AUDIO] Vol +1"); }
    l2Ant = l2; r2Ant = r2;
}

void processGamepad(GamepadPtr gp) {
    setVolume(gp);

    // Auto-reset de efecto expirado
    if (efectoActual != "NONE" && millis() > efectoFin) resetEfectos();

    if (motorBloqueado) { detener(); return; }

    // Salir de parqueo con cualquier input de movimiento
    if (gp->l1() || gp->r1() || abs(gp->axisY()) > 50 || abs(gp->axisX()) > 50)
        setParqueo(false);

    // Giro exacto con bumpers (edge-triggered)
    static bool l1Ant = false, r1Ant = false;
    bool l1 = gp->l1(), r1 = gp->r1();
    if (l1 && !l1Ant) { l1Ant = l1; r1Ant = r1; iniciarGiroExacto(1); return; }
    if (r1 && !r1Ant) { l1Ant = l1; r1Ant = r1; iniciarGiroExacto(-1); return; }
    l1Ant = l1; r1Ant = r1;

    // FIX: En modo DUAL el joystick derecho (axisRX) maneja el giro
    int throttle = -(gp->axisY());
    int steer    = modoDualJoystick ? -(gp->axisRX()) : -(gp->axisX());

    if (controlesInvertidos) { throttle = -throttle; steer = -steer; }

    throttle = (int)(throttle * multiplicadorVelocidad);
    steer    = (int)(steer    * multiplicadorVelocidad);

    chequearConfigMando(gp);
    procesarMovimiento(throttle, steer);

    // Efecto GREEN bloquea el motorA (escudo frontal)
    if (efectoActual == "GREEN") {
        analogWrite(PIN_MOTOR_A1, 0);
        analogWrite(PIN_MOTOR_A2, 0);
    }

    // Vaporizador con botón A
    bool vapOn = gp->a();
    digitalWrite(PIN_VAPORIZADOR, vapOn ? HIGH : LOW);
    if (vapOn) Serial.println("[VAPOR] Activado");

    // Lanzar poder con botón B
    if (gp->b()) throwPow();
}


// ════════════════════════════════════════════════════════════════
//  CALLBACKS BLUEPAD32
// ════════════════════════════════════════════════════════════════

void onConnectedGamepad(GamepadPtr gp) {
    Serial.println("[BT] Conexión recibida");
    if (mandoPrincipal != nullptr) {
        Serial.println("[BT] Rechazado — ya hay un mando activo");
        gp->disconnect();
        return;
    }
    mandoPrincipal   = gp;
    modoDualJoystick = true;
    Serial.printf("[BT] Conectado: %s\n", gp->getModelName());
}

void onDisconnectedGamepad(GamepadPtr gp) {
    Serial.println("[BT] Mando desconectado");
    if (mandoPrincipal == gp) {
        mandoPrincipal = nullptr;
        detener();
        setParqueo(true);
        Serial.println("[BT] Carro detenido");
    }
}


// ════════════════════════════════════════════════════════════════
//  INICIALIZACIÓN DE SUBSISTEMAS
// ════════════════════════════════════════════════════════════════

void setupMotores() {
    pinMode(PIN_MOTOR_A1, OUTPUT); digitalWrite(PIN_MOTOR_A1, LOW);
    pinMode(PIN_MOTOR_A2, OUTPUT); digitalWrite(PIN_MOTOR_A2, LOW);
    pinMode(PIN_MOTOR_B1, OUTPUT); digitalWrite(PIN_MOTOR_B1, LOW);
    pinMode(PIN_MOTOR_B2, OUTPUT); digitalWrite(PIN_MOTOR_B2, LOW);
}

void setupRGBLed() {
    FastLED.addLeds<WS2812B, PIN_RGB, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(60);
    Serial.println("[RGB] FastLED listo");
}

void setupAudio() {
    Serial.print(F("[AUDIO] Iniciando DFPlayer... "));
    dfSerial.begin(9600, SERIAL_8N1, 16, 17);
    if (!myDFPlayer.begin(dfSerial, false, true)) {
        Serial.println(F("Error — verificar conexión o SD"));
        while (true) delay(0);
    }
    delay(1000);
    Serial.println(F("[AUDIO] DFPlayer Mini online"));
    myDFPlayer.volume(sound_volume);
    delay(100);
}

void setupEspNow() {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[ESPNOW] Canal: %d\n", WiFi.channel());
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] Error al inicializar");
        return;
    }
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
    WiFi.macAddress(myMac);
    Serial.printf("[ESPNOW] Listo — MAC: %s\n", WiFi.macAddress().c_str());
}


// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════

void setup() {
    setupMotores();
    setupRGBLed();

    // Pausa de estabilización tras el chispazo del interruptor
    delay(2500);
    Serial.begin(115200);
    Serial.println("\n=== BOOT ===");

    randomSeed(analogRead(34));

    pinMode(PIN_VAPORIZADOR,  OUTPUT);
    pinMode(PIN_EYECCION,     INPUT_PULLUP);
    pinMode(PIN_LED_TRASERO,  OUTPUT);
    Serial.println("[PINES] OK");

    blink(1);
    BP32.setup(&onConnectedGamepad, &onDisconnectedGamepad);
    Serial.println("[BP32] Esperando mando...");

    blink(2);
    setupEspNow();

    blink(3);
    setupAudio();

    digitalWrite(PIN_LED_TRASERO, HIGH);
    enParqueo = true;
}


// ════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════

void checkEyeccion() {
    static unsigned long lastCheck   = 0;
    static unsigned long lastTrigger = 0;
    unsigned long now = millis();
    if (now - lastCheck < 2) return;
    lastCheck = now;
    if (digitalRead(PIN_EYECCION) == LOW && (now - lastTrigger > 500)) {
        eyeccionDetectada = true;
        lastTrigger       = now;
        Serial.println("[EYECCION] Pulso detectado");
    }
}

void loop() {
    BP32.update();
    checkEyeccion();
    checkPistasPendientes();
    checkAudio();
    updateLEDs();
    processQueue();

    // ── Eyección ─────────────────────────────────────────────────
    if (eyeccionDetectada) {
        eyeccionDetectada = false;
        setParqueo(true);
        Serial.println("[EYECCION] Piloto expulsado");
        if (mandoPrincipal && mandoPrincipal->isConnected()) {
            mandoPrincipal->setRumble(0xFF, 0xFF);
            rumbleActive = true;
            rumbleEnd    = millis() + 2000;
        }
        reproducirEyeccion();
    }

    // ── Rumble timeout ───────────────────────────────────────────
    if (rumbleActive && millis() > rumbleEnd) {
        if (mandoPrincipal) mandoPrincipal->setRumble(0, 0);
        rumbleActive = false;
        Serial.println("[MANDO] Rumble apagado");
    }

    // ── Giro exacto ──────────────────────────────────────────────
    if (giroExactoActivo) ejecutarGiroExacto();

    // ── Gamepad cada 20 ms ───────────────────────────────────────
    static unsigned long lastProcess = 0;
    if (millis() - lastProcess >= 20) {
        lastProcess = millis();
        if (mandoPrincipal && mandoPrincipal->isConnected() && !giroExactoActivo)
            processGamepad(mandoPrincipal);
    }

    // ── Debug cada 2 s ───────────────────────────────────────────
    if (millis() - lastDebug > 2000) {
        lastDebug = millis();
        Serial.printf("[LOOP] Mando:%s:%s Parq:%s Efecto:%s Poder:%s\n",
            mandoPrincipal ? "SI"     : "NO",
            modoDualJoystick ? "DUAL" : "SINGLE",
            enParqueo        ? "SI"   : "NO",
            efectoActual.c_str(),
            grantedPow.c_str());
    }
}
