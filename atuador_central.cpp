/**
 * ============================================================
 *  ATUADOR CENTRAL  —  Servidor REST (cpp-httplib)
 *  Camada B - Plataforma  |  Plataforma: Jetson Orin Nano (C++17)
 *
 *  Componente intermediario (Middleware) entre a camada de
 *  Aplicacao (A) e a camada de Biblioteca (C).
 *
 *  Responsabilidades (Especificacao - camada B, item 1):
 *    a. Oferecer uma interface centralizada para comando dos atuadores.
 *    b. Ser acionado pelo Programador de atuacao (programacao) ou pela
 *       Central de operacao (parada segura em caso de risco).
 *    c. Aceitar programacao completa (inicio ao fim) ou parcial.
 *    d. Receber novas programacoes a qualquer momento, com prioridade na
 *       fila: executar imediatamente, como proximo, ou apos a fila.
 *
 *  --- INTERFACE COM A CAMADA SUPERIOR (A - Aplicacao) ---
 *    POST /programar       <- Programador de atuacao (programacao + prioridade)
 *    POST /atuador         <- compatibilidade com payload atual do
 *                             Programador de atuacao ({comando, detalhes})
 *    POST /parada-segura   <- Central de operacao (interrompe e zera a fila)
 *    POST /retomar         <- Central de operacao (libera apos inspecao)
 *
 *  --- INTERFACE COM A CAMADA INFERIOR (C - Biblioteca) ---
 *    "Linguagem basica de comando" enviada por HTTP a cada atuador:
 *      { "atuador": "<nome>", "acao": "<acao>",
 *        "parametros": { ... }, "duracao_ms": <n> }
 *    Atuadores conhecidos: motor_movel, motor_fixo, iluminacao,
 *                          audio, grafico, ptz
 *
 *  HTTP: porta 8090  (ajuste HTTP_PORT)
 *
 *  Dependencias (header-only, via CMake FetchContent):
 *    cpp-httplib   — https://github.com/yhirose/cpp-httplib
 *    nlohmann/json — https://github.com/nlohmann/json
 *
 *  Build:
 *    cmake -S . -B build && cmake --build build
 *    ./build/atuador_central          (Linux/macOS)
 *    build\Debug\atuador_central.exe  (Windows)
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
#include <cstdarg>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <vector>
#include <map>

#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

/* ════════════════════════════════════════════════════════════
   SHIMS  (substitutos das APIs Arduino / ESP32)
   ════════════════════════════════════════════════════════════ */
static const auto g_boot = std::chrono::steady_clock::now();
static unsigned long millis() {
    return static_cast<unsigned long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_boot).count());
}

/* ─── Constantes ─────────────────────────────────────────── */
static constexpr int           HTTP_PORT          = 8090;
static constexpr int           MAX_FILA           = 32;
static constexpr int           HTTP_CMD_TIMEOUT   = 5;     /* segundos */
static constexpr int           LOG_BUFFER_SIZE    = 4096;
static constexpr unsigned long PASSO_ESPERA_MS    = 20;    /* granularidade do worker */
static constexpr int           PADRAO_VELOCIDADE  = 50;    /* velocidade/intensidade padrao p/ comando simples (bug1) */

/* ════════════════════════════════════════════════════════════
   ESTRUTURAS
   ════════════════════════════════════════════════════════════ */

/* Um movimento = um comando para um atuador da camada C */
struct Movimento {
    std::string   atuador;       /* motor_movel, motor_fixo, iluminacao, ... */
    std::string   acao;          /* mover, parar, cor, tocar, pan_tilt, ...   */
    json          parametros;    /* parametros livres da acao                 */
    unsigned long duracao_ms = 0;
};

/* Uma programacao = sequencia de movimentos (completa ou parcial) */
struct Programacao {
    std::string            id;
    std::string            origem;     
    bool                   completa = true;
    std::vector<Movimento> movimentos;
    unsigned long          recebida_em = 0;
};

/* ─── Estado global + mutex ──────────────────────────────── */
static std::recursive_mutex g_mutex;

