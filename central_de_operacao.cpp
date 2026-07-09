/**
 * ============================================================
 *  CENTRAL DE OPERACAO  —  Servidor REST (cpp-httplib)
 *  Plataforma: PC (Linux / Windows / macOS) — C++17
 *
 *   1. Recebe estado da escultura                       (US06)
 *   2. Controla jornadas + eventos                      (US07)
 *   3. Detecta anomalias (velocidade/posicao/programa/status/delta)
 *   4. Parada segura (emergencia bloqueia POSTs)
 *   5. Notifica registrador externo (POST HTTP)
 *   6. 5 estados padrao + 1 aleatorio
 *   7. Logs em buffer txt temporario (acessivel via GET /logs)
 *   8. Recebe do Processador de imagem o resultado do reconhecimento
 *      de gestos via POST /gesture_update
 *      { "timestamp": <epoch>, "state": {gesture, confidence,
 *        speed, count} | null }
 *
 *  HTTP: porta 8080  (ajuste HTTP_PORT)
 *
 *  Dependencias (header-only, gerenciadas via CMake FetchContent):
 *    cpp-httplib   — https://github.com/yhirose/cpp-httplib
 *    nlohmann/json — https://github.com/nlohmann/json
 *
 *  Build:
 *    cmake -S . -B build && cmake --build build
 *    ./build/central_operacao        (Linux/macOS)
 *    build\Debug\central_operacao.exe (Windows)
 * ============================================================
 */

/* ─── Portabilidade Windows ─────────────────────────────── */
# ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h>
# include <cstring>
/* strlcpy nao existe no MSVC — implementacao minima */
static size_t strlcpy(char* dst, const char* src, size_t size) {
    if (!size) return std::strlen(src);
    size_t i = 0;
    for (; i < size - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
    return std::strlen(src);
}
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

/* ════════════════════════════════════════════════════════════
   SHIMS  (substitutos das APIs Arduino / ESP32)
   ════════════════════════════════════════════════════════════ */

/* millis() — tempo em ms desde o inicio do programa */
static const auto g_boot = std::chrono::steady_clock::now();
static unsigned long millis() {
    return static_cast<unsigned long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_boot).count());
}

/* random(lo, hi) — intervalo [lo, hi) exclusivo, igual ao Arduino */
static std::mt19937 g_rng{
    static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())
};
static long random(long lo, long hi) {
    if (hi <= lo) return lo;
    return std::uniform_int_distribution<long>(lo, hi - 1)(g_rng);
}

/* ─── Constantes ─────────────────────────────────────────── */
static constexpr int           HTTP_PORT           = 8080;
static constexpr int           MAX_JORNADAS        = 10;
static constexpr int           MAX_EVENTOS_JORNADA = 30;
static constexpr unsigned long ESTADO_TIMEOUT_MS   = 30000;
static constexpr int           HTTP_NOTIF_TIMEOUT  = 5;    /* segundos */
static constexpr int           LOG_BUFFER_SIZE     = 4096;

/* ─── Limites de anomalia ────────────────────────────────── */
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

/* ─── Estruturas ─────────────────────────────────────────── */
struct EstadoEscultura {
    int           programa   = 0;
    float         posX = 0, posY = 0, posZ = 0;
    int           velocidade = 0;
    char          status[24] = {};
    unsigned long timestamp  = 0;
    unsigned long recebidoEm = 0;
    bool          valido     = false;
};

/* Ultimo resultado do reconhecimento de gestos recebido do
   Processador de imagem (camada B), que repassa o estado publicado
   pelo pipeline de visao (YOLO na webcam). */
struct GestoDetectado {
    char          gesture[32] = {};
    double        confidence  = 0.0;
    double        speed       = 0.0;
    long          count       = 0;
    double        timestamp   = 0.0;   /* epoch (s) enviado pelo detector */
    unsigned long recebidoEm  = 0;
    bool          valido      = false;
};

struct EventoJornada {
    unsigned long ts         = 0;
    float         posX = 0, posY = 0, posZ = 0;
    int           velocidade = 0;
    char          status[16] = {};
};

struct Jornada {
    char          id[32]                       = {};
    unsigned long inicio                       = 0;
    unsigned long fim                          = 0;
    bool          encerrada                    = false;
    EventoJornada eventos[MAX_EVENTOS_JORNADA] = {};
    int           num_eventos                  = 0;
};

/* ─── Presets de estado ──────────────────────────────────── */
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
static constexpr int NUM_PRESETS = 5;

/* ─── Estado global + mutex ──────────────────────────────── */
/*
 * std::recursive_mutex permite que log_append (que adquire o lock)
 * seja chamado de dentro de secoes ja travadas (ex: disparar_emergencia).
 */
static std::recursive_mutex g_mutex;

