#include <algorithm>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <curl/curl.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")
using SocketType = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using SocketType = int;
const int INVALID_SOCKET = -1;
const int SOCKET_ERROR = -1;
#endif

const int DEFAULT_PORT = 8190;
const long MAX_VIDEO_BYTES = 200L * 1024L * 1024L;

bool running = true;
std::string destinoExterno;

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct HttpResponse {
  int status;
  std::string body;
};

struct ForwardResult {
  bool ok;
  long status;
  std::string body;
  std::string erro;
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

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string trim(const std::string& value) {
  size_t start = value.find_first_not_of(" \r\n\t");
  if (start == std::string::npos) return "";
  size_t end = value.find_last_not_of(" \r\n\t");
  return value.substr(start, end - start + 1);
}

std::string jsonEscape(const std::string& value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u00";
          const char* hex = "0123456789abcdef";
          out << hex[(c >> 4) & 0x0F] << hex[c & 0x0F];
        } else {
          out << c;
        }
        break;
    }
  }
  return out.str();
}

std::string getHeader(const HttpRequest& request, const std::string& name) {
  auto it = request.headers.find(toLower(name));
  return it == request.headers.end() ? "" : it->second;
}

bool parseLong(const std::string& value, long& out) {
  try {
    size_t consumed = 0;
    out = std::stol(value, &consumed);
    return consumed == value.size();
  } catch (...) {
    return false;
  }
}

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

ForwardResult encaminharVideo(const HttpRequest& request) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return {false, 0, "", "Falha ao iniciar libcurl"};
  }

  std::string resposta;
  std::string contentType = getHeader(request, "Content-Type");
  if (contentType.empty()) {
    contentType = "application/octet-stream";
  }

  struct curl_slist* headers = nullptr;
  std::string contentTypeHeader = "Content-Type: " + contentType;
  headers = curl_slist_append(headers, contentTypeHeader.c_str());
  headers = curl_slist_append(headers, "Expect:");

  curl_easy_setopt(curl, CURLOPT_URL, destinoExterno.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resposta);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "VivaSeuVideoLocal/1.0");

  CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    return {false, status, resposta, curl_easy_strerror(code)};
  }
  return {status >= 200 && status < 300, status, resposta, ""};
}

HttpResponse handleRequest(const HttpRequest& request) {
  if (request.method == "GET" && (request.path == "/" || request.path == "/health")) {
    return {200, "{\"status\":\"online\",\"servico\":\"viva_seu_video\",\"rota_video\":\"/viva-seu-video/video\"}"};
  }

  if (request.method != "POST" || (request.path != "/viva-seu-video/video" && request.path != "/viva-seu-video")) {
    return {404, "{\"erro\":\"rota nao encontrada\"}"};
  }

  if (destinoExterno.empty()) {
    return {500, "{\"erro\":\"URL externa nao configurada\"}"};
  }

  std::string contentLength = getHeader(request, "Content-Length");
  long contentLengthValue = 0;
  if (!contentLength.empty() && parseLong(contentLength, contentLengthValue) && contentLengthValue > MAX_VIDEO_BYTES) {
    return {413, "{\"erro\":\"Video excede o limite de 200MB\"}"};
  }

  if (request.body.empty()) {
    return {400, "{\"erro\":\"Envie o video no corpo da requisicao\"}"};
  }

  if (request.body.size() > static_cast<size_t>(MAX_VIDEO_BYTES)) {
    return {413, "{\"erro\":\"Video excede o limite de 200MB\"}"};
  }

  ForwardResult result = encaminharVideo(request);
  std::ostringstream body;
  body << "{"
       << "\"mensagem\":\"Video recebido\","
       << "\"encaminhado\":" << (result.ok ? "true" : "false") << ","
       << "\"status_externo\":" << result.status;
  if (!result.erro.empty()) {
    body << ",\"erro_externo\":\"" << jsonEscape(result.erro) << "\"";
  }
  if (!result.body.empty()) {
    body << ",\"resposta_externa\":\"" << jsonEscape(result.body.substr(0, 2000)) << "\"";
  }
  body << "}";

  return {result.ok ? 200 : 502, body.str()};
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

  size_t headersEnd = raw.find("\r\n\r\n");
  if (headersEnd == std::string::npos) return request;

  std::istringstream headerLines(raw.substr(lineEnd + 2, headersEnd - lineEnd - 2));
  std::string headerLine;
  while (std::getline(headerLines, headerLine)) {
    size_t colon = headerLine.find(':');
    if (colon == std::string::npos) continue;
    std::string name = toLower(trim(headerLine.substr(0, colon)));
    std::string value = trim(headerLine.substr(colon + 1));
    request.headers[name] = value;
  }

  request.body = raw.substr(headersEnd + 4);
  return request;
}

