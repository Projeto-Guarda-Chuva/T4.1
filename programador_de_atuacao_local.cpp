#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")
using SocketType = SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using SocketType = int;
const int INVALID_SOCKET = -1;
const int SOCKET_ERROR = -1;
#endif

using Clock = std::chrono::steady_clock;

// Banco local de configuracao (especificacao camada A, item 1.b:
// "Armazena as programacoes antigas e atuais no banco de dados de
// configuracao"). A pasta e criada automaticamente na inicializacao.
const std::string DATA_DIR  = "dados";
const std::string DATA_FILE = "dados/programador_de_atuacao.dados.json";
const int PORT = 8180;

bool running = true;
std::string parametrizacao = "null";
double sensibilidadeGlobal = 0.5;
bool sensibilidadeConfigurada = false;
std::vector<std::string> programas;
std::vector<std::string> eventos;
Clock::time_point iniciadoEm = Clock::now();

// Destino do comando de atuacao (Atuador central, camada B - Plataforma).
// Spec camada A, item 1.a / 4: "repassando a programacao ao componente
// Atuador na camada de Plataforma". Configuravel via env ATUADOR_CENTRAL_URL
// (default = servico docker "atuador-central"). Endpoint /programar do
// Atuador central espera { movimentos:[...], prioridade }.
std::string atuadorCentralUrl = "http://atuador-central:8090/programar";

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct HttpResponse {
  int status;
  std::string body;
};

void onSignal(int) {
  running = false;
}

bool initSockets() {
#ifdef _WIN32
  WSADATA wsaData;
  return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
  return true;
#endif
}

void cleanupSockets() {
#ifdef _WIN32
  WSACleanup();
#endif
}

void closeSocket(SocketType socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

std::string trim(const std::string& value) {
  size_t start = value.find_first_not_of(" \r\n\t");
  if (start == std::string::npos) return "";
  size_t end = value.find_last_not_of(" \r\n\t");
  return value.substr(start, end - start + 1);
}

std::string jsonEscape(const std::string& value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << c; break;
    }
  }
  return out.str();
}

bool looksLikeJson(const std::string& body) {
  std::string clean = trim(body);
  if (clean.size() < 2) return false;
  char first = clean.front();
  char last = clean.back();
  return (first == '{' && last == '}') || (first == '[' && last == ']');
}

bool extractString(const std::string& json, const std::string& key, std::string& out) {
  std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) return false;
  out = match[1].str();
  return true;
}

bool extractNumber(const std::string& json, const std::string& key, double& out) {
  std::regex pattern("\"" + key + "\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) return false;
  out = std::stod(match[1].str());
  return true;
}

std::string nowText() {
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::tm localTime{};
#ifdef _WIN32
  localtime_s(&localTime, &time);
#else
  localtime_r(&time, &localTime);
#endif
  std::ostringstream out;
  out << std::put_time(&localTime, "%Y-%m-%dT%H:%M:%S");
  return out.str();
}

std::string jsonArray(const std::vector<std::string>& items) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out << ",";
    out << items[i];
  }
  out << "]";
  return out.str();
}

std::vector<std::string> extractObjectArray(const std::string& json, const std::string& key) {
  std::vector<std::string> items;
  size_t keyPos = json.find("\"" + key + "\"");
  if (keyPos == std::string::npos) return items;

  size_t arrayStart = json.find('[', keyPos);
  if (arrayStart == std::string::npos) return items;

  int arrayDepth = 0;
  int objectDepth = 0;
  bool inString = false;
  bool escaped = false;
  size_t objectStart = std::string::npos;

  for (size_t i = arrayStart; i < json.size(); ++i) {
    char c = json[i];

    if (escaped) {
      escaped = false;
      continue;
    }

    if (c == '\\' && inString) {
      escaped = true;
      continue;
    }

    if (c == '"') {
      inString = !inString;
      continue;
    }

    if (inString) continue;

    if (c == '[') {
      arrayDepth++;
      continue;
    }

    if (c == ']') {
      arrayDepth--;
      if (arrayDepth == 0) break;
      continue;
    }

    if (arrayDepth != 1) continue;

    if (c == '{') {
      if (objectDepth == 0) objectStart = i;
      objectDepth++;
      continue;
    }

    if (c == '}') {
      objectDepth--;
      if (objectDepth == 0 && objectStart != std::string::npos) {
        items.push_back(json.substr(objectStart, i - objectStart + 1));
        objectStart = std::string::npos;
      }
    }
  }

  return items;
}

