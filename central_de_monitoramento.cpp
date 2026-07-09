/**
 * ============================================================
 *  CENTRAL DE MONITORAMENTO  —  Servidor REST (cpp-httplib)
 *  (Operador de rede e sistema)
 *  Camada B - Plataforma  |  Plataforma: Jetson Orin Nano (C++17)
 *
 *  Componente intermediario (Middleware) entre a camada de
 *  Biblioteca (C) e a camada de Aplicacao (A).
 *
 *  Responsabilidades (Especificacao - camada B, item 4):
 *    a. Recebe de forma sincrona dados de monitoramento da camada
 *       inferior (Monitor Jetson / Monitor Wemos / Monitor P4),
 *       bufferizando e agregando, para repasse SOMENTE das
 *       informacoes importantes ao componente da camada superior:
 *       a Central de operacao.
 *
 *  --- INTERFACE COM A CAMADA INFERIOR (C - Biblioteca) ---
 *    POST /monitoramento   <- monitores reportam amostras
 *      { "origem": "monitor_jetson|monitor_wemos|monitor_p4",
 *        "tipo":   "estado|sistema|rede|seguranca",
 *        "dados":  { ... } }
 *
 *  --- INTERFACE COM A CAMADA SUPERIOR (A - Aplicacao) ---
 *    Repassa o estado consolidado a Central de operacao via
 *      POST {central_op_url}/estado
 *    no formato esperado por ela:
 *      { "programa":N, "posicao":{x,y,z},
 *        "velocidade":N, "status":"...", "timestamp":N }
 *
 *  HTTP: porta 8091  (ajuste HTTP_PORT)
 *
 *  Dependencias (header-only, via CMake FetchContent):
 *    cpp-httplib   — https://github.com/yhirose/cpp-httplib
 *    nlohmann/json — https://github.com/nlohmann/json
 *
 *  Build:
 *    cmake -S . -B build && cmake --build build
 *    ./build/central_monitoramento          (Linux/macOS)
 *    build\Debug\central_monitoramento.exe   (Windows)
 * ============================================================
 */

/* ─── Portabilidade Windows ─────────────────────────────── */
# ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h>
#endif
#include <cstring>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <chrono>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <map>

#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

/* ════════════════════════════════════════════════════════════
   SHIMS
   ════════════════════════════════════════════════════════════ */
static const auto g_boot = std::chrono::steady_clock::now();
static unsigned long millis() {
    return static_cast<unsigned long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_boot).count());
}

/* ─── Constantes ─────────────────────────────────────────── */
static constexpr int           HTTP_PORT          = 8091;
static constexpr int           MAX_BUFFER         = 64;     /* amostras retidas */
static constexpr int           HTTP_NOTIF_TIMEOUT = 5;      /* segundos         */
static constexpr int           LOG_BUFFER_SIZE    = 4096;
static constexpr unsigned long PERIODO_REPASSE_MS = 5000;   /* heartbeat        */

/* ─── Amostra de monitoramento recebida da camada C ──────── */
struct Amostra {
    std::string   origem;
    std::string   tipo;
    json          dados;
    unsigned long recebida_em = 0;
};

/* ─── Estado consolidado repassado a Central de operacao ─── */
struct EstadoConsolidado {
    int           programa   = 0;
    float         posX = 0, posY = 0, posZ = 0;
    int           velocidade = 0;
    std::string   status     = "desconhecido";
    unsigned long timestamp  = 0;
    bool          valido     = false;
};

/* ─── Estado global + mutex ──────────────────────────────── */
static std::recursive_mutex g_mutex;

static std::deque<Amostra>            g_buffer;        /* amostras recentes  */
static std::map<std::string, json>    g_ultimo_por_origem;  /* agregacao     */
static EstadoConsolidado              g_consolidado = {};
static EstadoConsolidado              g_ultimo_repassado = {};
/* So repassamos um estado a Central de operacao depois de conhecer um
   programa real; evita enviar programa=0, que ela trata como nao
   autorizado e dispararia uma emergencia falsa. */
static bool                           g_programa_conhecido = false;

static char            g_central_op_url[160] = {};     /* destino do repasse */
static int             g_vel_limiar          = 150;    /* velocidade "alta"  */
static unsigned long   g_ultimo_repasse      = 0;

/* Estatisticas */
static unsigned long g_recebidas   = 0;
static unsigned long g_repassadas  = 0;
static unsigned long g_filtradas   = 0;   /* recebidas mas nao importantes  */

/* Fila de repasse (1 pendente, processada em background) */
static std::atomic<bool> g_repasse_pendente{false};
static std::string       g_repasse_body;
static std::string       g_repasse_motivo;

/* ─── Log buffer ─────────────────────────────────────────── */
static char   g_log_buf[LOG_BUFFER_SIZE + 1] = {};
static size_t g_log_len                       = 0;

