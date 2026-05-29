#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define APPLY_CMD "APPLY:"
#define GRAY      0x7BEF

// Configuración de reintentos de fondo
#define MAX_INTENTOS        5
#define REENTRADA_INTERVALO 1500

#define MULTIPLICADOR 0.45

// ── Tiempos disponibles (ms) ──────────────────────────────────────────────────
const int TIEMPOS[]     = {2000, 5000, 10000, 15000, 20000, 30000};
const int NUM_TIEMPOS   = sizeof(TIEMPOS) / sizeof(TIEMPOS[0]);
int tiempoIndex         = 2;   // Default: 10 000 ms

// ── Efectos / colores ─────────────────────────────────────────────────────────
const int NUM_EFFECTS   = 4;
const char* efectos[NUM_EFFECTS] = {"RED", "YELLOW", "BLUE", "GREEN"};

// Colores de pantalla para cada botón
const uint32_t COLORES_BTN[NUM_EFFECTS] = {
    RED,      // RED
    YELLOW,   // YELLOW
    BLUE,     // BLUE
    GREEN     // GREEN
};

int currentIndex = 0;

// ── Peers ─────────────────────────────────────────────────────────────────────
#define MAX_PEERS 6
struct PeerEntry {
    uint8_t mac[6];
    int     fallos;
    char    nombre[16];
};

PeerEntry peers[MAX_PEERS] = {
    {{0x1C, 0xC3, 0xAB, 0xC3, 0xD0, 0xE8}, 0,  "CARRO_1"},
    {{0x24, 0x6F, 0x28, 0x96, 0x03, 0x08}, 0,  "CARRO_2"},
    {{0x1C, 0xC3, 0xAB, 0xF9, 0x2C, 0xE0}, 0,  "CARRO_3"},
    {{0x1C, 0xC3, 0xAB, 0xFA, 0x0C, 0x5C}, 0,  "CARRO NARANJA"},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 99, ""},
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 99, ""},
};

// ── Cola de mensajes pendientes ───────────────────────────────────────────────
struct PendingMsg {
    uint8_t  mac[6];
    char     msg[48];
    int      intentos;
    bool     confirmado;
    uint32_t ultimoIntentoTiempo;
};

#define MAX_PENDING 8
PendingMsg pendingQueue[MAX_PENDING];
int        pendingCount = 0;

// ── Estado de feedback visual ─────────────────────────────────────────────────
bool     feedbackActivo    = false;
uint32_t feedbackInicio    = 0;
#define  FEEDBACK_DURACION 400   // ms que se muestra el flash de envío

// ─────────────────────────────────────────────────────────────────────────────
//  drawUI  — layout horizontal de botones + selector de tiempo
// ─────────────────────────────────────────────────────────────────────────────
void drawUI() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);

    // ── Título ────────────────────────────────────────────────────────────────
    d.setTextColor(WHITE);
    d.setTextSize(1);
    d.setCursor(4, 4);
    d.print("< > SELECCIONAR  ENTER ENVIAR");

    // ── Botones de color horizontales ─────────────────────────────────────────
    // Pantalla Cardputer en landscape: 240 x 135 px
    const int BTN_W   = 52;
    const int BTN_H   = 52;
    const int BTN_Y   = 22;
    const int PADDING = 4;
    // Calcular X de inicio para centrar los 4 botones
    int totalW = NUM_EFFECTS * BTN_W + (NUM_EFFECTS - 1) * PADDING;
    int startX = (240 - totalW) / 2;

    for (int i = 0; i < NUM_EFFECTS; i++) {
        int x = startX + i * (BTN_W + PADDING);

        if (i == currentIndex) {
            // Borde blanco grueso para el seleccionado
            d.fillRoundRect(x - 3, BTN_Y - 3, BTN_W + 6, BTN_H + 6, 6, WHITE);
        }

        // Relleno del botón
        d.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 4, COLORES_BTN[i]);

        // Etiqueta del color (texto negro sobre colores claros, blanco en azul)
        uint16_t textCol = (i == 2) ? WHITE : BLACK;  // Azul → texto blanco
        d.setTextColor(textCol);
        d.setTextSize(1);

        // Centrar texto horizontalmente
        int tw = strlen(efectos[i]) * 6;
        d.setCursor(x + (BTN_W - tw) / 2, BTN_Y + (BTN_H / 2) - 4);
        d.print(efectos[i]);
    }

    // ── Selector de tiempo (fila inferior) ───────────────────────────────────
    // Instrucción
    d.setTextColor(GRAY);
    d.setTextSize(1);
    d.setCursor(4, 84);
    d.print("^ TIEMPO ^");

    // Valor actual centrado con flechas
    char buf[24];
    int  seg = TIEMPOS[tiempoIndex] / 1000;
    snprintf(buf, sizeof(buf), "< %2ds >", seg);

    d.setTextColor(WHITE);
    d.setTextSize(2);
    int tw2 = strlen(buf) * 12;
    d.setCursor((240 - tw2) / 2, 100);
    d.print(buf);

    // Barra de progreso discreta para el tiempo
    const int BAR_W = 200;
    const int BAR_H = 4;
    int barX = (240 - BAR_W) / 2;
    int barY = 128;
    d.fillRect(barX, barY, BAR_W, BAR_H, GRAY);
    int filled = (BAR_W * tiempoIndex) / (NUM_TIEMPOS - 1);
    d.fillRect(barX, barY, filled, BAR_H, WHITE);
}