std::string extractJsonValue(const std::string& json, const std::string& key) {
  size_t keyPos = json.find("\"" + key + "\"");
  if (keyPos == std::string::npos) return "";

  size_t colon = json.find(':', keyPos);
  if (colon == std::string::npos) return "";

  size_t valueStart = json.find_first_not_of(" \r\n\t", colon + 1);
  if (valueStart == std::string::npos) return "";

  char first = json[valueStart];
  if (first == 'n' && json.compare(valueStart, 4, "null") == 0) return "null";

  char closing = '\0';
  if (first == '{') closing = '}';
  if (first == '[') closing = ']';
  if (closing == '\0') return "";

  int depth = 0;
  bool inString = false;
  bool escaped = false;

  for (size_t i = valueStart; i < json.size(); ++i) {
    char c = json[i];

    if (escaped) {
      escaped = false;
      continue;
    }

    if (c == '\\' && inString) {
      escaped = true;
      continue;
    }

    if (c == '"') {
      inString = !inString;
      continue;
    }

    if (inString) continue;

    if (c == first) {
      depth++;
      continue;
    }

    if (c == closing) {
      depth--;
      if (depth == 0) return json.substr(valueStart, i - valueStart + 1);
    }
  }

  return "";
}

void registrarEvento(const std::string& tipo, const std::string& payload) {
  std::ostringstream evento;
  evento << "{\"tipo\":\"" << jsonEscape(tipo)
         << "\",\"timestamp\":\"" << jsonEscape(nowText())
         << "\",\"payload\":" << (looksLikeJson(payload) ? payload : "null") << "}";
  eventos.push_back(evento.str());
}

void loadData() {
  std::ifstream file(DATA_FILE);
  if (!file) return;

  std::ostringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  std::string savedParametrizacao = extractJsonValue(content, "parametrizacao");
  parametrizacao = savedParametrizacao.empty() ? "null" : savedParametrizacao;

  double sensibilidade = 0;
  if (looksLikeJson(parametrizacao) && extractNumber(parametrizacao, "sensibilidade", sensibilidade)) {
    sensibilidadeGlobal = sensibilidade;
    sensibilidadeConfigurada = true;
  } else {
    sensibilidadeConfigurada = false;
  }

  programas = extractObjectArray(content, "configuracoes");
  eventos = extractObjectArray(content, "eventos");
}

void saveData() {
  std::ofstream file(DATA_FILE, std::ios::trunc);
  file << "{\n";
  file << "  \"parametrizacao\": " << parametrizacao << ",\n";
  file << "  \"configuracoes\": " << jsonArray(programas) << ",\n";
  file << "  \"eventos\": " << jsonArray(eventos) << "\n";
  file << "}\n";
}

std::string statusJson() {
  auto uptime = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - iniciadoEm).count();
  std::ostringstream out;
  out << "{"
      << "\"status\":\"online\","
      << "\"servico\":\"programador_de_atuacao\","
      << "\"uptime_segundos\":" << uptime << ","
      << "\"arquivo_dados\":\"" << jsonEscape(DATA_FILE) << "\""
      << "}";
  return out.str();
}

std::string parametrizacaoJson() {
  return parametrizacao;
}

std::string findProgramaByAcao(const std::string& acaoDetectada) {
  for (const std::string& programa : programas) {
    std::string acao;
    if (extractString(programa, "acao", acao) && acao == acaoDetectada) {
      return programa;
    }
  }
  return "";
}