/* ─── Servidor HTTP ──────────────────────────────────────── */
static httplib::Server g_server;

/* ════════════════════════════════════════════════════════════
   LOG BUFFER
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

    std::printf("%s", line);
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

/* ════════════════════════════════════════════════════════════
   AGREGACAO + REGRA DE IMPORTANCIA
   ════════════════════════════════════════════════════════════ */

/* Atualiza o estado consolidado a partir dos campos presentes na amostra
   (programa/posicao/velocidade/status). Marca g_programa_conhecido quando a
   amostra traz um programa. NOTA: deve ser chamada com g_mutex adquirido. */
static void atualizar_consolidado(const json& dados) {
    if (dados.contains("programa")) {
        g_consolidado.programa = dados.value("programa", g_consolidado.programa);
        g_programa_conhecido   = true;
    }
    if (dados.contains("posicao")) {
        g_consolidado.posX = dados["posicao"].value("x", g_consolidado.posX);
        g_consolidado.posY = dados["posicao"].value("y", g_consolidado.posY);
        g_consolidado.posZ = dados["posicao"].value("z", g_consolidado.posZ);
    }
    if (dados.contains("velocidade"))
        g_consolidado.velocidade = dados.value("velocidade", g_consolidado.velocidade);
    if (dados.contains("status"))
        g_consolidado.status = dados.value("status", g_consolidado.status);
    g_consolidado.timestamp = millis();
    g_consolidado.valido    = true;
}

/* Decide se a amostra carrega informacao "importante" o suficiente
   para repasse IMEDIATO a Central de operacao.
   NOTA: deve ser chamada com g_mutex adquirido. */
static const char* avaliar_importancia(const std::string& tipo,
                                       const json& dados) {
    static char buf[80];

    /* Estimulos de seguranca sao sempre importantes */
    if (tipo == "seguranca") {
        std::snprintf(buf, sizeof(buf), "estimulo de seguranca");
        return buf;
    }
    /* Flag explicita de anomalia vinda do monitor */
    if (dados.value("anomalia", false)) {
        std::snprintf(buf, sizeof(buf), "anomalia sinalizada pelo monitor");
        return buf;
    }
    /* Status de falha */
    std::string st = dados.value("status", std::string(""));
    if (st == "erro" || st == "falha" || st == "panico") {
        std::snprintf(buf, sizeof(buf), "status de falha: %s", st.c_str());
        return buf;
    }
    /* Mudanca de programa */
    if (dados.contains("programa") &&
        dados.value("programa", g_consolidado.programa) != g_consolidado.programa) {
        std::snprintf(buf, sizeof(buf), "mudanca de programa");
        return buf;
    }
    /* Velocidade acima do limiar */
    if (dados.contains("velocidade") &&
        dados.value("velocidade", 0) > g_vel_limiar) {
        std::snprintf(buf, sizeof(buf), "velocidade alta: %d",
                      dados.value("velocidade", 0));
        return buf;
    }
    return nullptr;
}

/* Monta o corpo no formato esperado pela Central de operacao.
   NOTA: deve ser chamada com g_mutex adquirido. */
static json montar_estado_para_central() {
    json body;
    body["programa"]     = g_consolidado.programa;
    body["posicao"]["x"] = g_consolidado.posX;
    body["posicao"]["y"] = g_consolidado.posY;
    body["posicao"]["z"] = g_consolidado.posZ;
    body["velocidade"]   = g_consolidado.velocidade;
    body["status"]       = g_consolidado.status;
    body["timestamp"]    = g_consolidado.timestamp;
    body["origem"]       = "central_monitoramento";
    return body;
}

/* Agenda repasse do estado consolidado (processado em background).
   NOTA: deve ser chamada com g_mutex adquirido. */
static void agendar_repasse(const char* motivo) {
    g_repasse_body     = montar_estado_para_central().dump();
    g_repasse_motivo   = motivo;
    g_repasse_pendente = true;
    g_ultimo_repassado = g_consolidado;
    g_ultimo_repasse   = millis();
}

/* Houve mudanca relevante desde o ultimo repasse? (para heartbeat) */
static bool consolidado_mudou() {
    return g_consolidado.programa   != g_ultimo_repassado.programa ||
           g_consolidado.velocidade != g_ultimo_repassado.velocidade ||
           g_consolidado.status     != g_ultimo_repassado.status     ||
           g_consolidado.posX       != g_ultimo_repassado.posX       ||
           g_consolidado.posY       != g_ultimo_repassado.posY       ||
           g_consolidado.posZ       != g_ultimo_repassado.posZ;
}

/* ════════════════════════════════════════════════════════════
   REPASSE HTTP A CENTRAL DE OPERACAO  (background)
   ════════════════════════════════════════════════════════════ */