static std::deque<Programacao> g_fila;               /* fila de execucao   */
static Programacao             g_atual;              /* programacao em uso */
static bool                    g_tem_atual = false;
static size_t                  g_mov_idx   = 0;      /* movimento corrente */

static std::atomic<bool> g_abort_atual{false};       /* descarta a atual   */
static std::atomic<bool> g_parada_segura{false};     /* bloqueio de risco  */
static char              g_parada_motivo[96] = {};
static unsigned long     g_parada_ts         = 0;

/* URLs dos atuadores da camada C (vazio = modo simulacao/log) */
static std::map<std::string, std::string> g_atuadores = {
    { "motor_movel", "" },
    { "motor_fixo",  "" },
    { "iluminacao",  "" },
    { "audio",       "" },
    { "grafico",     "" },
    { "ptz",         "" }
};

/* Estatisticas */
static unsigned long g_programas_executados = 0;
static unsigned long g_movimentos_enviados  = 0;

/* ─── Log buffer (txt temporario em RAM) ─────────────────── */
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

/* ════════════════════════════════════════════════════════════
   TRADUCAO PARA O PROTOCOLO JELLYFISH V3 (camada C)
   Converte um Movimento interno no corpo JSON { "id": N, ... }
   esperado pelas placas. A acao pode ser um nome (mapeado abaixo)
   ou ja um id numerico em parametros["id"] (pass-through).
   Retorna json nulo quando o atuador nao fala Jellyfish (ex.: ptz),
   caso em que se mantem o formato generico.
   ════════════════════════════════════════════════════════════ */
static json traduzir_jellyfish(const Movimento& mv) {
    /* Pass-through: id explicito no proprio movimento */
    if (mv.parametros.contains("id") && mv.parametros["id"].is_number_integer())
        return mv.parametros;

    std::string a = mv.acao;
    std::transform(a.begin(), a.end(), a.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    bool seguro = (a == "parada_seguranca" || a == "parar" || a == "stop");

    json p;
    if (mv.atuador == "grafico") {                    /* Familia 10 */
        if (a == "cor" || a == "cor_customizada" || mv.parametros.contains("r")) {
            p["id"] = 10;
            p["r"]  = mv.parametros.value("r", 0);
            p["g"]  = mv.parametros.value("g", 0);
            p["b"]  = mv.parametros.value("b", 0);
        } else if (a == "vermelho")               p["id"] = 11;
        else if (a == "verde")                    p["id"] = 12;
        else if (a == "azul")                     p["id"] = 13;
        else if (a == "preto" || a == "limpar" || seguro) p["id"] = 14;
        else if (a == "branco")                   p["id"] = 15;
        else                                      p["id"] = 14; /* fallback: limpar */
    } else if (mv.atuador == "motor_fixo") {          /* Familia 20 */
        if (a == "abrir")       p["id"] = 21;
        else if (a == "fechar") p["id"] = 22;
        else                    p["id"] = 20;         /* parar / seguranca */
    } else if (mv.atuador == "motor_movel") {         /* Familia 30 */
        if (a == "subir")       p["id"] = 31;
        else if (a == "descer") p["id"] = 32;
        else                    p["id"] = 30;         /* parar / seguranca */
    } else if (mv.atuador == "audio") {               /* Familia 40 */
        if (a == "reproduzir" || a == "tocar") {
            p["id"]   = 41;
            p["file"] = mv.parametros.value("file", std::string("alerta.mp3"));
        } else if (a == "volume" || a == "definir_volume") {
            p["id"]     = 42;
            p["volume"] = mv.parametros.value("volume", 50);
        } else {
            p["id"] = 40;                             /* parar / seguranca */
        }
    } else {
        return json();                                /* sem protocolo Jellyfish */
    }
    return p;
}

/* ════════════════════════════════════════════════════════════
   INTERFACE COM A CAMADA INFERIOR (C - Biblioteca)
   Envia o comando no protocolo Jellyfish V3; para atuadores fora do
   protocolo (ex.: ptz), usa o formato generico interno.
   ════════════════════════════════════════════════════════════ */
static void enviar_comando_atuador(const Movimento& mv) {
    json cmd = traduzir_jellyfish(mv);
    bool jellyfish = !cmd.is_null();
    if (!jellyfish) {
        cmd["atuador"]    = mv.atuador;
        cmd["acao"]       = mv.acao;
        cmd["parametros"] = mv.parametros;
        cmd["duracao_ms"] = mv.duracao_ms;
        cmd["origem"]     = "atuador_central";
    }

    std::string url;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        g_movimentos_enviados++;
        auto it = g_atuadores.find(mv.atuador);
        if (it != g_atuadores.end()) url = it->second;
    }

    if (url.empty()) {
        /* Modo simulacao: atuador da camada C ainda nao conectado */
        std::printf("[ATUADOR~sim] %s :: %s -> %s\n",
                    mv.atuador.c_str(), mv.acao.c_str(),
                    cmd.dump().c_str());
        log_append("[ATUADOR~sim] %s acao=%s jellyfish=%s",
                   mv.atuador.c_str(), mv.acao.c_str(), cmd.dump().c_str());
        return;
    }

    std::string host, path;
    int port = 80;
    if (!parse_url(url.c_str(), host, port, path)) {
        log_append("[ATUADOR] URL invalida p/ %s: %s",
                   mv.atuador.c_str(), url.c_str());
        return;
    }
    httplib::Client cli(host, port);
    cli.set_connection_timeout(HTTP_CMD_TIMEOUT, 0);
    cli.set_read_timeout(HTTP_CMD_TIMEOUT, 0);

    auto r = cli.Post(path, cmd.dump(), "application/json");
    if (r) {
        log_append("[ATUADOR] %s acao=%s -> HTTP %d",
                   mv.atuador.c_str(), mv.acao.c_str(), r->status);
    } else {
        log_append("[ATUADOR] erro ao comandar %s (%s)",
                   mv.atuador.c_str(),
                   httplib::to_string(r.error()).c_str());
    }
}

