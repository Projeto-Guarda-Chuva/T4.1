/**
 * ============================================================
 *  CENTRAL DE OPERACAO  —  Servidor REST (AsyncWebServer)
 *  Plataforma: ESP32 (Wokwi)
 *
 *   1. Recebe estado da escultura                       (US06)
 *   2. Controla jornadas + eventos                      (US07)
 *   3. Detecta anomalias (velocidade/posicao/programa/status/delta)
 *   4. Parada segura (emergencia bloqueia POSTs)
 *   5. Notifica registrador externo (POST HTTP)
 *   6. 5 estados padrao + 1 aleatorio
 *   7. Logs em buffer txt temporario (acessivel via GET /logs)
 *
 *  WiFi: Wokwi-GUEST    HTTP: porta 80
 *  Acesso externo:      http://localhost:8180
 * ============================================================
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <HTTPClient.h>
#include <stdarg.h>

// ─── Constantes ──────────────────────────────────────────────
#define SERIAL_BAUD          115200
#define HTTP_PORT            80
#define MAX_JORNADAS         10
#define MAX_EVENTOS_JORNADA  30
#define ESTADO_TIMEOUT_MS    30000
#define HTTP_NOTIF_TIMEOUT   5000
#define LOG_BUFFER_SIZE      4096

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ─── Limites de anomalia ────────────────────────────────────
struct Limites {
    int   velocidade_max;
    float pos_min[3];
    float pos_max[3];
    float delta_pos_max;
    int   programas_validos[6];
    int   num_programas;
};
static Limites g_lim = {
    /* velocidade_max */ 200,
    /* pos_min        */ {-1000.0f, -1000.0f, -1000.0f},
    /* pos_max        */ { 1000.0f,  1000.0f,  1000.0f},
    /* delta_pos_max  */ 200.0f,
    /* programas      */ {1, 2, 3, 4, 5, 0},
    /* num_programas  */ 5
};

// ─── Estruturas ──────────────────────────────────────────────
struct EstadoEscultura {
    int           programa;
    float         posX, posY, posZ;
    int           velocidade;
    char          status[24];
    unsigned long timestamp;
    unsigned long recebidoEm;
    bool          valido;
};

struct EventoJornada {
    unsigned long ts;
    float         posX, posY, posZ;
    int           velocidade;
    char          status[16];
};

struct Jornada {
    char          id[32];
    unsigned long inicio;
    unsigned long fim;
    bool          encerrada;
    EventoJornada eventos[MAX_EVENTOS_JORNADA];
    int           num_eventos;
};

// ─── Presets de estado ───────────────────────────────────────
struct Preset {
    const char* nome;
    int         programa;
    float       posX, posY, posZ;
    int         velocidade;
    const char* status;
};
static const Preset g_presets[5] = {
    { "inicial",         1,    0.0f,    0.0f,    0.0f,    0, "parado"     },
    { "operacional",     2,   50.0f,   30.0f,   10.0f,   60, "executando" },
    { "alta_velocidade", 3,  100.0f,    0.0f,   50.0f,  180, "executando" },
    { "manutencao",      4,   25.0f,   25.0f,   25.0f,    0, "manutencao" },
    { "demonstracao",    5,  -50.0f,   50.0f,   20.0f,  100, "executando" }
};
static const int NUM_PRESETS = sizeof(g_presets) / sizeof(g_presets[0]);

// ─── Estado global ───────────────────────────────────────────
static EstadoEscultura g_estado          = {};
static EstadoEscultura g_estado_anterior = {};
static Jornada         g_jornadas[MAX_JORNADAS] = {};
static int             g_num_jornadas    = 0;
static int             g_jornada_idx     = -1;
static unsigned long   g_ultimo_update   = 0;

static bool   g_emergencia       = false;
static char   g_emerg_motivo[96] = "";
static unsigned long g_emerg_ts  = 0;

static char   g_registrador_url[128] = "";

// ─── Log buffer (txt temporario) ────────────────────────────
static char   g_log_buf[LOG_BUFFER_SIZE + 1] = {0};
static size_t g_log_len = 0;

// ─── Fila de notificacoes (processada em loop) ──────────────
struct Notificacao {
    char tipo[32];
    char body[640];
    bool pendente;
};
static Notificacao g_notif = {};

AsyncWebServer server(HTTP_PORT);