// Cliente HTTP minimo (POST JSON). Retorna o status HTTP, ou -1 em falha de
// conexao. Multiplataforma: winsock no Windows, POSIX no Linux/Docker.
int httpPostJson(const std::string& host, int port, const std::string& path,
                 const std::string& body, std::string* respOut = nullptr) {
  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  std::string portStr = std::to_string(port);
  if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
    return -1;
  }

  SocketType sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock == INVALID_SOCKET) {
    freeaddrinfo(res);
    return -1;
  }

  if (connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
    closeSocket(sock);
    freeaddrinfo(res);
    return -1;
  }
  freeaddrinfo(res);

  std::ostringstream req;
  req << "POST " << path << " HTTP/1.1\r\n"
      << "Host: " << host << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  std::string data = req.str();
  if (send(sock, data.c_str(), static_cast<int>(data.size()), 0) == SOCKET_ERROR) {
    closeSocket(sock);
    return -1;
  }

  std::string raw;
  char buffer[2048];
  int received;
  while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
    raw.append(buffer, received);
    if (raw.size() > 65536) break;
  }
  closeSocket(sock);

  int status = -1;
  size_t sp = raw.find(' ');
  if (sp != std::string::npos) status = std::atoi(raw.c_str() + sp + 1);
  if (respOut) {
    size_t bodyStart = raw.find("\r\n\r\n");
    *respOut = (bodyStart != std::string::npos) ? raw.substr(bodyStart + 4) : "";
  }
  return status;
}

bool parseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
  std::string s = url;
  size_t scheme = s.find("://");
  if (scheme != std::string::npos) s = s.substr(scheme + 3);
  size_t slash = s.find('/');
  std::string hostPort;
  if (slash == std::string::npos) { hostPort = s; path = "/"; }
  else { hostPort = s.substr(0, slash); path = s.substr(slash); }
  size_t colon = hostPort.rfind(':');
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

// Traduz o programa encontrado numa programacao no formato do Atuador central
// e a envia via POST /programar. Retorna um resumo JSON do disparo para
// registro no evento e na resposta ao chamador (spec camada A, item 4:
// "Encaminhar comandos de execucao para o atuador").
std::string dispararProgramacao(const std::string& programa, const std::string& acaoDetectada) {
  // Usa "movimentos" do programa se existir; senao deriva um unico movimento
  // a partir de atuador / movimento / velocidade.
  std::string movimentos = extractJsonValue(programa, "movimentos");
  if (movimentos.empty() || movimentos == "null") {
    std::string atuador = "motor_movel";
    extractString(programa, "atuador", atuador);
    std::string movimento = "mover";
    extractString(programa, "movimento", movimento);
    double velocidade = 0;
    bool temVel = extractNumber(programa, "velocidade", velocidade);
    double duracao = 1000;
    extractNumber(programa, "duracao_ms", duracao);

    std::ostringstream mv;
    mv << "[{\"atuador\":\"" << jsonEscape(atuador)
       << "\",\"acao\":\"" << jsonEscape(movimento)
       << "\",\"parametros\":{";
    if (temVel) mv << "\"velocidade\":" << velocidade;
    mv << "},\"duracao_ms\":" << static_cast<long>(duracao) << "}]";
    movimentos = mv.str();
  }

  std::string prioridade = "fila";
  extractString(programa, "prioridade", prioridade);

  std::ostringstream payload;
  payload << "{\"id\":\"PRG_" << jsonEscape(acaoDetectada) << "\""
          << ",\"origem\":\"programador_de_atuacao\""
          << ",\"completa\":true"
          << ",\"prioridade\":\"" << jsonEscape(prioridade) << "\""
          << ",\"movimentos\":" << movimentos << "}";

  std::string host, path;
  int port = 8090;
  if (!parseUrl(atuadorCentralUrl, host, port, path)) {
    std::cout << "[HTTP->Atuador] URL invalida: " << atuadorCentralUrl << "\n";
    return "{\"enviado\":false,\"motivo\":\"url_invalida\"}";
  }

  std::string resposta;
  int status = httpPostJson(host, port, path, payload.str(), &resposta);
  std::cout << "[HTTP->Atuador] POST " << atuadorCentralUrl << " -> " << status << "\n";

  std::ostringstream resumo;
  resumo << "{\"enviado\":" << ((status >= 200 && status < 300) ? "true" : "false")
         << ",\"status\":" << status
         << ",\"url\":\"" << jsonEscape(atuadorCentralUrl) << "\""
         << ",\"programacao\":" << payload.str() << "}";
  return resumo.str();
}