/* Comanda TODOS os atuadores conhecidos para um estado seguro. */
static void comandar_parada_atuadores(const char* motivo) {
    std::vector<std::string> nomes;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        for (auto& kv : g_atuadores) nomes.push_back(kv.first);
    }
    for (auto& nome : nomes) {
        Movimento mv;
        mv.atuador            = nome;
        mv.acao               = "parada_seguranca";
        mv.parametros["motivo"] = motivo;
        mv.duracao_ms         = 0;
        enviar_comando_atuador(mv);
    }
}

/* ════════════════════════════════════════════════════════════
   PARSE DE PROGRAMACAO
   ════════════════════════════════════════════════════════════ */
static Programacao parse_programacao(const json& body) {
    Programacao p;
    p.id          = body.value("id",
                               std::string("PRG_") + std::to_string(millis()));
    p.origem      = body.value("origem", std::string("desconhecida"));
    p.completa    = body.value("completa", true);
    p.recebida_em = millis();

    if (body.contains("movimentos") && body["movimentos"].is_array()) {
        for (const auto& m : body["movimentos"]) {
            Movimento mv;
            mv.atuador    = m.value("atuador", std::string("motor_movel"));
            mv.acao       = m.value("acao",    std::string("mover"));
            mv.parametros = m.contains("parametros") ? m["parametros"]
                                                     : json::object();
            mv.duracao_ms = m.value("duracao_ms", (unsigned long)1000);
            p.movimentos.push_back(mv);
        }
    }
    return p;
}

/* Insere a programacao na fila respeitando a prioridade.
   prioridade: "imediata" | "proxima" | "fila" (default).
   NOTA: deve ser chamada com g_mutex adquirido. */