// ─── Prototipos ──────────────────────────────────────────────
void wifi_init();
void registrar_rotas();
void log_append(const char* fmt, ...);
void verificar_timeout();
void enfileirar_notificacao(const char* tipo, const JsonDocument& payload);
void processar_notificacao_pendente();
const char* detectar_anomalia(const EstadoEscultura& novo, const EstadoEscultura& ant);
void disparar_emergencia(const char* motivo);
void registrar_evento_jornada(const EstadoEscultura& e);
bool aplicar_novo_estado(const EstadoEscultura& novo, const char** out_motivo);
void preencher_aleatorio(EstadoEscultura& e);

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    randomSeed((uint32_t) esp_random());

    Serial.println(F(""));
    Serial.println(F("=== CENTRAL DE OPERACAO (REST async) ==="));
    log_append("Sistema iniciando");

    wifi_init();
    registrar_rotas();
    server.begin();

    Serial.print(F("[HTTP] Servidor REST ativo em http://"));
    Serial.print(WiFi.localIP());
    Serial.print(F(":"));
    Serial.println(HTTP_PORT);
    Serial.println(F("[HTTP] Acesso externo via http://localhost:8180"));
    Serial.println(F("========================================"));
    log_append("Servidor REST pronto em %s:%d", WiFi.localIP().toString().c_str(), HTTP_PORT);

    g_estado.valido = false;
    g_ultimo_update = millis();
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
    verificar_timeout();
    processar_notificacao_pendente();
    delay(10);
}