// Nucleo do processamento de estimulo ({"acao_detectada", "intensidade"}).
// Usado por POST /processar-estimulo (payload ja no formato de estimulo) e
// por POST /gesture_update (descoberta do Processador de imagem traduzida).
HttpResponse processarEstimulo(const std::string& body) {
  std::string acaoDetectada;
  double intensidade = 0;

  if (!extractString(body, "acao_detectada", acaoDetectada)) {
    registrarEvento("estimulo_invalido", body);
    saveData();
    return {400, "{\"erro\":\"Campo acao_detectada obrigatorio\"}"};
  }

  if (sensibilidadeConfigurada && extractNumber(body, "intensidade", intensidade) && intensidade < sensibilidadeGlobal) {
    registrarEvento("estimulo_ignorado", body);
    saveData();
    return {200, "{\"mensagem\":\"Estimulo ignorado\",\"motivo\":\"intensidade abaixo da sensibilidade\"}"};
  }

  std::string programa = findProgramaByAcao(acaoDetectada);
  if (programa.empty()) {
    registrarEvento("estimulo_sem_configuracao", body);
    saveData();
    return {404, "{\"erro\":\"Acao nao mapeada localmente\",\"estimulo\":" + body + "}"};
  }

  // Encaminha a programacao ao Atuador central (camada B).
  std::string disparo = dispararProgramacao(programa, acaoDetectada);

  std::string eventoPayload = "{\"estimulo\":" + body + ",\"programa\":" + programa + ",\"disparo\":" + disparo + "}";
  registrarEvento("estimulo_processado", eventoPayload);
  saveData();
  return {200, "{\"mensagem\":\"Estimulo processado\",\"programa\":" + programa + ",\"disparo\":" + disparo + "}"};
}