static void processar_repasse_pendente() {
    if (!g_repasse_pendente) return;

    std::string body, motivo, url;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (!g_repasse_pendente) return;
        g_repasse_pendente = false;
        body   = g_repasse_body;
        motivo = g_repasse_motivo;
        url    = g_central_op_url;
    }

    if (url.empty()) {
        std::printf("[REPASSE] (Central de operacao nao configurada) motivo=%s\n",
                    motivo.c_str());
        log_append("[REPASSE] %s (sem URL da central)", motivo.c_str());
        return;
    }

    std::string host, path;
    int port = 80;
    if (!parse_url(url.c_str(), host, port, path)) {
        log_append("[REPASSE] URL invalida: %s", url.c_str());
        return;
    }

    httplib::Client cli(host, port);
    cli.set_connection_timeout(HTTP_NOTIF_TIMEOUT, 0);
    cli.set_read_timeout(HTTP_NOTIF_TIMEOUT, 0);

    auto r = cli.Post(path, body, "application/json");
    {
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (r) {
            g_repassadas++;
            std::printf("[REPASSE] %s -> HTTP %d\n", motivo.c_str(), r->status);
            log_append("[REPASSE] %s -> HTTP %d", motivo.c_str(), r->status);
        } else {
            std::printf("[REPASSE] erro %s -> %s\n", motivo.c_str(),
                        httplib::to_string(r.error()).c_str());
            log_append("[REPASSE] erro: %s", motivo.c_str());
        }
    }
}