static void enfileirar(const Programacao& p, const std::string& prioridade) {
    if (g_fila.size() >= MAX_FILA) {
        g_fila.pop_back();
        log_append("[FILA] cheia: programacao mais antiga descartada");
    }

    if (prioridade == "imediata") {
        /* Interrompe a atual e a coloca na frente da fila */
        g_fila.push_front(p);
        g_abort_atual = true;
        log_append("[FILA] '%s' INSERIDA c/ prioridade IMEDIATA (interrompe atual)",
                   p.id.c_str());
    } else if (prioridade == "proxima") {
        g_fila.push_front(p);
        log_append("[FILA] '%s' inserida como PROXIMA", p.id.c_str());
    } else {
        g_fila.push_back(p);
        log_append("[FILA] '%s' inserida ao FIM da fila (pos=%zu)",
                   p.id.c_str(), g_fila.size());
    }
}

/* ════════════════════════════════════════════════════════════
   WORKER  (executa a fila — equivalente ao loop() do Arduino)
   ════════════════════════════════════════════════════════════ */
static void worker_execucao() {
    while (true) {
        if (g_parada_segura) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        /* 1. Garante uma programacao corrente */
        Programacao atual;
        size_t      idx = 0;
        bool        tem = false;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            if (!g_tem_atual && !g_fila.empty()) {
                g_atual       = g_fila.front();
                g_fila.pop_front();
                g_tem_atual   = true;
                g_mov_idx     = 0;
                g_abort_atual = false;
                std::printf("[EXEC] iniciando programacao '%s' (%zu movimentos)\n",
                            g_atual.id.c_str(), g_atual.movimentos.size());
                log_append("[EXEC] iniciando '%s' (%zu mov, %s)",
                           g_atual.id.c_str(), g_atual.movimentos.size(),
                           g_atual.completa ? "completa" : "parcial");
            }
            tem   = g_tem_atual;
            atual = g_atual;
            idx   = g_mov_idx;
        }

        if (!tem) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        /* 2. Programacao concluida? */
        if (idx >= atual.movimentos.size()) {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            g_tem_atual = false;
            g_programas_executados++;
            std::printf("[EXEC] programacao '%s' concluida\n", atual.id.c_str());
            log_append("[EXEC] '%s' concluida", atual.id.c_str());
            continue;
        }

        /* 3. Reverifica parada/abort imediatamente antes do despacho: a
         *    /parada-segura (ou uma programacao imediata) pode ter chegado
         *    entre a leitura sob lock e este ponto, e nao se deve comandar
         *    atuador apos uma parada de seguranca. */
        if (g_parada_segura) continue;   /* handler ja zerou g_tem_atual */
        if (g_abort_atual) {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            g_tem_atual   = false;
            g_abort_atual = false;
            log_append("[EXEC] '%s' interrompida antes do despacho (programacao imediata)",
                       atual.id.c_str());
            continue;
        }

        /* Executa o movimento corrente (HTTP — fora do lock) */
        const Movimento& mv = atual.movimentos[idx];
        enviar_comando_atuador(mv);

        /* 4. Aguarda a duracao do movimento, de forma interrompivel */
        unsigned long fim = millis() + mv.duracao_ms;
        while (millis() < fim) {
            if (g_abort_atual || g_parada_segura) break;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(PASSO_ESPERA_MS));
        }

        /* 5. Interrupcao por prioridade imediata -> descarta a atual */
        if (g_abort_atual) {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            g_tem_atual   = false;
            g_abort_atual = false;
            log_append("[EXEC] '%s' interrompida (nova programacao imediata)",
                       atual.id.c_str());
            continue;
        }
        if (g_parada_segura) continue;

        /* 6. Avanca para o proximo movimento */
        {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            if (g_tem_atual && g_atual.id == atual.id)
                g_mov_idx++;
        }
    }
}

/* ════════════════════════════════════════════════════════════
   ROTAS
   ════════════════════════════════════════════════════════════ */