static EstadoEscultura g_estado          = {};
static EstadoEscultura g_estado_anterior = {};
static Jornada         g_jornadas[MAX_JORNADAS] = {};
static int             g_num_jornadas    = 0;
static int             g_jornada_idx     = -1;
static unsigned long   g_ultimo_update   = 0;

static GestoDetectado  g_gesto           = {};
static unsigned long   g_gestos_recebidos = 0;

static bool            g_emergencia      = false;
static char            g_emerg_motivo[96]= {};
static unsigned long   g_emerg_ts        = 0;

static char            g_registrador_url[128] = {};

/* ─── Log buffer (txt temporario em RAM) ─────────────────── */
static char   g_log_buf[LOG_BUFFER_SIZE + 1] = {};
static size_t g_log_len                       = 0;

/* ─── Fila de notificacoes (processada em background) ─────── */
struct Notificacao {
    char tipo[32]  = {};
    char body[640] = {};
    bool pendente  = false;
};
static Notificacao g_notif = {};

/* ─── Servidor HTTP ──────────────────────────────────────── */
static httplib::Server g_server;

/* ════════════════════════════════════════════════════════════
   PROTOTIPOS
   ════════════════════════════════════════════════════════════ */
static void        registrar_rotas();
static void        log_append(const char* fmt, ...);
static void        verificar_timeout();
static void        enfileirar_notificacao(const char* tipo, const json& payload);
static void        processar_notificacao_pendente();
static const char* detectar_anomalia(const EstadoEscultura& novo,
                                     const EstadoEscultura& ant);
static void        disparar_emergencia(const char* motivo);
static void        registrar_evento_jornada(const EstadoEscultura& e);
static bool        aplicar_novo_estado(const EstadoEscultura& novo,
                                       const char** out_motivo);
static void        preencher_aleatorio(EstadoEscultura& e);

/* ════════════════════════════════════════════════════════════
   LOG BUFFER (txt temporario em RAM)
   ════════════════════════════════════════════════════════════ */
static void log_append(const char* fmt, ...) {
    char msg[180];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::lock_guard<std::recursive_mutex> lk(g_mutex);

    char line[200];
    int total = std::snprintf(line, (int)sizeof(line) - 3,
                              "[%lus] %s", millis() / 1000, msg);
    if (total < 0) return;
    if (total > (int)sizeof(line) - 3) total = (int)sizeof(line) - 3;
    line[total++] = '\r';
    line[total++] = '\n';
    line[total]   = '\0';

    if (g_log_len + (size_t)total >= LOG_BUFFER_SIZE) {
        size_t drop = g_log_len / 2;
        char* nl = (char*)std::memchr(g_log_buf + drop, '\n', g_log_len - drop);
        if (nl) drop = (size_t)(nl - g_log_buf) + 1;
        std::memmove(g_log_buf, g_log_buf + drop, g_log_len - drop);
        g_log_len -= drop;
    }
    std::memcpy(g_log_buf + g_log_len, line, total);
    g_log_len += total;
    g_log_buf[g_log_len] = '\0';

    std::printf("%s", line);   /* espelho no stdout */
}

/* ════════════════════════════════════════════════════════════
   HELPERS HTTP
   ════════════════════════════════════════════════════════════ */
static void enviar_json(httplib::Response& res, int code, const json& doc) {
    res.status = code;
    res.set_content(doc.dump(), "application/json");
    res.set_header("Access-Control-Allow-Origin", "*");
}

static void enviar_erro(httplib::Response& res, int code, const char* msg) {
    json doc;
    doc["erro"] = msg;
    enviar_json(res, code, doc);
}

/* Retorna true (e preenche res com 503) se o sistema estiver em emergencia */
static bool bloqueado_por_emergencia(httplib::Response& res) {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    if (!g_emergencia) return false;
    json doc;
    doc["erro"]   = "sistema em parada de emergencia";
    doc["motivo"] = g_emerg_motivo;
    doc["desde"]  = g_emerg_ts;
    enviar_json(res, 503, doc);
    return true;
}

/* ════════════════════════════════════════════════════════════
   DETECCAO DE ANOMALIA
   ════════════════════════════════════════════════════════════ */