/* Heartbeat: repassa periodicamente se houve mudanca relevante. */
static void verificar_heartbeat() {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    if (!g_consolidado.valido || !g_programa_conhecido) return;
    if (millis() - g_ultimo_repasse < PERIODO_REPASSE_MS) return;
    if (!consolidado_mudou()) { g_ultimo_repasse = millis(); return; }
    agendar_repasse("heartbeat periodico");
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
        doc["servico"]         = "central_monitoramento";
        doc["camada"]          = "B - Plataforma";
        doc["uptime_s"]        = millis() / 1000;
        doc["central_op_url"]  = g_central_op_url[0] ? g_central_op_url
                                                     : "(nao configurada)";
        doc["buffer"]          = g_buffer.size();
        doc["stats"]["recebidas"]  = g_recebidas;
        doc["stats"]["repassadas"] = g_repassadas;
        doc["stats"]["filtradas"]  = g_filtradas;
        doc["rotas"] = json::array({
            "POST   /monitoramento          (monitores da camada C)",
            "GET    /agregado",
            "GET    /buffer",
            "GET    /config",
            "POST   /config/central-operacao",
            "POST   /config/limiares",
            "GET    /logs",
            "DELETE /logs"
        });
        enviar_json(res, 200, doc);
    });

    /* ─── POST /monitoramento ────────────────────────────── *
     * Interface principal com os monitores da camada inferior.
     * Body (formato Monitor Jetson/Wemos/P4):
     *   { "timestamp": "...", "device": "...",
     *     "component": "monitor_jetson|monitor_wemos|monitor_p4",
     *     "type":      "event|telemetry",
     *     "data":      { ... } }
     */
    g_server.Post("/monitoramento", [](const httplib::Request& req,
                                       httplib::Response& res) {
        std::printf("[HTTP] POST /monitoramento\n");
        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        Amostra a;
        a.origem      = body.value("component", std::string("desconhecida"));
        a.tipo        = body.value("type",      std::string("event"));
        a.dados       = body.contains("data") ? body["data"] : json::object();
        a.recebida_em = millis();

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        g_recebidas++;

        /* Bufferiza */
        if (g_buffer.size() >= MAX_BUFFER) g_buffer.pop_front();
        g_buffer.push_back(a);

        /* Agrega: ultima amostra por origem */
        g_ultimo_por_origem[a.origem] = a.dados;

        /* Regra de importancia avaliada ANTES de consolidar, para que
           comparacoes (ex.: mudanca de programa) usem o estado anterior */
        const char* motivo = avaliar_importancia(a.tipo, a.dados);

        /* Consolida a partir de QUALQUER amostra: assim o estado repassado
           reflete os campos que dispararam o repasse (ex.: velocidade alta
           vinda de uma amostra de 'sistema'), e nao valores defasados. */
        atualizar_consolidado(a.dados);

        bool repassado = false;
        if (motivo && g_programa_conhecido) {
            agendar_repasse(motivo);
            repassado = true;
            log_append("[MONIT] %s/%s -> IMPORTANTE (%s)",
                       a.origem.c_str(), a.tipo.c_str(), motivo);
        } else if (motivo) {
            /* Importante, mas ainda sem programa conhecido: nao repassa um
               estado com programa=0 (a Central de operacao o trataria como
               nao autorizado). Mantem bufferizado ate conhecer o programa. */
            log_append("[MONIT] %s/%s importante (%s) retido: programa desconhecido",
                       a.origem.c_str(), a.tipo.c_str(), motivo);
        } else {
            g_filtradas++;
            log_append("[MONIT] %s/%s bufferizado (nao repassado)",
                       a.origem.c_str(), a.tipo.c_str());
        }

        json resp;
        resp["aceito"]    = true;
        resp["origem"]    = a.origem;
        resp["repassado"] = repassado;
        if (motivo) {
            resp["motivo"] = motivo;
            if (!g_programa_conhecido) resp["retido"] = "programa desconhecido";
        }
        resp["buffer"]    = g_buffer.size();
        enviar_json(res, 200, resp);
    });

    /* ─── GET /agregado ──────────────────────────────────── */
    g_server.Get("/agregado", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /agregado\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        doc["consolidado"] = montar_estado_para_central();
        doc["valido"]      = g_consolidado.valido;
        doc["por_origem"]  = json::object();
        for (auto& kv : g_ultimo_por_origem)
            doc["por_origem"][kv.first] = kv.second;
        enviar_json(res, 200, doc);
    });

    /* ─── GET /buffer ────────────────────────────────────── */
    g_server.Get("/buffer", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /buffer\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        doc["tamanho"]  = g_buffer.size();
        doc["amostras"] = json::array();
        for (const auto& a : g_buffer) {
            json j;
            j["origem"]      = a.origem;
            j["tipo"]        = a.tipo;
            j["dados"]       = a.dados;
            j["recebida_em"] = a.recebida_em;
            doc["amostras"].push_back(j);
        }
        enviar_json(res, 200, doc);
    });

    /* ─── GET /config ────────────────────────────────────── */
    g_server.Get("/config", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /config\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        doc["central_op_url"]      = g_central_op_url;
        doc["velocidade_limiar"]   = g_vel_limiar;
        doc["periodo_repasse_ms"]  = PERIODO_REPASSE_MS;
        doc["buffer_max"]          = MAX_BUFFER;
        enviar_json(res, 200, doc);
    });

    /* ─── POST /config/central-operacao ──────────────────── *
     * Body: { "url": "http://host:8080/estado" }
     */
    g_server.Post("/config/central-operacao", [](const httplib::Request& req,
                                                 httplib::Response& res) {
        std::printf("[HTTP] POST /config/central-operacao\n");
        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }
        std::string url = body.value("url", std::string(""));

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        strlcpy(g_central_op_url, url.c_str(), sizeof(g_central_op_url));
        log_append("[CONFIG] central_op_url=%s", g_central_op_url);
        json resp;
        resp["aceito"]         = true;
        resp["central_op_url"] = g_central_op_url;
        enviar_json(res, 200, resp);
    });

    /* ─── POST /config/limiares ──────────────────────────── *
     * Body: { "velocidade_limiar": 150 }
     */
    g_server.Post("/config/limiares", [](const httplib::Request& req,
                                         httplib::Response& res) {
        std::printf("[HTTP] POST /config/limiares\n");
        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (body.contains("velocidade_limiar"))
            g_vel_limiar = body.value("velocidade_limiar", g_vel_limiar);
        log_append("[CONFIG] velocidade_limiar=%d", g_vel_limiar);
        json resp;
        resp["aceito"]            = true;
        resp["velocidade_limiar"] = g_vel_limiar;
        enviar_json(res, 200, resp);
    });

    /* ─── GET /logs ──────────────────────────────────────── */
    g_server.Get("/logs", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /logs\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        res.status = 200;
        res.set_content(g_log_buf, "text/plain; charset=utf-8");
        res.set_header("Content-Disposition",
                       "inline; filename=\"central_monitoramento_logs.txt\"");
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
    std::printf("=== CENTRAL DE MONITORAMENTO (Camada B - Plataforma) ===\n");
    log_append("Sistema iniciando");

    registrar_rotas();

    /* Thread de background:
     *   — heartbeat periodico de repasse
     *   — processamento da fila de repasse HTTP */
    std::thread bg([] {
        while (true) {
            verificar_heartbeat();
            processar_repasse_pendente();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    bg.detach();

    std::printf("[HTTP] Servidor REST iniciando em http://0.0.0.0:%d\n", HTTP_PORT);
    log_append("Servidor REST pronto na porta %d", HTTP_PORT);
    std::printf("========================================\n");

    if (!g_server.listen("0.0.0.0", HTTP_PORT)) {
        std::printf("[ERRO] Falha ao iniciar servidor na porta %d\n", HTTP_PORT);
        return 1;
    }
    return 0;
}