// ── Flash de confirmación de envío ────────────────────────────────────────────
void mostrarFeedback() {
    auto& d = M5Cardputer.Display;
    // Fondo del color seleccionado por 400 ms
    d.fillScreen(COLORES_BTN[currentIndex]);
    d.setTextColor((currentIndex == 2) ? WHITE : BLACK);
    d.setTextSize(2);
    const char* lbl = "ENVIANDO...";
    int tw = strlen(lbl) * 12;
    d.setCursor((240 - tw) / 2, 55);
    d.print(lbl);

    feedbackActivo = true;
    feedbackInicio = millis();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Cola
// ─────────────────────────────────────────────────────────────────────────────
void dequeue(const uint8_t* mac_addr) {
    for (int i = 0; i < pendingCount; i++) {
        if (memcmp(pendingQueue[i].mac, mac_addr, 6) == 0) {
            Serial.printf("[COLA] Removido OK para %02X:%02X:%02X\n",
                          mac_addr[3], mac_addr[4], mac_addr[5]);
            for (int j = i; j < pendingCount - 1; j++) pendingQueue[j] = pendingQueue[j + 1];
            pendingCount--;
            break;
        }
    }
}

void OnDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        for (int i = 0; i < MAX_PEERS; i++) {
            if (memcmp(peers[i].mac, mac_addr, 6) == 0) {
                peers[i].fallos = 0;
                Serial.printf("[ESPNOW] Éxito — fallos reseteados: %s\n", peers[i].nombre);
                break;
            }
        }
        dequeue(mac_addr);
    } else {
        Serial.printf("[ESPNOW] Fallo ACK desde %02X:%02X:%02X\n",
                      mac_addr[3], mac_addr[4], mac_addr[5]);
    }
}

void queueEffectCommand(const char* efecto) {
    int duracion = TIEMPOS[tiempoIndex];
    char payload[48];
    snprintf(payload, sizeof(payload), "%s%s:%d:%.2f", APPLY_CMD, efecto, duracion, MULTIPLICADOR);

    Serial.printf("[UI] Comando: %s\n", payload);

    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].fallos == 99 || (peers[i].mac[0] == 0 && peers[i].mac[5] == 0)) continue;
        if (pendingCount >= MAX_PENDING) {
            Serial.println("[COLA] Cola llena.");
            break;
        }
        memcpy(pendingQueue[pendingCount].mac, peers[i].mac, 6);
        strncpy(pendingQueue[pendingCount].msg, payload, sizeof(pendingQueue[pendingCount].msg));
        pendingQueue[pendingCount].intentos           = 0;
        pendingQueue[pendingCount].confirmado         = false;
        pendingQueue[pendingCount].ultimoIntentoTiempo = 0;
        Serial.printf("[COLA] Agendado → %s\n", peers[i].nombre);
        pendingCount++;
    }
}