std::string readRequest(SocketType client) {
  std::string raw;
  char buffer[8192];
  int received = recv(client, buffer, sizeof(buffer), 0);
  if (received <= 0) return raw;
  raw.append(buffer, received);

  size_t headerEnd = raw.find("\r\n\r\n");
  while (headerEnd == std::string::npos) {
    received = recv(client, buffer, sizeof(buffer), 0);
    if (received <= 0) return raw;
    raw.append(buffer, received);
    headerEnd = raw.find("\r\n\r\n");
  }

  std::regex contentLengthPattern("Content-Length:\\s*(\\d+)", std::regex_constants::icase);
  std::smatch match;
  long contentLength = 0;
  std::string headers = raw.substr(0, headerEnd);
  if (std::regex_search(headers, match, contentLengthPattern)) {
    parseLong(match[1].str(), contentLength);
  }
  if (contentLength > MAX_VIDEO_BYTES) return raw;

  size_t expectedSize = headerEnd + 4 + static_cast<size_t>(contentLength);
  while (raw.size() < expectedSize) {
    received = recv(client, buffer, sizeof(buffer), 0);
    if (received <= 0) break;
    raw.append(buffer, received);
  }

  return raw;
}

std::string reasonPhrase(int status) {
  if (status == 200) return "OK";
  if (status == 400) return "Bad Request";
  if (status == 404) return "Not Found";
  if (status == 413) return "Payload Too Large";
  if (status == 500) return "Internal Server Error";
  if (status == 502) return "Bad Gateway";
  return "OK";
}

void sendResponse(SocketType client, const HttpResponse& response) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << " " << reasonPhrase(response.status) << "\r\n";
  out << "Content-Type: application/json; charset=utf-8\r\n";
  out << "Content-Length: " << response.body.size() << "\r\n";
  out << "Connection: close\r\n\r\n";
  out << response.body;

  std::string data = out.str();
  send(client, data.c_str(), static_cast<int>(data.size()), 0);
}

int main(int argc, char* argv[]) {
  signal(SIGINT, onSignal);

  int port = DEFAULT_PORT;
  if (argc >= 2) {
    destinoExterno = argv[1];
  } else if (const char* envUrl = std::getenv("VIVA_SEU_VIDEO_DESTINO")) {
    destinoExterno = envUrl;
  }
  if (argc >= 3) {
    port = std::stoi(argv[2]);
  }

  if (!initSockets()) {
    std::cerr << "[Erro] Falha ao iniciar sockets\n";
    return 1;
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    std::cerr << "[Erro] Falha ao iniciar libcurl\n";
    cleanupSockets();
    return 1;
  }

  SocketType serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (serverSocket == INVALID_SOCKET) {
    std::cerr << "[Erro] Falha ao criar socket\n";
    curl_global_cleanup();
    cleanupSockets();
    return 1;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  if (bind(serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
    std::cerr << "[Erro] Nao foi possivel usar a porta " << port << "\n";
    closeSocket(serverSocket);
    curl_global_cleanup();
    cleanupSockets();
    return 1;
  }

  if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
    std::cerr << "[Erro] Falha ao iniciar escuta HTTP\n";
    closeSocket(serverSocket);
    curl_global_cleanup();
    cleanupSockets();
    return 1;
  }

  std::cout << "[Boot] Viva Seu Video online em http://localhost:" << port << "\n";
  std::cout << "[Rota] POST /viva-seu-video/video\n";
  std::cout << "[Destino] " << (destinoExterno.empty() ? "nao configurado" : destinoExterno) << "\n";

  while (running) {
    SocketType client = accept(serverSocket, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;

    std::string raw = readRequest(client);
    if (!raw.empty()) {
      HttpRequest request = parseRequest(raw);
      std::cout << "[HTTP] " << request.method << " " << request.path
                << " (" << request.body.size() << " bytes)\n";
      sendResponse(client, handleRequest(request));
    }

    closeSocket(client);
  }

  closeSocket(serverSocket);
  curl_global_cleanup();
  cleanupSockets();
  return 0;
}