HttpResponse handleRequest(const HttpRequest& request) {
  if (request.method == "GET" && (request.path == "/" || request.path == "/health")) {
    return {200, statusJson()};
  }

  if (request.method == "GET" && (request.path == "/parametrizar" || request.path == "/parametrizacao")) {
    return {200, parametrizacaoJson()};
  }

  if (request.method == "GET" && (request.path == "/programas" || request.path == "/configuracoes" || request.path == "/configs")) {
    return {200, jsonArray(programas)};
  }

  if (request.method == "GET" && (request.path == "/eventos" || request.path == "/logs")) {
    return {200, jsonArray(eventos)};
  }

  if (request.method == "POST") {
    if (!looksLikeJson(request.body)) {
      return {400, "{\"erro\":\"JSON invalido\"}"};
    }

    if (request.path == "/parametrizar" || request.path == "/parametrizacao") {
      double sensibilidade = 0;
      parametrizacao = request.body;
      if (extractNumber(request.body, "sensibilidade", sensibilidade)) {
        sensibilidadeGlobal = sensibilidade;
        sensibilidadeConfigurada = true;
      } else {
        sensibilidadeConfigurada = false;
      }
      registrarEvento("parametrizacao_recebida", request.body);
      saveData();
      return {200, "{\"mensagem\":\"Parametrizacao salva\",\"parametrizacao\":" + parametrizacaoJson() + "}"};
    }

    if (request.path == "/adicionar-programa" || request.path == "/programas" || request.path == "/configuracoes" || request.path == "/configs") {
      programas.push_back(request.body);
      registrarEvento("configuracao_recebida", request.body);
      saveData();
      return {201, "{\"mensagem\":\"Configuracao salva\",\"total\":" + std::to_string(programas.size()) + "}"};
    }

    if (request.path == "/processar-estimulo") {
      return processarEstimulo(request.body);
    }

    // Descobertas do Processador de imagem (camada B), no formato publicado
    // pelo pipeline de visao (YOLO na webcam) — objeto plano:
    //   { "gesture": "UP", "confidence": 0.33, "speed": 1.0, "count": 1 }
    // A descoberta e traduzida para o formato de estimulo
    // (gesture -> acao_detectada, confidence -> intensidade) e segue o
    // mesmo fluxo de processamento do /processar-estimulo.
    if (request.path == "/gesture_update") {
      std::string gesto;
      if (!extractString(request.body, "gesture", gesto)) {
        registrarEvento("gesto_invalido", request.body);
        saveData();
        return {400, "{\"erro\":\"Campo gesture obrigatorio\"}"};
      }

      // Gesto de repouso (padrao do detector) = ausencia de descoberta
      if (gesto == "REST") {
        registrarEvento("gesto_repouso_ignorado", request.body);
        saveData();
        return {200, "{\"mensagem\":\"Gesto de repouso ignorado\"}"};
      }

      double confianca = 0;
      bool temConfianca = extractNumber(request.body, "confidence", confianca);

      std::ostringstream estimulo;
      estimulo << "{\"acao_detectada\":\"" << jsonEscape(gesto) << "\"";
      if (temConfianca) estimulo << ",\"intensidade\":" << confianca;
      estimulo << ",\"origem\":\"processador_imagem\"}";
      return processarEstimulo(estimulo.str());
    }

    // Resultado do reconhecimento de audio, enviado pelo Processador de
    // audio (camada B):
    //   { "type": "command", "payload": "abrir" }
    // Comandos conhecidos: abrir, foto, descer, parar. O comando e
    // traduzido para o formato de estimulo (payload -> acao_detectada) e
    // segue o mesmo fluxo de processamento do /processar-estimulo.
    if (request.path == "/audioUpdate") {
      std::string tipo;
      if (!extractString(request.body, "type", tipo)) tipo = "command";

      std::string comando;
      if (!extractString(request.body, "payload", comando) || comando.empty()) {
        registrarEvento("audio_invalido", request.body);
        saveData();
        return {400, "{\"erro\":\"Campo payload obrigatorio\"}"};
      }

      // Tipos futuros (ex.: telemetria) sao aceitos sem virar estimulo
      if (tipo != "command") {
        registrarEvento("audio_tipo_nao_suportado", request.body);
        saveData();
        return {200, "{\"mensagem\":\"Aceito\",\"motivo\":\"type nao suportado, nada a processar\"}"};
      }

      std::ostringstream estimulo;
      estimulo << "{\"acao_detectada\":\"" << jsonEscape(comando)
               << "\",\"origem\":\"processador_audio\"}";
      return processarEstimulo(estimulo.str());
    }

    if (request.path == "/eventos" || request.path == "/logs") {
      registrarEvento("evento_recebido", request.body);
      saveData();
      return {201, "{\"mensagem\":\"Evento salvo\"}"};
    }
  }

  if (request.method == "DELETE" && (request.path == "/parametrizar" || request.path == "/parametrizacao")) {
    parametrizacao = "null";
    sensibilidadeConfigurada = false;
    registrarEvento("parametrizacao_removida", "null");
    saveData();
    return {200, "{\"mensagem\":\"Parametrizacao removida\"}"};
  }

  if (request.method == "DELETE" && (request.path == "/programas" || request.path == "/configuracoes" || request.path == "/configs")) {
    programas.clear();
    registrarEvento("configuracoes_removidas", "null");
    saveData();
    return {200, "{\"mensagem\":\"Configuracoes removidas\"}"};
  }

  if (request.method == "DELETE" && (request.path == "/eventos" || request.path == "/logs")) {
    eventos.clear();
    saveData();
    return {200, "{\"mensagem\":\"Eventos removidos\"}"};
  }

  return {404, "{\"erro\":\"rota nao encontrada\"}"};
}

HttpRequest parseRequest(const std::string& raw) {
  HttpRequest request;
  size_t lineEnd = raw.find("\r\n");
  std::string firstLine = raw.substr(0, lineEnd);
  std::istringstream line(firstLine);
  line >> request.method >> request.path;

  size_t queryStart = request.path.find('?');
  if (queryStart != std::string::npos) {
    request.path = request.path.substr(0, queryStart);
  }
  if (request.path.size() > 1 && request.path.back() == '/') {
    request.path.pop_back();
  }

  size_t bodyStart = raw.find("\r\n\r\n");
  if (bodyStart != std::string::npos) {
    request.body = raw.substr(bodyStart + 4);
  }

  return request;
}