void procesarColaMensajes() {
    uint32_t ahora = millis();
    int i = 0;
    while (i < pendingCount) {
        if (pendingQueue[i].intentos >= MAX_INTENTOS) {
            for (int p = 0; p < MAX_PEERS; p++) {
                if (memcmp(peers[p].mac, pendingQueue[i].mac, 6) == 0) {
                    peers[p].fallos++;
                    Serial.printf("[COLA] Descartado (reintentos agotados): %s. Fallos: %d\n",
                                  peers[p].nombre, peers[p].fallos);
                    break;
                }
            }
            for (int j = i; j < pendingCount - 1; j++) pendingQueue[j] = pendingQueue[j + 1];
            pendingCount--;
            continue;
        }
        if (ahora - pendingQueue[i].ultimoIntentoTiempo >= REENTRADA_INTERVALO) {
            pendingQueue[i].intentos++;
            pendingQueue[i].ultimoIntentoTiempo = ahora;
            Serial.printf("[ENVÍO] → %02X:%02X:%02X  intento %d/%d  msg: %s\n",
                          pendingQueue[i].mac[3], pendingQueue[i].mac[4], pendingQueue[i].mac[5],
                          pendingQueue[i].intentos, MAX_INTENTOS, pendingQueue[i].msg);
            esp_err_t r = esp_now_send(pendingQueue[i].mac,
                                       (uint8_t*)pendingQueue[i].msg,
                                       strlen(pendingQueue[i].msg) + 1);
            if (r != ESP_OK)
                Serial.printf("[ESPNOW] Error stack inmediato (código %d)\n", r);
        }
        i++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup / loop
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    Serial.begin(115200);
    Serial.println("[SISTEMA] Iniciando...");

    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[SISTEMA] Error crítico: ESP-NOW no inicializado.");
        M5Cardputer.Display.fillScreen(RED);
        while (1) delay(100);
    }

    esp_now_register_send_cb((esp_now_send_cb_t)OnDataSent);

    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].fallos == 99) continue;
        esp_now_peer_info_t pi;
        memset(&pi, 0, sizeof(pi));
        memcpy(pi.peer_addr, peers[i].mac, 6);
        pi.channel = 1;
        pi.encrypt = false;
        if (esp_now_add_peer(&pi) == ESP_OK)
            Serial.printf("[SISTEMA] Peer registrado: %s\n", peers[i].nombre);
        else
            Serial.printf("[SISTEMA] Fallo al registrar: %s\n", peers[i].nombre);
    }

    drawUI();
    Serial.println("[SISTEMA] Listo.");
}

void loop() {
    M5Cardputer.update();

    // ── Gestión del feedback visual ───────────────────────────────────────────
    if (feedbackActivo && (millis() - feedbackInicio >= FEEDBACK_DURACION)) {
        feedbackActivo = false;
        drawUI();   // Volver a la UI normal
    }

    // ── Teclado (solo si no hay feedback activo) ──────────────────────────────
    if (!feedbackActivo && M5Cardputer.Keyboard.isPressed()) {

        // Izquierda (',') → botón anterior
        if (M5Cardputer.Keyboard.isKeyPressed(',')) {
            currentIndex = (currentIndex == 0) ? NUM_EFFECTS - 1 : currentIndex - 1;
            drawUI();
            while (M5Cardputer.Keyboard.isKeyPressed(',')) M5Cardputer.update();
        }
        // Derecha ('/') → botón siguiente  (en Cardputer '/' es flecha derecha)
        else if (M5Cardputer.Keyboard.isKeyPressed('/')) {
            currentIndex = (currentIndex == NUM_EFFECTS - 1) ? 0 : currentIndex + 1;
            drawUI();
            while (M5Cardputer.Keyboard.isKeyPressed('/')) M5Cardputer.update();
        }
        // Arriba (';') → más tiempo
        else if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            if (tiempoIndex < NUM_TIEMPOS - 1) tiempoIndex++;
            drawUI();
            while (M5Cardputer.Keyboard.isKeyPressed(';')) M5Cardputer.update();
        }
        // Abajo ('.') → menos tiempo
        else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            if (tiempoIndex > 0) tiempoIndex--;
            drawUI();
            while (M5Cardputer.Keyboard.isKeyPressed('.')) M5Cardputer.update();
        }
        // Enter → enviar
        else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
            mostrarFeedback();
            queueEffectCommand(efectos[currentIndex]);
            while (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) M5Cardputer.update();
        }
    }

    procesarColaMensajes();
    delay(30);
}