static json snapshot_estado() {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    json doc;
    doc["servico"]       = "atuador_central";
    doc["uptime_s"]      = millis() / 1000;
    doc["parada_segura"] = g_parada_segura.load();
    if (g_parada_segura) {
        doc["motivo_parada"] = g_parada_motivo;
        doc["parada_desde"]  = g_parada_ts;
    }
    doc["executando"]    = g_tem_atual;
    if (g_tem_atual) {
        doc["atual"]["id"]            = g_atual.id;
        doc["atual"]["origem"]        = g_atual.origem;
        doc["atual"]["completa"]      = g_atual.completa;
        doc["atual"]["movimento_idx"] = g_mov_idx;
        doc["atual"]["total_mov"]     = g_atual.movimentos.size();
    }
    doc["fila"] = g_fila.size();
    doc["stats"]["programas_executados"] = g_programas_executados;
    doc["stats"]["movimentos_enviados"]  = g_movimentos_enviados;
    return doc;
}

static void registrar_rotas() {

    /* ─── GET / ──────────────────────────────────────────── */
    g_server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /\n");
        json doc = snapshot_estado();
        doc["camada"] = "B - Plataforma";
        doc["rotas"]  = json::array({
            "GET    /estado",
            "GET    /fila",
            "POST   /programar        (Programador de atuacao)",
            "POST   /atuador          (compat. payload atual do Programador)",
            "POST   /parada-segura    (Central de operacao)",
            "POST   /retomar          (Central de operacao)",
            "GET    /config/atuadores",
            "POST   /config/atuadores",
            "GET    /logs",
            "DELETE /logs"
        });
        enviar_json(res, 200, doc);
    });

    /* ─── GET /estado ────────────────────────────────────── */
    g_server.Get("/estado", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /estado\n");
        enviar_json(res, 200, snapshot_estado());
    });

    /* ─── GET /fila ──────────────────────────────────────── */
    g_server.Get("/fila", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /fila\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        doc["tamanho"] = g_fila.size();
        doc["itens"]   = json::array();
        for (const auto& p : g_fila) {
            json j;
            j["id"]            = p.id;
            j["origem"]        = p.origem;
            j["completa"]      = p.completa;
            j["num_movimentos"]= p.movimentos.size();
            j["recebida_em"]   = p.recebida_em;
            doc["itens"].push_back(j);
        }
        enviar_json(res, 200, doc);
    });

    /* ─── POST /programar ────────────────────────────────── *
     * Interface principal com o Programador de atuacao.
     * Body (completo):
     *   { "id": "...", "origem": "programador_de_atuacao",
     *     "completa": true|false, "prioridade": "imediata|proxima|fila",
     *     "movimentos": [ { "atuador":"motor_movel", "acao":"mover",
     *                       "parametros": {...}, "duracao_ms": 1500 }, ... ] }
     *
     * Body (comando simples — item 1c: "movimentos"/"detalhes" e OPCIONAL):
     *   { "programa": 1, "prioridade": "fim" }
     * -> aceito, assumindo os parametros de velocidade/intensidade padrao.
     */
    g_server.Post("/programar", [](const httplib::Request& req,
                                   httplib::Response& res) {
        std::printf("[HTTP] POST /programar\n");
        if (g_parada_segura) {
            enviar_erro(res, 503, "atuador em parada de seguranca"); return;
        }
        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        Programacao p = parse_programacao(body);

        /* ─── Correcao Bug 1 (especificacao, item 1c) ─────────────────────
         * O campo de movimentos/detalhes e OPCIONAL. Comandos simples como
         * {"programa": N, "prioridade": "..."} devem ser ACEITOS (201),
         * assumindo os parametros de velocidade/intensidade PADRAO da
         * escultura. Quando nao vier um array "movimentos", sintetiza-se
         * um movimento padrao em vez de rejeitar com 400.
         * O payload completo com "movimentos" continua funcionando igual. */
        if (p.movimentos.empty()) {
            if (body.contains("programa")) {
                std::string prog_ref = body["programa"].is_string()
                        ? body["programa"].get<std::string>()
                        : std::to_string(body.value("programa", 0));
                p.id = std::string("PRG_") + prog_ref;
            }

            Movimento mv;
            mv.atuador                  = body.value("atuador", std::string("motor_movel"));
            mv.acao                     = body.value("acao",    std::string("mover"));
            mv.parametros["velocidade"] = PADRAO_VELOCIDADE;   /* intensidade padrao */
            if (body.contains("programa"))
                mv.parametros["programa"] = body["programa"];
            mv.duracao_ms               = body.value("duracao_ms", (unsigned long)1000);
            p.movimentos.push_back(mv);

            log_append("[PROGRAMAR] comando simples aceito (movimento padrao) id=%s vel=%d",
                       p.id.c_str(), PADRAO_VELOCIDADE);
        }

        std::string prioridade = body.value("prioridade", std::string("fila"));

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        enfileirar(p, prioridade);

        json resp;
        resp["aceito"]      = true;
        resp["id"]          = p.id;
        resp["prioridade"]  = prioridade;
        resp["completa"]    = p.completa;
        resp["movimentos"]  = p.movimentos.size();
        resp["fila"]        = g_fila.size();
        enviar_json(res, 201, resp);
    });

    /* ─── POST /atuador ──────────────────────────────────── *
     * Compatibilidade com o payload ja enviado pelo Programador
     * de atuacao atual:
     *   { "comando": "executar_programa",
     *     "detalhes": { "velocidade": N, "movimento": "..." } }
     * E traduzido para uma programacao de um unico movimento.
     */
    g_server.Post("/atuador", [](const httplib::Request& req,
                                 httplib::Response& res) {
        std::printf("[HTTP] POST /atuador (compat)\n");
        if (g_parada_segura) {
            enviar_erro(res, 503, "atuador em parada de seguranca"); return;
        }
        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        std::string comando = body.value("comando", std::string("executar_programa"));
        json detalhes = body.contains("detalhes") ? body["detalhes"]
                                                   : json::object();

        Programacao p;
        p.id          = std::string("PRG_compat_") + std::to_string(millis());
        p.origem      = "programador_de_atuacao";
        p.completa    = true;
        p.recebida_em = millis();

        Movimento mv;
        mv.atuador            = "motor_movel";
        mv.acao               = detalhes.value("movimento", std::string("mover"));
        mv.parametros["velocidade"] = detalhes.value("velocidade", 0);
        mv.parametros["comando"]    = comando;
        mv.duracao_ms         = detalhes.value("duracao_ms", (unsigned long)1000);
        p.movimentos.push_back(mv);

        std::string prioridade = body.value("prioridade", std::string("fila"));

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        enfileirar(p, prioridade);

        json resp;
        resp["aceito"]     = true;
        resp["id"]         = p.id;
        resp["traduzido"]  = "1 movimento";
        resp["prioridade"] = prioridade;
        enviar_json(res, 201, resp);
    });

    /* ─── POST /parada-segura ────────────────────────────── *
     * Acionado pela Central de operacao em caso de risco.
     * Interrompe a execucao, zera a fila e comanda os atuadores
     * para estado seguro.
     */
    g_server.Post("/parada-segura", [](const httplib::Request& req,
                                       httplib::Response& res) {
        std::printf("[HTTP] POST /parada-segura\n");
        json body;
        try { body = json::parse(req.body); } catch (...) { body = {}; }
        std::string motivo = body.value("motivo", std::string("nao_informado"));

        {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            g_parada_segura = true;
            g_abort_atual   = true;
            strlcpy(g_parada_motivo, motivo.c_str(), sizeof(g_parada_motivo));
            g_parada_ts     = millis();
            size_t descartadas = g_fila.size();
            g_fila.clear();
            g_tem_atual = false;
            std::printf("[!!!] PARADA SEGURA: %s (%zu prog. descartadas)\n",
                        motivo.c_str(), descartadas);
            log_append("[PARADA-SEGURA] %s (%zu descartadas)",
                       motivo.c_str(), descartadas);
        }
        comandar_parada_atuadores(motivo.c_str());

        json resp;
        resp["parada_segura"] = true;
        resp["motivo"]        = motivo;
        resp["desde"]         = g_parada_ts;
        enviar_json(res, 200, resp);
    });

    /* ─── POST /retomar ──────────────────────────────────── */
    g_server.Post("/retomar", [](const httplib::Request&,
                                 httplib::Response& res) {
        std::printf("[HTTP] POST /retomar\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        g_parada_segura   = false;
        g_abort_atual     = false;
        g_parada_motivo[0]= '\0';
        g_parada_ts       = 0;
        log_append("[OK] parada segura liberada");
        json resp;
        resp["parada_segura"] = false;
        resp["aceito"]        = true;
        enviar_json(res, 200, resp);
    });

    /* ─── GET /config/atuadores ──────────────────────────── */
    g_server.Get("/config/atuadores", [](const httplib::Request&,
                                         httplib::Response& res) {
        std::printf("[HTTP] GET /config/atuadores\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        json doc;
        for (auto& kv : g_atuadores)
            doc["atuadores"][kv.first] = kv.second.empty() ? "(simulacao)"
                                                           : kv.second;
        enviar_json(res, 200, doc);
    });

    /* ─── POST /config/atuadores ─────────────────────────── *
     * Body: { "motor_movel": "http://host:porta/cmd", ... }
     * Define a URL HTTP de cada atuador da camada C.
     */
    g_server.Post("/config/atuadores", [](const httplib::Request& req,
                                          httplib::Response& res) {
        std::printf("[HTTP] POST /config/atuadores\n");
        json body;
        try { body = json::parse(req.body); }
        catch (...) { enviar_erro(res, 400, "JSON invalido"); return; }

        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        int atualizados = 0;
        for (auto it = body.begin(); it != body.end(); ++it) {
            if (!it.value().is_string()) continue;
            g_atuadores[it.key()] = it.value().get<std::string>();
            atualizados++;
            log_append("[CONFIG] atuador %s -> %s",
                       it.key().c_str(),
                       it.value().get<std::string>().c_str());
        }
        json resp;
        resp["aceito"]      = true;
        resp["atualizados"] = atualizados;
        enviar_json(res, 200, resp);
    });

    /* ─── GET /logs ──────────────────────────────────────── */
    g_server.Get("/logs", [](const httplib::Request&, httplib::Response& res) {
        std::printf("[HTTP] GET /logs\n");
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        res.status = 200;
        res.set_content(g_log_buf, "text/plain; charset=utf-8");
        res.set_header("Content-Disposition",
                       "inline; filename=\"atuador_central_logs.txt\"");
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
    std::printf("=== ATUADOR CENTRAL (Camada B - Plataforma) ===\n");
    log_append("Sistema iniciando");

    /* Bootstrap das URLs dos atuadores da camada C (T2.2) via variaveis
       de ambiente (docker-compose). Sem valor, o atuador opera em modo
       simulacao. Podem ser sobrescritas em runtime por POST /config/atuadores. */
    struct { const char* atuador; const char* env; } mapa_env[] = {
        { "motor_movel", "ATUADOR_MOTOR_MOVEL_URL" },
        { "motor_fixo",  "ATUADOR_MOTOR_FIXO_URL"  },
        { "iluminacao",  "ATUADOR_ILUMINACAO_URL"  },
        { "audio",       "ATUADOR_AUDIO_URL"       },
        { "grafico",     "ATUADOR_GRAFICO_URL"     },
        { "ptz",         "ATUADOR_PTZ_URL"         },
    };
    for (auto& m : mapa_env) {
        if (const char* url = std::getenv(m.env)) {
            g_atuadores[m.atuador] = url;
            std::printf("[CONFIG] atuador %s -> %s\n", m.atuador, url);
            log_append("[CONFIG] atuador %s -> %s", m.atuador, url);
        }
    }

    registrar_rotas();

    /* Thread que consome a fila de programacao (equiv. ao loop()) */
    std::thread worker(worker_execucao);
    worker.detach();

    std::printf("[HTTP] Servidor REST iniciando em http://0.0.0.0:%d\n", HTTP_PORT);
    log_append("Servidor REST pronto na porta %d", HTTP_PORT);
    std::printf("========================================\n");

    if (!g_server.listen("0.0.0.0", HTTP_PORT)) {
        std::printf("[ERRO] Falha ao iniciar servidor na porta %d\n", HTTP_PORT);
        return 1;
    }
    return 0;
}