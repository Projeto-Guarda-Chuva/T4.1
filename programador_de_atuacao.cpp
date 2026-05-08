#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>

// Configurações Gerais
#define WIFI_SSID "Wokwi-GUEST" // Padrão para funcionar direto no simulador Wokwi
#define WIFI_PASSWORD ""
#define ARQUIVO_PROGRAMAS "/programas.json"
#define EXTERNAL_API_URL "http://seu-receptor-web.com/api"
#define ACTUATOR_URL "http://url-da-plataforma/atuador"

WebServer server(80);
float sensibilidadeGlobal = 0.5;
bool littleFsDisponivel = false;
String programasEmMemoria = "[]";

void handleRoot() {
  StaticJsonDocument<160> doc;
  doc["status"] = "online";
  doc["wifi"] = (WiFi.status() == WL_CONNECTED) ? "conectado" : "desconectado";
  doc["ip"] = WiFi.localIP().toString();

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleHealth() {
  StaticJsonDocument<160> doc;
  doc["status"] = "ok";
  doc["wifi"] = (WiFi.status() == WL_CONNECTED) ? "conectado" : "desconectado";
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Armazenamento Local (LittleFS)
void salvarArquivo(const char* path, const String& conteudo) {
  if (!littleFsDisponivel) {
    programasEmMemoria = conteudo;
    Serial.println("[Aviso] LittleFS indisponivel. Dados salvos apenas em memoria.");
    return;
  }

  File file = LittleFS.open(path, FILE_WRITE);
  if (file) {
    file.print(conteudo);
    file.close();
  } else {
    Serial.println("[Erro] Falha ao abrir arquivo para escrita.");
  }
}

String lerArquivo(const char* path) {
  if (!littleFsDisponivel) {
    return programasEmMemoria;
  }

  File file = LittleFS.open(path, FILE_READ);
  if (!file) return "[]"; // Retorna um array JSON vazio se não existir
  
  String conteudo = file.readString();
  file.close();
  return conteudo.isEmpty() ? "[]" : conteudo;
}

// Integração Externa
// Responsável por enviar os logs e dados para seu receptor web
void enviarDadosExternos(String endpoint, String payload) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(endpoint);
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST(payload);
    Serial.printf("[HTTP] POST %s | Resultado: %d\n", endpoint.c_str(), httpCode);
    
    http.end();
  } else {
    Serial.println("[Erro] WiFi desconectado. Falha ao enviar dados.");
  }
}

// Controladores (Handlers)
void handleParametrizar() {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"erro\":\"JSON invalido\"}");
    return;
  }

  if (doc.containsKey("sensibilidade")) {
    sensibilidadeGlobal = doc["sensibilidade"];
  }

  server.send(200, "application/json", "{\"mensagem\":\"Sensibilidade atualizada\"}");
}

void handleAdicionarPrograma() {
  DynamicJsonDocument docIn(512);
  if (deserializeJson(docIn, server.arg("plain"))) {
    server.send(400, "application/json", "{\"erro\":\"JSON invalido\"}");
    return;
  }

  // Carrega o banco local
  DynamicJsonDocument programas(4096);
  deserializeJson(programas, lerArquivo(ARQUIVO_PROGRAMAS));
  
  JsonArray arr = programas.is<JsonArray>() ? programas.as<JsonArray>() : programas.to<JsonArray>();
  
  // Adiciona o novo programa e salva
  arr.add(docIn);
  String saida;
  serializeJson(programas, saida);
  salvarArquivo(ARQUIVO_PROGRAMAS, saida);

  server.send(201, "application/json", "{\"mensagem\":\"Programa salvo localmente\"}");
}

void handleProcessarEstimulo() {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"erro\":\"JSON invalido\"}");
    return;
  }

  String acao_detectada = doc["acao_detectada"];
  float intensidade = doc["intensidade"];

  if (intensidade < sensibilidadeGlobal) {
    server.send(200, "application/json", "{\"mensagem\":\"Ignorado: Intensidade baixa\"}");
    return;
  }

  // Busca a configuração correspondente no banco local
  DynamicJsonDocument programas(4096);
  deserializeJson(programas, lerArquivo(ARQUIVO_PROGRAMAS));
  JsonArray arr = programas.as<JsonArray>();
  
  bool encontrado = false;
  JsonObject programaEscolhido;

  for (JsonObject p : arr) {
    if (p["acao"] == acao_detectada) {
      programaEscolhido = p;
      encontrado = true;
      break;
    }
  }

  if (!encontrado) {
    server.send(404, "application/json", "{\"erro\":\"Acao nao mapeada localmente\"}");
    return;
  }

  // 1. Monta e envia comando para o Atuador Externo
  StaticJsonDocument<256> atuadorDoc;
  atuadorDoc["comando"] = "executar_programa";
  atuadorDoc["detalhes"]["velocidade"] = programaEscolhido["velocidade"];
  atuadorDoc["detalhes"]["movimento"] = programaEscolhido["movimento"];
  
  String atuadorPayload;
  serializeJson(atuadorDoc, atuadorPayload);
  enviarDadosExternos(ACTUATOR_URL, atuadorPayload);

  // 2. Monta e envia Log para o Receptor Web
  StaticJsonDocument<256> logDoc;
  logDoc["acao_origem"] = acao_detectada;
  logDoc["objeto"] = programaEscolhido["objeto"];
  logDoc["status"] = "comando_disparado";
  
  String logPayload;
  serializeJson(logDoc, logPayload);
  enviarDadosExternos(String(EXTERNAL_API_URL) + "/logs", logPayload);

  server.send(200, "application/json", "{\"mensagem\":\"Estimulo processado e dados enviados\"}");
}

void handleNotFound() {
  Serial.printf("[HTTP] Rota nao encontrada: %s\n", server.uri().c_str());
  server.send(404, "application/json", "{\"erro\":\"rota nao encontrada\"}");
}

// Setup e Loop
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[Boot] Programador de atuacao iniciando...");
  
  // Inicializa o sistema de arquivos local
  littleFsDisponivel = LittleFS.begin(true);
  if (!littleFsDisponivel) {
    Serial.println("[Aviso] Falha ao montar o LittleFS. Seguindo com armazenamento em memoria.");
  }

  // Inicializa WiFi (no Wokwi)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
  Serial.print("Conectando ao WiFi");
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 40) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi conectado! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi nao conectado, seguindo em modo local.");
  }

  // Configuração das Rotas
  server.on("/", HTTP_GET, handleRoot);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/parametrizar", HTTP_POST, handleParametrizar);
  server.on("/adicionar-programa", HTTP_POST, handleAdicionarPrograma);
  server.on("/processar-estimulo", HTTP_POST, handleProcessarEstimulo);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("Servidor Web Online.");
}

void loop() {
  server.handleClient();

  static unsigned long ultimoLog = 0;
  if (millis() - ultimoLog >= 2000) {
    ultimoLog = millis();
    Serial.printf("[Status] uptime=%lus wifi=%s ip=%s\n",
                  millis() / 1000,
                  WiFi.status() == WL_CONNECTED ? "conectado" : "desconectado",
                  WiFi.localIP().toString().c_str());
  }
}