std::string readRequest(SocketType client) {
  std::string raw;
  char buffer[4096];
  int received = recv(client, buffer, sizeof(buffer), 0);
  if (received <= 0) return raw;
  raw.append(buffer, received);

  size_t headerEnd = raw.find("\r\n\r\n");
  if (headerEnd == std::string::npos) return raw;

  std::regex contentLengthPattern("Content-Length:\\s*(\\d+)", std::regex_constants::icase);
  std::smatch match;
  int contentLength = 0;
  std::string headers = raw.substr(0, headerEnd);
  if (std::regex_search(headers, match, contentLengthPattern)) {
    contentLength = std::stoi(match[1].str());
  }

  size_t expectedSize = headerEnd + 4 + static_cast<size_t>(contentLength);
  while (raw.size() < expectedSize) {
    received = recv(client, buffer, sizeof(buffer), 0);
    if (received <= 0) break;
    raw.append(buffer, received);
  }

  return raw;
}

void sendResponse(SocketType client, const HttpResponse& response) {
  std::string reason = "OK";
  if (response.status == 201) reason = "Created";
  if (response.status == 400) reason = "Bad Request";
  if (response.status == 404) reason = "Not Found";

  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << " " << reason << "\r\n";
  out << "Content-Type: application/json; charset=utf-8\r\n";
  out << "Content-Length: " << response.body.size() << "\r\n";
  out << "Connection: close\r\n\r\n";
  out << response.body;

  std::string data = out.str();
  send(client, data.c_str(), static_cast<int>(data.size()), 0);
}

int main() {
  signal(SIGINT, onSignal);

  // Destino do Atuador central via env (docker-compose); default = servico
  // "atuador-central". Permite trocar o alvo sem recompilar.
  if (const char* e = std::getenv("ATUADOR_CENTRAL_URL")) {
    atuadorCentralUrl = e;
  }
  std::cout << "[Config] Atuador central em " << atuadorCentralUrl << "\n";

  if (!initSockets()) {
    std::cerr << "[Erro] Falha ao iniciar sockets\n";
    return 1;
  }

  SocketType serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (serverSocket == INVALID_SOCKET) {
    std::cerr << "[Erro] Falha ao criar socket\n";
    cleanupSockets();
    return 1;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(PORT);

  if (bind(serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
    std::cerr << "[Erro] Nao foi possivel usar a porta " << PORT << "\n";
    closeSocket(serverSocket);
    cleanupSockets();
    return 1;
  }

  if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
    std::cerr << "[Erro] Falha ao iniciar escuta HTTP\n";
    closeSocket(serverSocket);
    cleanupSockets();
    return 1;
  }

  // Garante o diretorio do banco de configuracao; sem ele a persistencia
  // falharia em silencio (ofstream nao cria diretorios) e os programas
  // cadastrados se perderiam a cada reinicio.
  std::error_code dirErr;
  std::filesystem::create_directories(DATA_DIR, dirErr);
  if (dirErr) {
    std::cerr << "[Aviso] Nao foi possivel criar '" << DATA_DIR
              << "' (" << dirErr.message() << "); operando so em memoria\n";
  }

  loadData();
  saveData();
  std::cout << "[Boot] Servidor C++ online em http://localhost:" << PORT << "\n";
  std::cout << "[Dados] " << DATA_FILE << "\n";
  std::cout << "[Ngrok] Exponha esta porta com: ngrok http " << PORT << "\n";

  while (running) {
    SocketType client = accept(serverSocket, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;

    std::string raw = readRequest(client);
    if (!raw.empty()) {
      HttpRequest request = parseRequest(raw);
      std::cout << "[HTTP] " << request.method << " " << request.path << "\n";
      sendResponse(client, handleRequest(request));
    }

    closeSocket(client);
  }

  closeSocket(serverSocket);
  cleanupSockets();
  return 0;
}