static const char* detectar_anomalia(const EstadoEscultura& n,
                                     const EstadoEscultura& ant) {
    static char buf[80];

    if (n.velocidade > g_lim.velocidade_max || n.velocidade < 0) {
        std::snprintf(buf, sizeof(buf),
                      "velocidade fora dos limites: %d", n.velocidade);
        return buf;
    }
    const float pos[3] = { n.posX, n.posY, n.posZ };
    const char* eixos  = "xyz";
    for (int i = 0; i < 3; i++) {
        if (pos[i] < g_lim.pos_min[i] || pos[i] > g_lim.pos_max[i]) {
            std::snprintf(buf, sizeof(buf),
                          "posicao %c=%.1f fora da zona segura",
                          eixos[i], pos[i]);
            return buf;
        }
    }
    bool prog_ok = false;
    for (int i = 0; i < g_lim.num_programas; i++) {
        if (g_lim.programas_validos[i] == n.programa) { prog_ok = true; break; }
    }
    if (!prog_ok) {
        std::snprintf(buf, sizeof(buf),
                      "programa nao autorizado: %d", n.programa);
        return buf;
    }
    if (std::strcmp(n.status, "erro")   == 0 ||
        std::strcmp(n.status, "falha")  == 0 ||
        std::strcmp(n.status, "panico") == 0) {
        std::snprintf(buf, sizeof(buf),
                      "status reportado de falha: %s", n.status);
        return buf;
    }
    if (ant.valido) {
        float dx   = n.posX - ant.posX;
        float dy   = n.posY - ant.posY;
        float dz   = n.posZ - ant.posZ;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dist > g_lim.delta_pos_max) {
            std::snprintf(buf, sizeof(buf),
                          "salto brusco de posicao: %.1f", dist);
            return buf;
        }
    }
    return nullptr;
}

/* NOTA: deve ser chamada com g_mutex ja adquirido */
static void disparar_emergencia(const char* motivo) {
    g_emergencia = true;
    strlcpy(g_emerg_motivo, motivo, sizeof(g_emerg_motivo));
    g_emerg_ts = millis();

    std::printf("[!!!] PARADA DE EMERGENCIA: %s\n", motivo);
    log_append("[EMERGENCIA] %s", motivo);

    json payload;
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
        g_jornadas[g_jornada_idx].fim       = millis();
        g_jornadas[g_jornada_idx].encerrada = true;
        std::printf("[US07] Jornada '%s' fechada por emergencia.\n",
                    g_jornadas[g_jornada_idx].id);
        log_append("[US07] jornada '%s' fechada por emergencia",
                   g_jornadas[g_jornada_idx].id);
    }
}