// ════════════════════════════════════════════════════════════
//  WIFI
// ════════════════════════════════════════════════════════════
void wifi_init() {
    Serial.print(F("[WiFi] Conectando a "));
    Serial.print(WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    Serial.print(F("[WiFi] OK | IP: "));
    Serial.println(WiFi.localIP());
    log_append("WiFi conectado: %s", WiFi.localIP().toString().c_str());
}

// ════════════════════════════════════════════════════════════
//  LOG BUFFER (txt temporario em RAM)
// ════════════════════════════════════════════════════════════
void log_append(const char* fmt, ...) {
    char line[200];
    int prefix = snprintf(line, sizeof(line), "[%lus] ", millis() / 1000);
    if (prefix < 0 || prefix >= (int)sizeof(line)) return;

    va_list args;
    va_start(args, fmt);
    int rest = vsnprintf(line + prefix, sizeof(line) - prefix - 3, fmt, args);
    va_end(args);
    if (rest < 0) return;

    int total = prefix + rest;
    if (total > (int)sizeof(line) - 3) total = sizeof(line) - 3;
    line[total++] = '\r';
    line[total++] = '\n';
    line[total]   = '\0';

    if (g_log_len + total >= LOG_BUFFER_SIZE) {
        size_t drop = g_log_len / 2;
        char* nl = (char*) memchr(g_log_buf + drop, '\n', g_log_len - drop);
        if (nl) drop = (nl - g_log_buf) + 1;
        memmove(g_log_buf, g_log_buf + drop, g_log_len - drop);
        g_log_len -= drop;
    }
    memcpy(g_log_buf + g_log_len, line, total);
    g_log_len += total;
    g_log_buf[g_log_len] = '\0';
}

// ════════════════════════════════════════════════════════════
//  HELPERS HTTP
// ════════════════════════════════════════════════════════════
void enviar_json(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", out);
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

void enviar_erro(AsyncWebServerRequest* req, int code, const char* msg) {
    JsonDocument doc;
    doc["erro"] = msg;
    enviar_json(req, code, doc);
}

bool bloqueado_por_emergencia(AsyncWebServerRequest* req) {
    if (!g_emergencia) return false;
    JsonDocument doc;
    doc["erro"]   = "sistema em parada de emergencia";
    doc["motivo"] = g_emerg_motivo;
    doc["desde"]  = g_emerg_ts;
    enviar_json(req, 503, doc);
    return true;
}

// ════════════════════════════════════════════════════════════
//  DETECCAO DE ANOMALIA
// ════════════════════════════════════════════════════════════
const char* detectar_anomalia(const EstadoEscultura& n, const EstadoEscultura& ant) {
    static char buf[80];

    if (n.velocidade > g_lim.velocidade_max || n.velocidade < 0) {
        snprintf(buf, sizeof(buf), "velocidade fora dos limites: %d", n.velocidade);
        return buf;
    }
    const float pos[3] = { n.posX, n.posY, n.posZ };
    const char* eixos  = "xyz";
    for (int i = 0; i < 3; i++) {
        if (pos[i] < g_lim.pos_min[i] || pos[i] > g_lim.pos_max[i]) {
            snprintf(buf, sizeof(buf), "posicao %c=%.1f fora da zona segura",
                     eixos[i], pos[i]);
            return buf;
        }
    }
    bool prog_ok = false;
    for (int i = 0; i < g_lim.num_programas; i++) {
        if (g_lim.programas_validos[i] == n.programa) { prog_ok = true; break; }
    }
    if (!prog_ok) {
        snprintf(buf, sizeof(buf), "programa nao autorizado: %d", n.programa);
        return buf;
    }
    if (strcmp(n.status, "erro")  == 0 || strcmp(n.status, "falha")  == 0 ||
        strcmp(n.status, "panico") == 0) {
        snprintf(buf, sizeof(buf), "status reportado de falha: %s", n.status);
        return buf;
    }
    if (ant.valido) {
        float dx = n.posX - ant.posX;
        float dy = n.posY - ant.posY;
        float dz = n.posZ - ant.posZ;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > g_lim.delta_pos_max) {
            snprintf(buf, sizeof(buf), "salto brusco de posicao: %.1f", dist);
            return buf;
        }
    }
    return nullptr;
}

void disparar_emergencia(const char* motivo) {
    g_emergencia = true;
    strlcpy(g_emerg_motivo, motivo, sizeof(g_emerg_motivo));
    g_emerg_ts = millis();

    Serial.print(F("[!!!] PARADA DE EMERGENCIA: "));
    Serial.println(motivo);
    log_append("[EMERGENCIA] %s", motivo);

    JsonDocument payload;
    payload["motivo"]       = motivo;
    payload["timestamp"]    = g_emerg_ts;
    payload["programa"]     = g_estado.programa;
    payload["velocidade"]   = g_estado.velocidade;
    payload["status"]       = g_estado.status;
    payload["posicao"]["x"] = g_estado.posX;
    payload["posicao"]["y"] = g_estado.posY;
    payload["posicao"]["z"] = g_estado.posZ;
    enfileirar_notificacao("PARADA_EMERGENCIA", payload);

    if (g_jornada_idx >= 0 && !g_jornadas[g_jornada_idx].encerrada) {
        g_jornadas[g_jornada_idx].fim = millis();
        g_jornadas[g_jornada_idx].encerrada = true;
        Serial.printf("[US07] Jornada '%s' fechada por emergencia.\r\n",
                      g_jornadas[g_jornada_idx].id);
        log_append("[US07] jornada '%s' fechada por emergencia",
                   g_jornadas[g_jornada_idx].id);
    }
}

void registrar_evento_jornada(const EstadoEscultura& e) {
    if (g_jornada_idx < 0) return;
    Jornada& j = g_jornadas[g_jornada_idx];
    if (j.encerrada) return;

    if (j.num_eventos >= MAX_EVENTOS_JORNADA) {
        memmove(&j.eventos[0], &j.eventos[1],
                (MAX_EVENTOS_JORNADA - 1) * sizeof(EventoJornada));
        j.num_eventos = MAX_EVENTOS_JORNADA - 1;
    }
    EventoJornada& ev = j.eventos[j.num_eventos++];
    ev.ts         = e.timestamp;
    ev.posX       = e.posX;
    ev.posY       = e.posY;
    ev.posZ       = e.posZ;
    ev.velocidade = e.velocidade;
    strlcpy(ev.status, e.status, sizeof(ev.status));
}

// ────── Aplica novo estado: anomalia + atualiza + evento de jornada ────
bool aplicar_novo_estado(const EstadoEscultura& novo, const char** out_motivo) {
    const char* anomalia = detectar_anomalia(novo, g_estado_anterior);
    if (anomalia) {
        g_estado_anterior = g_estado;
        g_estado          = novo;
        g_ultimo_update   = millis();
        disparar_emergencia(anomalia);
        if (out_motivo) *out_motivo = anomalia;
        return false;
    }
    g_estado_anterior = g_estado;
    g_estado          = novo;
    g_ultimo_update   = millis();

    Serial.printf("[US06] prog=%d | pos=(%.1f,%.1f,%.1f) | vel=%d | status=%s\r\n",
                  novo.programa, novo.posX, novo.posY, novo.posZ,
                  novo.velocidade, novo.status);
    log_append("[US06] prog=%d pos=(%.1f,%.1f,%.1f) vel=%d status=%s",
               novo.programa, novo.posX, novo.posY, novo.posZ,
               novo.velocidade, novo.status);

    if (g_jornada_idx >= 0 && !g_jornadas[g_jornada_idx].encerrada) {
        registrar_evento_jornada(g_estado);
    }
    if (out_motivo) *out_motivo = nullptr;
    return true;
}

// ────── Estado aleatorio (respeita delta para nao disparar emergencia) ──
void preencher_aleatorio(EstadoEscultura& e) {
    e.programa = g_lim.programas_validos[random(g_lim.num_programas)];

    // posicao = atual + deslocamento pequeno
    float baseX = g_estado.valido ? g_estado.posX : 0.0f;
    float baseY = g_estado.valido ? g_estado.posY : 0.0f;
    float baseZ = g_estado.valido ? g_estado.posZ : 0.0f;
    float maxDelta = g_lim.delta_pos_max * 0.5f;
    e.posX = baseX + (random(-1000, 1001) / 1000.0f) * maxDelta;
    e.posY = baseY + (random(-1000, 1001) / 1000.0f) * maxDelta;
    e.posZ = baseZ + (random(-1000, 1001) / 1000.0f) * maxDelta;

    // clip pra dentro dos limites
    for (int i = 0; i < 3; i++) {
        float& p = (i == 0 ? e.posX : (i == 1 ? e.posY : e.posZ));
        if (p < g_lim.pos_min[i]) p = g_lim.pos_min[i];
        if (p > g_lim.pos_max[i]) p = g_lim.pos_max[i];
    }

    e.velocidade = random(0, g_lim.velocidade_max + 1);

    const char* statuses[] = { "executando", "parado", "manutencao", "ocioso" };
    strlcpy(e.status, statuses[random(4)], sizeof(e.status));

    e.timestamp  = millis();
    e.recebidoEm = millis();
    e.valido     = true;
}

// ════════════════════════════════════════════════════════════
//  NOTIFICACOES 
// ════════════════════════════════════════════════════════════
void enfileirar_notificacao(const char* tipo, const JsonDocument& payload) {
    JsonDocument env;
    env["origem"]    = "central_operacao";
    env["tipo"]      = tipo;
    env["timestamp"] = millis();
    env["dados"]     = payload;

    String body;
    serializeJson(env, body);
    strlcpy(g_notif.tipo, tipo,         sizeof(g_notif.tipo));
    strlcpy(g_notif.body, body.c_str(), sizeof(g_notif.body));
    g_notif.pendente = true;
}

void processar_notificacao_pendente() {
    if (!g_notif.pendente) return;
    g_notif.pendente = false;

    if (g_registrador_url[0] == '\0') {
        Serial.printf("[REGISTRADOR] (URL nao configurada) tipo=%s\r\n", g_notif.tipo);
        log_append("[REGISTRADOR] %s (sem URL configurada)", g_notif.tipo);
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[REGISTRADOR] WiFi caiu, abortando"));
        return;
    }
    HTTPClient http;
    http.setTimeout(HTTP_NOTIF_TIMEOUT);
    if (!http.begin(g_registrador_url)) return;
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(g_notif.body);
    if (code > 0) {
        Serial.printf("[REGISTRADOR] %s -> %d\r\n", g_notif.tipo, code);
        log_append("[REGISTRADOR] %s -> HTTP %d", g_notif.tipo, code);
    } else {
        Serial.printf("[REGISTRADOR] erro %s -> %s\r\n",
                      g_notif.tipo, http.errorToString(code).c_str());
        log_append("[REGISTRADOR] erro %s", g_notif.tipo);
    }
    http.end();
}

void verificar_timeout() {
    if (!g_estado.valido) return;
    if (millis() - g_ultimo_update <= ESTADO_TIMEOUT_MS) return;
    if (strcmp(g_estado.status, "sem_sinal") == 0) return;
    strlcpy(g_estado.status, "sem_sinal", sizeof(g_estado.status));
    g_estado.valido = false;
    Serial.println(F("[US06] AVISO: timeout — status=sem_sinal"));
    log_append("[US06] timeout: status=sem_sinal");
}

// ════════════════════════════════════════════════════════════
//  ROTAS
// ════════════════════════════════════════════════════════════
void registrar_rotas() {

    // ─── GET / ─────────────────────────────────────────────
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] GET /"));
        JsonDocument doc;
        doc["servico"]    = "central_operacao";
        doc["versao"]     = "3.0";
        doc["uptime_s"]   = millis() / 1000;
        doc["emergencia"] = g_emergencia;
        if (g_emergencia) doc["motivo_emergencia"] = g_emerg_motivo;
        JsonArray rotas = doc["rotas"].to<JsonArray>();
        rotas.add("GET    /estado");
        rotas.add("POST   /estado");
        rotas.add("GET    /estado/presets");
        rotas.add("POST   /estado/preset?id=N        (1..5)");
        rotas.add("POST   /estado/aleatorio");
        rotas.add("GET    /jornadas");
        rotas.add("GET    /jornadas/detalhe?idx=N");
        rotas.add("POST   /jornadas/inicio");
        rotas.add("POST   /jornadas/fim");
        rotas.add("POST   /emergencia");
        rotas.add("POST   /emergencia/reset");
        rotas.add("GET    /config");
        rotas.add("POST   /config/registrador");
        rotas.add("GET    /logs            (txt temporario)");
        rotas.add("DELETE /logs            (limpa buffer)");
        enviar_json(req, 200, doc);
    });

    // ─── GET /estado ───────────────────────────────────────
    server.on("/estado", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] GET /estado"));
        JsonDocument doc;
        doc["valido"]       = g_estado.valido;
        doc["programa"]     = g_estado.programa;
        doc["posicao"]["x"] = g_estado.posX;
        doc["posicao"]["y"] = g_estado.posY;
        doc["posicao"]["z"] = g_estado.posZ;
        doc["velocidade"]   = g_estado.velocidade;
        doc["status"]       = g_estado.status;
        doc["timestamp"]    = g_estado.timestamp;
        doc["recebido_ms"]  = g_estado.recebidoEm;
        doc["emergencia"]   = g_emergencia;
        enviar_json(req, 200, doc);
    });

    // ─── POST /estado ──────────────────────────────────────
    auto* h_estado = new AsyncCallbackJsonWebHandler("/estado",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            Serial.println(F("[HTTP] POST /estado"));
            if (bloqueado_por_emergencia(req)) return;

            JsonObject doc = json.as<JsonObject>();
            EstadoEscultura novo = {};
            novo.programa   = doc["programa"]     | 0;
            novo.posX       = doc["posicao"]["x"] | 0.0f;
            novo.posY       = doc["posicao"]["y"] | 0.0f;
            novo.posZ       = doc["posicao"]["z"] | 0.0f;
            novo.velocidade = doc["velocidade"]   | 0;
            strlcpy(novo.status, doc["status"] | "desconhecido", sizeof(novo.status));
            novo.timestamp  = doc["timestamp"]    | (unsigned long)millis();
            novo.recebidoEm = millis();
            novo.valido     = true;

            const char* motivo = nullptr;
            bool ok = aplicar_novo_estado(novo, &motivo);
            if (!ok) {
                JsonDocument resp;
                resp["aceito"]     = false;
                resp["emergencia"] = true;
                resp["motivo"]     = motivo;
                enviar_json(req, 503, resp);
                return;
            }
            JsonDocument resp;
            resp["origem"]     = "central_op";
            resp["tipo"]       = "estado_pub";
            resp["aceito"]     = true;
            resp["programa"]   = g_estado.programa;
            resp["status"]     = g_estado.status;
            resp["velocidade"] = g_estado.velocidade;
            enviar_json(req, 200, resp);
        });
    h_estado->setMethod(HTTP_POST);
    server.addHandler(h_estado);

    // ─── GET /estado/presets ───────────────────────────────
    server.on("/estado/presets", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] GET /estado/presets"));
        JsonDocument doc;
        JsonArray arr = doc["presets"].to<JsonArray>();
        for (int i = 0; i < NUM_PRESETS; i++) {
            JsonObject p = arr.add<JsonObject>();
            p["id"]         = i + 1;
            p["nome"]       = g_presets[i].nome;
            p["programa"]   = g_presets[i].programa;
            p["pos"]["x"]   = g_presets[i].posX;
            p["pos"]["y"]   = g_presets[i].posY;
            p["pos"]["z"]   = g_presets[i].posZ;
            p["velocidade"] = g_presets[i].velocidade;
            p["status"]     = g_presets[i].status;
        }
        enviar_json(req, 200, doc);
    });

    // ─── POST /estado/preset?id=N ──────────────────────────
    server.on("/estado/preset", HTTP_POST, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] POST /estado/preset"));
        if (bloqueado_por_emergencia(req)) return;
        if (!req->hasParam("id")) { enviar_erro(req, 400, "parametro 'id' ausente (1..5)"); return; }
        int id = req->getParam("id")->value().toInt();
        if (id < 1 || id > NUM_PRESETS) { enviar_erro(req, 400, "id fora do intervalo (1..5)"); return; }

        const Preset& p = g_presets[id - 1];
        EstadoEscultura novo = {};
        novo.programa   = p.programa;
        novo.posX = p.posX; novo.posY = p.posY; novo.posZ = p.posZ;
        novo.velocidade = p.velocidade;
        strlcpy(novo.status, p.status, sizeof(novo.status));
        novo.timestamp  = millis();
        novo.recebidoEm = millis();
        novo.valido     = true;

        log_append("[PRESET] aplicando '%s' (id=%d)", p.nome, id);

        const char* motivo = nullptr;
        bool ok = aplicar_novo_estado(novo, &motivo);
        JsonDocument resp;
        resp["aceito"]   = ok;
        resp["preset"]   = p.nome;
        resp["id"]       = id;
        if (!ok) { resp["emergencia"] = true; resp["motivo"] = motivo; }
        enviar_json(req, ok ? 200 : 503, resp);
    });

    // ─── POST /estado/aleatorio ────────────────────────────
    server.on("/estado/aleatorio", HTTP_POST, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] POST /estado/aleatorio"));
        if (bloqueado_por_emergencia(req)) return;

        EstadoEscultura novo = {};
        preencher_aleatorio(novo);

        log_append("[ALEATORIO] gerando estado");

        const char* motivo = nullptr;
        bool ok = aplicar_novo_estado(novo, &motivo);
        JsonDocument resp;
        resp["aceito"]      = ok;
        resp["aleatorio"]   = true;
        resp["programa"]    = novo.programa;
        resp["pos"]["x"]    = novo.posX;
        resp["pos"]["y"]    = novo.posY;
        resp["pos"]["z"]    = novo.posZ;
        resp["velocidade"]  = novo.velocidade;
        resp["status"]      = novo.status;
        if (!ok) { resp["emergencia"] = true; resp["motivo"] = motivo; }
        enviar_json(req, ok ? 200 : 503, resp);
    });

    // ─── GET /jornadas ─────────────────────────────────────
    server.on("/jornadas", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] GET /jornadas"));
        JsonDocument doc;
        doc["total"]      = g_num_jornadas;
        doc["aberta_idx"] = g_jornada_idx;
        JsonArray arr = doc["jornadas"].to<JsonArray>();
        for (int i = 0; i < g_num_jornadas; i++) {
            JsonObject j   = arr.add<JsonObject>();
            j["idx"]       = i;
            j["id"]        = g_jornadas[i].id;
            j["inicio"]    = g_jornadas[i].inicio;
            j["fim"]       = g_jornadas[i].fim;
            j["encerrada"] = g_jornadas[i].encerrada;
            j["eventos"]   = g_jornadas[i].num_eventos;
        }
        enviar_json(req, 200, doc);
    });

    // ─── GET /jornadas/detalhe?idx=N ───────────────────────
    server.on("/jornadas/detalhe", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] GET /jornadas/detalhe"));
        if (!req->hasParam("idx")) { enviar_erro(req, 400, "parametro idx ausente"); return; }
        int idx = req->getParam("idx")->value().toInt();
        if (idx < 0 || idx >= g_num_jornadas) { enviar_erro(req, 404, "jornada nao encontrada"); return; }
        Jornada& j = g_jornadas[idx];
        JsonDocument doc;
        doc["idx"]         = idx;
        doc["id"]          = j.id;
        doc["inicio"]      = j.inicio;
        doc["fim"]         = j.fim;
        doc["encerrada"]   = j.encerrada;
        doc["num_eventos"] = j.num_eventos;
        JsonArray evs = doc["eventos"].to<JsonArray>();
        for (int k = 0; k < j.num_eventos; k++) {
            JsonObject ev = evs.add<JsonObject>();
            ev["ts"]         = j.eventos[k].ts;
            ev["pos"]["x"]   = j.eventos[k].posX;
            ev["pos"]["y"]   = j.eventos[k].posY;
            ev["pos"]["z"]   = j.eventos[k].posZ;
            ev["velocidade"] = j.eventos[k].velocidade;
            ev["status"]     = j.eventos[k].status;
        }
        enviar_json(req, 200, doc);
    });

    // ─── POST /jornadas/inicio ─────────────────────────────
    auto* h_inicio = new AsyncCallbackJsonWebHandler("/jornadas/inicio",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            Serial.println(F("[HTTP] POST /jornadas/inicio"));
            if (bloqueado_por_emergencia(req)) return;

            JsonObject doc = json.as<JsonObject>();
            const char*   id = doc["id_jornada"] | "J_SEM_ID";
            unsigned long ts = doc["timestamp"]  | (unsigned long)millis();

            if (g_jornada_idx >= 0 && !g_jornadas[g_jornada_idx].encerrada) {
                g_jornadas[g_jornada_idx].encerrada = true;
                g_jornadas[g_jornada_idx].fim = ts;
                Serial.printf("[US07] Jornada '%s' fechada (forcado).\r\n",
                              g_jornadas[g_jornada_idx].id);
                log_append("[US07] jornada '%s' fechada (forcado)",
                           g_jornadas[g_jornada_idx].id);
            }
            if (g_num_jornadas >= MAX_JORNADAS) {
                memmove(&g_jornadas[0], &g_jornadas[1],
                        (MAX_JORNADAS - 1) * sizeof(Jornada));
                g_num_jornadas = MAX_JORNADAS - 1;
            }
            int idx = g_num_jornadas++;
            memset(&g_jornadas[idx], 0, sizeof(Jornada));
            strlcpy(g_jornadas[idx].id, id, sizeof(g_jornadas[idx].id));
            g_jornadas[idx].inicio    = ts;
            g_jornadas[idx].encerrada = false;
            g_jornada_idx = idx;

            Serial.printf("[US07] Jornada INICIADA: %s | ts=%lu\r\n", id, ts);
            log_append("[US07] jornada iniciada: %s", id);

            JsonDocument resp;
            resp["aceito"]     = true;
            resp["id_jornada"] = id;
            resp["idx"]        = idx;
            resp["inicio"]     = ts;
            enviar_json(req, 201, resp);
        });
    h_inicio->setMethod(HTTP_POST);
    server.addHandler(h_inicio);

    // ─── POST /jornadas/fim ────────────────────────────────
    auto* h_fim = new AsyncCallbackJsonWebHandler("/jornadas/fim",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            Serial.println(F("[HTTP] POST /jornadas/fim"));
            if (bloqueado_por_emergencia(req)) return;

            JsonObject doc = json.as<JsonObject>();
            const char*   id = doc["id_jornada"] | "";
            unsigned long ts = doc["timestamp"]  | (unsigned long)millis();

            if (g_jornada_idx < 0) { enviar_erro(req, 409, "nenhuma jornada aberta"); return; }
            Jornada& j = g_jornadas[g_jornada_idx];
            if (id[0] != '\0' && strcmp(j.id, id) != 0) {
                Serial.printf("[US07] AVISO: ID divergente. Esperado='%s' Recebido='%s'\r\n",
                              j.id, id);
            }
            j.fim       = ts;
            j.encerrada = true;
            unsigned long dur = (ts > j.inicio) ? (ts - j.inicio) : 0;
            Serial.printf("[US07] Jornada ENCERRADA: %s | duracao=%lu | eventos=%d\r\n",
                          j.id, dur, j.num_eventos);
            log_append("[US07] jornada encerrada: %s (dur=%lu, eventos=%d)",
                       j.id, dur, j.num_eventos);

            JsonDocument payload;
            payload["id_jornada"]  = j.id;
            payload["inicio"]      = j.inicio;
            payload["fim"]         = j.fim;
            payload["duracao"]     = dur;
            payload["num_eventos"] = j.num_eventos;
            enfileirar_notificacao("JORNADA_FIM", payload);

            JsonDocument resp;
            resp["aceito"]      = true;
            resp["id_jornada"]  = j.id;
            resp["inicio"]      = j.inicio;
            resp["fim"]         = j.fim;
            resp["duracao_s"]   = dur;
            resp["num_eventos"] = j.num_eventos;
            enviar_json(req, 200, resp);
            g_jornada_idx = -1;
        });
    h_fim->setMethod(HTTP_POST);
    server.addHandler(h_fim);

    // ─── POST /emergencia ──────────────────────────────────
    auto* h_emerg = new AsyncCallbackJsonWebHandler("/emergencia",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            Serial.println(F("[HTTP] POST /emergencia"));
            JsonObject doc = json.as<JsonObject>();
            const char* motivo = doc["motivo"] | "manual";
            disparar_emergencia(motivo);
            JsonDocument resp;
            resp["emergencia"] = true;
            resp["motivo"]     = motivo;
            resp["desde"]      = g_emerg_ts;
            enviar_json(req, 200, resp);
        });
    h_emerg->setMethod(HTTP_POST);
    server.addHandler(h_emerg);

    // ─── POST /emergencia/reset ────────────────────────────
    server.on("/emergencia/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] POST /emergencia/reset"));
        g_emergencia = false;
        g_emerg_motivo[0] = '\0';
        g_emerg_ts = 0;
        Serial.println(F("[OK] Emergencia resetada."));
        log_append("[OK] emergencia resetada");
        JsonDocument resp;
        resp["emergencia"] = false;
        resp["aceito"]     = true;
        enviar_json(req, 200, resp);
    });

    // ─── GET /config ───────────────────────────────────────
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] GET /config"));
        JsonDocument doc;
        doc["registrador_url"] = g_registrador_url;
        JsonObject lim = doc["limites"].to<JsonObject>();
        lim["velocidade_max"] = g_lim.velocidade_max;
        lim["delta_pos_max"]  = g_lim.delta_pos_max;
        JsonArray pmin = lim["pos_min"].to<JsonArray>();
        pmin.add(g_lim.pos_min[0]); pmin.add(g_lim.pos_min[1]); pmin.add(g_lim.pos_min[2]);
        JsonArray pmax = lim["pos_max"].to<JsonArray>();
        pmax.add(g_lim.pos_max[0]); pmax.add(g_lim.pos_max[1]); pmax.add(g_lim.pos_max[2]);
        JsonArray prgs = lim["programas_validos"].to<JsonArray>();
        for (int i = 0; i < g_lim.num_programas; i++) prgs.add(g_lim.programas_validos[i]);
        enviar_json(req, 200, doc);
    });

    // ─── POST /config/registrador ──────────────────────────
    auto* h_cfg = new AsyncCallbackJsonWebHandler("/config/registrador",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            Serial.println(F("[HTTP] POST /config/registrador"));
            JsonObject doc = json.as<JsonObject>();
            const char* url = doc["url"] | "";
            strlcpy(g_registrador_url, url, sizeof(g_registrador_url));
            Serial.printf("[CONFIG] registrador_url='%s'\r\n", g_registrador_url);
            log_append("[CONFIG] registrador_url=%s", g_registrador_url);
            JsonDocument resp;
            resp["aceito"]          = true;
            resp["registrador_url"] = g_registrador_url;
            enviar_json(req, 200, resp);
        });
    h_cfg->setMethod(HTTP_POST);
    server.addHandler(h_cfg);

    // ─── GET /logs (txt temporario em RAM) ─────────────────
    server.on("/logs", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] GET /logs"));
        String body(g_log_buf);
        AsyncWebServerResponse* r = req->beginResponse(200,
                                       "text/plain; charset=utf-8", body);
        r->addHeader("Content-Disposition", "inline; filename=\"central_op_logs.txt\"");
        r->addHeader("Access-Control-Allow-Origin", "*");
        req->send(r);
    });

    // ─── DELETE /logs ──────────────────────────────────────
    server.on("/logs", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        Serial.println(F("[HTTP] DELETE /logs"));
        g_log_len = 0;
        g_log_buf[0] = '\0';
        log_append("[LOGS] buffer limpo");
        JsonDocument resp;
        resp["aceito"] = true;
        resp["bytes"]  = g_log_len;
        enviar_json(req, 200, resp);
    });

    // ─── 404 ───────────────────────────────────────────────
    server.onNotFound([](AsyncWebServerRequest* req) {
        enviar_erro(req, 404, "rota nao encontrada");
    });
}