static void registrar_evento_jornada(const EstadoEscultura& e) {
    if (g_jornada_idx < 0) return;
    Jornada& j = g_jornadas[g_jornada_idx];
    if (j.encerrada) return;

    if (j.num_eventos >= MAX_EVENTOS_JORNADA) {
        std::memmove(&j.eventos[0], &j.eventos[1],
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

/* NOTA: deve ser chamada com g_mutex adquirido */
static bool aplicar_novo_estado(const EstadoEscultura& novo,
                                const char** out_motivo) {
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

    std::printf("[US06] prog=%d | pos=(%.1f,%.1f,%.1f) | vel=%d | status=%s\n",
                novo.programa, novo.posX, novo.posY, novo.posZ,
                novo.velocidade, novo.status);
    log_append("[US06] prog=%d pos=(%.1f,%.1f,%.1f) vel=%d status=%s",
               novo.programa, novo.posX, novo.posY, novo.posZ,
               novo.velocidade, novo.status);

    if (g_jornada_idx >= 0 && !g_jornadas[g_jornada_idx].encerrada)
        registrar_evento_jornada(g_estado);

    if (out_motivo) *out_motivo = nullptr;
    return true;
}

/* Estado aleatorio — respeita delta para nao disparar emergencia imediata.
   NOTA: deve ser chamada com g_mutex adquirido */
static void preencher_aleatorio(EstadoEscultura& e) {
    e.programa = g_lim.programas_validos[random(0, g_lim.num_programas)];

    float baseX    = g_estado.valido ? g_estado.posX : 0.0f;
    float baseY    = g_estado.valido ? g_estado.posY : 0.0f;
    float baseZ    = g_estado.valido ? g_estado.posZ : 0.0f;
    float maxDelta = g_lim.delta_pos_max * 0.5f;
    e.posX = baseX + (random(-1000, 1001) / 1000.0f) * maxDelta;
    e.posY = baseY + (random(-1000, 1001) / 1000.0f) * maxDelta;
    e.posZ = baseZ + (random(-1000, 1001) / 1000.0f) * maxDelta;

    /* Clip para dentro dos limites */
    for (int i = 0; i < 3; i++) {
        float& p = (i == 0 ? e.posX : (i == 1 ? e.posY : e.posZ));
        if (p < g_lim.pos_min[i]) p = g_lim.pos_min[i];
        if (p > g_lim.pos_max[i]) p = g_lim.pos_max[i];
    }

    e.velocidade = (int)random(0, g_lim.velocidade_max + 1);

    const char* statuses[] = { "executando", "parado", "manutencao", "ocioso" };
    strlcpy(e.status, statuses[random(0, 4)], sizeof(e.status));

    e.timestamp  = millis();
    e.recebidoEm = millis();
    e.valido     = true;
}

/* ════════════════════════════════════════════════════════════
   NOTIFICACOES (POST HTTP para registrador externo)
   ════════════════════════════════════════════════════════════ */

/* NOTA: deve ser chamada com g_mutex adquirido */
static void enfileirar_notificacao(const char* tipo, const json& payload) {
    json env;
    env["origem"]    = "central_operacao";
    env["tipo"]      = tipo;
    env["timestamp"] = millis();
    env["dados"]     = payload;

    std::string body = env.dump();
    strlcpy(g_notif.tipo, tipo,         sizeof(g_notif.tipo));
    strlcpy(g_notif.body, body.c_str(), sizeof(g_notif.body));
    g_notif.pendente = true;
}

/* Parseia "http://host:port/caminho" nos seus componentes */
static bool parse_url(const char* url,
                      std::string& host, int& port, std::string& path) {
    std::string s(url);
    auto schemeEnd = s.find("://");
    if (schemeEnd == std::string::npos) return false;
    s = s.substr(schemeEnd + 3);

    auto slash = s.find('/');
    std::string hostPort;
    if (slash == std::string::npos) {
        hostPort = s;
        path     = "/";
    } else {
        hostPort = s.substr(0, slash);
        path     = s.substr(slash);
    }

    auto colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        try { port = std::stoi(hostPort.substr(colon + 1)); }
        catch (...) { return false; }
    } else {
        host = hostPort;
        port = 80;
    }
    return !host.empty();
}

static void processar_notificacao_pendente() {
    /* Copia dados com lock; envia HTTP sem lock (operacao bloqueante) */
    std::unique_lock<std::recursive_mutex> lk(g_mutex);
    if (!g_notif.pendente) return;
    g_notif.pendente = false;
    char tipo[32], body[640], url[128];
    std::memcpy(tipo, g_notif.tipo,      sizeof(tipo));
    std::memcpy(body, g_notif.body,      sizeof(body));
    std::memcpy(url,  g_registrador_url, sizeof(url));
    lk.unlock();

    if (url[0] == '\0') {
        std::printf("[REGISTRADOR] (URL nao configurada) tipo=%s\n", tipo);
        log_append("[REGISTRADOR] %s (sem URL configurada)", tipo);
        return;
    }

    std::string host, path;
    int port = 80;
    if (!parse_url(url, host, port, path)) {
        std::printf("[REGISTRADOR] URL invalida: %s\n", url);
        log_append("[REGISTRADOR] URL invalida: %s", url);
        return;
    }

    httplib::Client cli(host, port);
    cli.set_connection_timeout(HTTP_NOTIF_TIMEOUT, 0);
    cli.set_read_timeout(HTTP_NOTIF_TIMEOUT, 0);

    auto res = cli.Post(path, body, "application/json");
    if (res) {
        std::printf("[REGISTRADOR] %s -> %d\n", tipo, res->status);
        log_append("[REGISTRADOR] %s -> HTTP %d", tipo, res->status);
    } else {
        std::printf("[REGISTRADOR] erro %s -> %s\n", tipo,
                    httplib::to_string(res.error()).c_str());
        log_append("[REGISTRADOR] erro %s", tipo);
    }
}

/* ════════════════════════════════════════════════════════════
   TIMEOUT
   ════════════════════════════════════════════════════════════ */
static void verificar_timeout() {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    if (!g_estado.valido) return;
    if (millis() - g_ultimo_update <= ESTADO_TIMEOUT_MS) return;
    if (std::strcmp(g_estado.status, "sem_sinal") == 0) return;
    strlcpy(g_estado.status, "sem_sinal", sizeof(g_estado.status));
    g_estado.valido = false;
    std::printf("[US06] AVISO: timeout — status=sem_sinal\n");
    log_append("[US06] timeout: status=sem_sinal");
}

/* ════════════════════════════════════════════════════════════
   ROTAS
   ════════════════════════════════════════════════════════════ */
static void registrar_rotas() {

    /* ─── GET / ──────────────────────────────────────────── */
    g_server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        doc["servico"]    = "central_operacao";
        doc["versao"]     = "3.0";
        doc["uptime_s"]   = millis() / 1000;
        doc["emergencia"] = g_emergencia;
        if (g_emergencia) doc["motivo_emergencia"] = g_emerg_motivo;
        doc["rotas"] = json::array({
            "GET    /estado",
            "POST   /estado",
            "POST   /gesture_update    (Processador de imagem)",
            "GET    /gesto",
            "GET    /estado/presets",
            "POST   /estado/preset?id=N        (1..5)",
            "POST   /estado/aleatorio",
            "GET    /jornadas",
            "GET    /jornadas/detalhe?idx=N",
            "POST   /jornadas/inicio",
            "POST   /jornadas/fim",
            "POST   /emergencia",
            "POST   /emergencia/reset",
            "GET    /config",
            "POST   /config/registrador",
            "GET    /logs            (txt temporario)",
            "DELETE /logs            (limpa buffer)"
        });
        enviar_json(res, 200, doc);
    });

    /* ─── GET /estado ────────────────────────────────────── */
    g_server.Get("/estado", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /estado\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
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
        enviar_json(res, 200, doc);
    });

    /* ─── POST /estado ───────────────────────────────────── */
    g_server.Post("/estado", [](const httplib::Request& req,
                                httplib::Response& res) {
        std::printf("[HTTP] POST /estado\n");
        if (bloqueado_por_emergencia(res)) return;

        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        EstadoEscultura novo = {};
        novo.programa   = body.value("programa",   0);
        novo.posX       = body.contains("posicao")
                          ? body["posicao"].value("x", 0.0f) : 0.0f;
        novo.posY       = body.contains("posicao")
                          ? body["posicao"].value("y", 0.0f) : 0.0f;
        novo.posZ       = body.contains("posicao")
                          ? body["posicao"].value("z", 0.0f) : 0.0f;
        novo.velocidade = body.value("velocidade", 0);
        strlcpy(novo.status,
                body.value("status", std::string("desconhecido")).c_str(),
                sizeof(novo.status));
        novo.timestamp  = body.value("timestamp",
                                     static_cast<unsigned long>(millis()));
        novo.recebidoEm = millis();
        novo.valido     = true;

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        const char* motivo = nullptr;
        bool ok = aplicar_novo_estado(novo, &motivo);

        if (!ok) {
            json resp;
            resp["aceito"]     = false;
            resp["emergencia"] = true;
            resp["motivo"]     = motivo ? motivo : "";
            enviar_json(res, 503, resp);
            return;
        }
        json resp;
        resp["origem"]     = "central_op";
        resp["tipo"]       = "estado_pub";
        resp["aceito"]     = true;
        resp["programa"]   = g_estado.programa;
        resp["status"]     = g_estado.status;
        resp["velocidade"] = g_estado.velocidade;
        enviar_json(res, 200, resp);
    });

    /* ─── POST /gesture_update ───────────────────────────── *
     * Dados de monitoramento enviados pelo Processador de imagem
     * (camada B), com o resultado do reconhecimento de gestos.
     * Body:
     *   { "timestamp": 1720375934.5,
     *     "state": { "gesture":    "REST",
     *                "confidence": 1.0,
     *                "speed":      0.0,
     *                "count":      0 } | null }
     * ("state" pode ser null enquanto o detector nao publicou estado;
     *  campos extras sao ignorados)
     */
    g_server.Post("/gesture_update", [](const httplib::Request& req,
                                        httplib::Response& res) {
        std::printf("[HTTP] POST /gesture_update\n");
        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        if (!body.contains("timestamp") || !body["timestamp"].is_number()) {
            enviar_erro(res, 400, "campo timestamp obrigatorio"); return;
        }

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        g_gestos_recebidos++;

        if (!body.contains("state") || body["state"].is_null()) {
            log_append("[GESTO] payload sem state (detector iniciando)");
            json resp;
            resp["aceito"] = true;
            resp["motivo"] = "state nulo, nada a processar";
            enviar_json(res, 200, resp);
            return;
        }
        const json& st = body["state"];
        if (!st.is_object()) {
            enviar_erro(res, 400, "state deve ser objeto ou null"); return;
        }

        strlcpy(g_gesto.gesture,
                st.value("gesture", std::string("REST")).c_str(),
                sizeof(g_gesto.gesture));
        g_gesto.confidence = st.value("confidence", 0.0);
        g_gesto.speed      = st.value("speed",      0.0);
        g_gesto.count      = (long)st.value("count", 0);
        g_gesto.timestamp  = body.value("timestamp", 0.0);
        g_gesto.recebidoEm = millis();
        g_gesto.valido     = true;

        std::printf("[GESTO] %s conf=%.2f speed=%.2f count=%ld\n",
                    g_gesto.gesture, g_gesto.confidence,
                    g_gesto.speed, g_gesto.count);
        log_append("[GESTO] %s conf=%.2f speed=%.2f count=%ld",
                   g_gesto.gesture, g_gesto.confidence,
                   g_gesto.speed, g_gesto.count);

        json resp;
        resp["aceito"]     = true;
        resp["gesto"]      = g_gesto.gesture;
        resp["emergencia"] = g_emergencia;
        enviar_json(res, 200, resp);
    });

    /* ─── GET /gesto ─────────────────────────────────────── */
    g_server.Get("/gesto", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /gesto\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        doc["valido"]      = g_gesto.valido;
        doc["gesture"]     = g_gesto.gesture;
        doc["confidence"]  = g_gesto.confidence;
        doc["speed"]       = g_gesto.speed;
        doc["count"]       = g_gesto.count;
        doc["timestamp"]   = g_gesto.timestamp;
        doc["recebido_ms"] = g_gesto.recebidoEm;
        doc["recebidos"]   = g_gestos_recebidos;
        enviar_json(res, 200, doc);
    });

    /* ─── GET /estado/presets ────────────────────────────── */
    g_server.Get("/estado/presets",
                 [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /estado/presets\n");
        json doc;
        doc["presets"] = json::array();
        for (int i = 0; i < NUM_PRESETS; i++) {
            json p;
            p["id"]         = i + 1;
            p["nome"]       = g_presets[i].nome;
            p["programa"]   = g_presets[i].programa;
            p["pos"]["x"]   = g_presets[i].posX;
            p["pos"]["y"]   = g_presets[i].posY;
            p["pos"]["z"]   = g_presets[i].posZ;
            p["velocidade"] = g_presets[i].velocidade;
            p["status"]     = g_presets[i].status;
            doc["presets"].push_back(p);
        }
        enviar_json(res, 200, doc);
    });

    /* ─── POST /estado/preset?id=N ───────────────────────── */
    g_server.Post("/estado/preset", [](const httplib::Request& req,
                                       httplib::Response& res) {
        std::printf("[HTTP] POST /estado/preset\n");
        if (bloqueado_por_emergencia(res)) return;
        if (!req.has_param("id")) {
            enviar_erro(res, 400, "parametro 'id' ausente (1..5)"); return;
        }
        int id = 0;
        try { id = std::stoi(req.get_param_value("id")); }
        catch (...) { id = 0; }
        if (id < 1 || id > NUM_PRESETS) {
            enviar_erro(res, 400, "id fora do intervalo (1..5)"); return;
        }

        const Preset& p = g_presets[id - 1];
        EstadoEscultura novo = {};
        novo.programa   = p.programa;
        novo.posX = p.posX; novo.posY = p.posY; novo.posZ = p.posZ;
        novo.velocidade = p.velocidade;
        strlcpy(novo.status, p.status, sizeof(novo.status));
        novo.timestamp  = millis();
        novo.recebidoEm = millis();
        novo.valido     = true;

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        log_append("[PRESET] aplicando '%s' (id=%d)", p.nome, id);

        const char* motivo = nullptr;
        bool ok = aplicar_novo_estado(novo, &motivo);
        json resp;
        resp["aceito"] = ok;
        resp["preset"] = p.nome;
        resp["id"]     = id;
        if (!ok) { resp["emergencia"] = true; resp["motivo"] = motivo ? motivo : ""; }
        enviar_json(res, ok ? 200 : 503, resp);
    });

    /* ─── POST /estado/aleatorio ─────────────────────────── */
    g_server.Post("/estado/aleatorio", [](const httplib::Request&,
                                          httplib::Response& res) {
        std::printf("[HTTP] POST /estado/aleatorio\n");
        if (bloqueado_por_emergencia(res)) return;

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        EstadoEscultura novo = {};
        preencher_aleatorio(novo);
        log_append("[ALEATORIO] gerando estado");

        const char* motivo = nullptr;
        bool ok = aplicar_novo_estado(novo, &motivo);
        json resp;
        resp["aceito"]     = ok;
        resp["aleatorio"]  = true;
        resp["programa"]   = novo.programa;
        resp["pos"]["x"]   = novo.posX;
        resp["pos"]["y"]   = novo.posY;
        resp["pos"]["z"]   = novo.posZ;
        resp["velocidade"] = novo.velocidade;
        resp["status"]     = novo.status;
        if (!ok) { resp["emergencia"] = true; resp["motivo"] = motivo ? motivo : ""; }
        enviar_json(res, ok ? 200 : 503, resp);
    });

    /* ─── GET /jornadas ──────────────────────────────────── */
    g_server.Get("/jornadas", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /jornadas\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        doc["total"]      = g_num_jornadas;
        doc["aberta_idx"] = g_jornada_idx;
        doc["jornadas"]   = json::array();
        for (int i = 0; i < g_num_jornadas; i++) {
            json j;
            j["idx"]       = i;
            j["id"]        = g_jornadas[i].id;
            j["inicio"]    = g_jornadas[i].inicio;
            j["fim"]       = g_jornadas[i].fim;
            j["encerrada"] = g_jornadas[i].encerrada;
            j["eventos"]   = g_jornadas[i].num_eventos;
            doc["jornadas"].push_back(j);
        }
        enviar_json(res, 200, doc);
    });

    /* ─── GET /jornadas/detalhe?idx=N ────────────────────── */
    g_server.Get("/jornadas/detalhe", [](const httplib::Request& req,
                                         httplib::Response& res) {
        std::printf("[HTTP] GET /jornadas/detalhe\n");
        if (!req.has_param("idx")) {
            enviar_erro(res, 400, "parametro idx ausente"); return;
        }
        int idx = -1;
        try { idx = std::stoi(req.get_param_value("idx")); }
        catch (...) { idx = -1; }

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (idx < 0 || idx >= g_num_jornadas) {
            enviar_erro(res, 404, "jornada nao encontrada"); return;
        }
        Jornada& j = g_jornadas[idx];
        json doc;
        doc["idx"]         = idx;
        doc["id"]          = j.id;
        doc["inicio"]      = j.inicio;
        doc["fim"]         = j.fim;
        doc["encerrada"]   = j.encerrada;
        doc["num_eventos"] = j.num_eventos;
        doc["eventos"]     = json::array();
        for (int k = 0; k < j.num_eventos; k++) {
            json ev;
            ev["ts"]         = j.eventos[k].ts;
            ev["pos"]["x"]   = j.eventos[k].posX;
            ev["pos"]["y"]   = j.eventos[k].posY;
            ev["pos"]["z"]   = j.eventos[k].posZ;
            ev["velocidade"] = j.eventos[k].velocidade;
            ev["status"]     = j.eventos[k].status;
            doc["eventos"].push_back(ev);
        }
        enviar_json(res, 200, doc);
    });

    /* ─── POST /jornadas/inicio ──────────────────────────── */
    g_server.Post("/jornadas/inicio", [](const httplib::Request& req,
                                         httplib::Response& res) {
        std::printf("[HTTP] POST /jornadas/inicio\n");
        if (bloqueado_por_emergencia(res)) return;

        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        std::string   id_jornada = body.value("id_jornada",
                                               std::string("J_SEM_ID"));
        unsigned long ts         = body.value("timestamp",
                                               static_cast<unsigned long>(millis()));

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (g_jornada_idx >= 0 && !g_jornadas[g_jornada_idx].encerrada) {
            g_jornadas[g_jornada_idx].encerrada = true;
            g_jornadas[g_jornada_idx].fim = ts;
            std::printf("[US07] Jornada '%s' fechada (forcado).\n",
                        g_jornadas[g_jornada_idx].id);
            log_append("[US07] jornada '%s' fechada (forcado)",
                       g_jornadas[g_jornada_idx].id);
        }
        if (g_num_jornadas >= MAX_JORNADAS) {
            std::memmove(&g_jornadas[0], &g_jornadas[1],
                         (MAX_JORNADAS - 1) * sizeof(Jornada));
            g_num_jornadas = MAX_JORNADAS - 1;
        }
        int new_idx = g_num_jornadas++;
        std::memset(&g_jornadas[new_idx], 0, sizeof(Jornada));
        strlcpy(g_jornadas[new_idx].id, id_jornada.c_str(),
                sizeof(g_jornadas[new_idx].id));
        g_jornadas[new_idx].inicio    = ts;
        g_jornadas[new_idx].encerrada = false;
        g_jornada_idx = new_idx;

        std::printf("[US07] Jornada INICIADA: %s | ts=%lu\n",
                    id_jornada.c_str(), ts);
        log_append("[US07] jornada iniciada: %s", id_jornada.c_str());

        json resp;
        resp["aceito"]     = true;
        resp["id_jornada"] = id_jornada;
        resp["idx"]        = new_idx;
        resp["inicio"]     = ts;
        enviar_json(res, 201, resp);
    });

    /* ─── POST /jornadas/fim ─────────────────────────────── */
    g_server.Post("/jornadas/fim", [](const httplib::Request& req,
                                      httplib::Response& res) {
        std::printf("[HTTP] POST /jornadas/fim\n");
        if (bloqueado_por_emergencia(res)) return;

        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        std::string   id_jornada = body.value("id_jornada", std::string(""));
        unsigned long ts         = body.value("timestamp",
                                               static_cast<unsigned long>(millis()));

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (g_jornada_idx < 0) {
            enviar_erro(res, 409, "nenhuma jornada aberta"); return;
        }
        Jornada& j = g_jornadas[g_jornada_idx];
        if (!id_jornada.empty() && id_jornada != j.id) {
            std::printf("[US07] AVISO: ID divergente. Esperado='%s' Recebido='%s'\n",
                        j.id, id_jornada.c_str());
        }
        j.fim       = ts;
        j.encerrada = true;
        unsigned long dur = (ts > j.inicio) ? (ts - j.inicio) : 0;
        std::printf("[US07] Jornada ENCERRADA: %s | duracao=%lu | eventos=%d\n",
                    j.id, dur, j.num_eventos);
        log_append("[US07] jornada encerrada: %s (dur=%lu, eventos=%d)",
                   j.id, dur, j.num_eventos);

        json payload;
        payload["id_jornada"]  = j.id;
        payload["inicio"]      = j.inicio;
        payload["fim"]         = j.fim;
        payload["duracao"]     = dur;
        payload["num_eventos"] = j.num_eventos;
        enfileirar_notificacao("JORNADA_FIM", payload);

        json resp;
        resp["aceito"]      = true;
        resp["id_jornada"]  = j.id;
        resp["inicio"]      = j.inicio;
        resp["fim"]         = j.fim;
        resp["duracao_s"]   = dur;
        resp["num_eventos"] = j.num_eventos;
        enviar_json(res, 200, resp);
        g_jornada_idx = -1;
    });

    /* ─── POST /emergencia ───────────────────────────────── */
    g_server.Post("/emergencia", [](const httplib::Request& req,
                                    httplib::Response& res) {
        std::printf("[HTTP] POST /emergencia\n");
        json body;
        try { body = json::parse(req.body); } catch (...) { body = {}; }
        std::string motivo = body.value("motivo", std::string("manual"));

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        disparar_emergencia(motivo.c_str());
        json resp;
        resp["emergencia"] = true;
        resp["motivo"]     = motivo;
        resp["desde"]      = g_emerg_ts;
        enviar_json(res, 200, resp);
    });

    /* ─── POST /emergencia/reset ─────────────────────────── */
    g_server.Post("/emergencia/reset",
                  [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] POST /emergencia/reset\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        g_emergencia      = false;
        g_emerg_motivo[0] = '\0';
        g_emerg_ts        = 0;
        std::printf("[OK] Emergencia resetada.\n");
        log_append("[OK] emergencia resetada");
        json resp;
        resp["emergencia"] = false;
        resp["aceito"]     = true;
        enviar_json(res, 200, resp);
    });

    /* ─── GET /config ────────────────────────────────────── */
    g_server.Get("/config", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /config\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        doc["registrador_url"]             = g_registrador_url;
        doc["limites"]["velocidade_max"]   = g_lim.velocidade_max;
        doc["limites"]["delta_pos_max"]    = g_lim.delta_pos_max;
        doc["limites"]["pos_min"]          = { g_lim.pos_min[0],
                                               g_lim.pos_min[1],
                                               g_lim.pos_min[2] };
        doc["limites"]["pos_max"]          = { g_lim.pos_max[0],
                                               g_lim.pos_max[1],
                                               g_lim.pos_max[2] };
        json prgs = json::array();
        for (int i = 0; i < g_lim.num_programas; i++)
            prgs.push_back(g_lim.programas_validos[i]);
        doc["limites"]["programas_validos"] = prgs;
        enviar_json(res, 200, doc);
    });

    /* ─── POST /config/registrador ───────────────────────── */
    g_server.Post("/config/registrador", [](const httplib::Request& req,
                                             httplib::Response& res) {
        std::printf("[HTTP] POST /config/registrador\n");
        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }
        std::string url = body.value("url", std::string(""));

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        strlcpy(g_registrador_url, url.c_str(), sizeof(g_registrador_url));
        std::printf("[CONFIG] registrador_url='%s'\n", g_registrador_url);
        log_append("[CONFIG] registrador_url=%s", g_registrador_url);

        json resp;
        resp["aceito"]          = true;
        resp["registrador_url"] = g_registrador_url;
        enviar_json(res, 200, resp);
    });

    /* ─── GET /logs (txt temporario em RAM) ──────────────── */
    g_server.Get("/logs", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /logs\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        res.status = 200;
        res.set_content(g_log_buf, "text/plain; charset=utf-8");
        res.set_header("Content-Disposition",
                       "inline; filename=\"central_op_logs.txt\"");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    /* ─── DELETE /logs ───────────────────────────────────── */
    g_server.Delete("/logs", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] DELETE /logs\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        g_log_len    = 0;
        g_log_buf[0] = '\0';
        log_append("[LOGS] buffer limpo");
        json resp;
        resp["aceito"] = true;
        resp["bytes"]  = 0;
        enviar_json(res, 200, resp);
    });

    /* ─── 404 ────────────────────────────────────────────── */
    g_server.set_error_handler(
        [](const httplib::Request&, httplib::Response& res) {
            if (res.status == 404)
                enviar_erro(res, 404, "rota nao encontrada");
        });
}

/* ════════════════════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════════════════════ */
int main() {
    std::printf("\n");
    std::printf("=== CENTRAL DE OPERACAO (REST — cpp-httplib) ===\n");
    log_append("Sistema iniciando");

    registrar_rotas();

    /*
     * Thread de background:
     *   — verifica timeout do estado (US06)
     *   — processa fila de notificacoes HTTP
     * Equivalente ao loop() do Arduino.
     */
    std::thread bg([] {
        while (true) {
            verificar_timeout();
            processar_notificacao_pendente();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    bg.detach();

    std::printf("[HTTP] Servidor REST iniciando em http://0.0.0.0:%d\n", HTTP_PORT);
    log_append("Servidor REST pronto na porta %d", HTTP_PORT);
    std::printf("========================================\n");

    /* Bloqueante — equivalente ao server.begin() + loop() do AsyncWebServer */
    if (!g_server.listen("0.0.0.0", HTTP_PORT)) {
        std::printf("[ERRO] Falha ao iniciar servidor na porta %d\n", HTTP_PORT);
        return 1;
    }
    return 0;
}