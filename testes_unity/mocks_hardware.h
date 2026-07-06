#ifndef MOCKS_HARDWARE_H
#define MOCKS_HARDWARE_H

#include <string>
#include <vector>
#include <iostream>

// --- Simulação da classe String do Arduino ---
class String : public std::string
{
public:
    String() : std::string("") {}
    String(const char *s) : std::string(s ? s : "") {}
    String(const std::string &s) : std::string(s) {}

    bool isEmpty() const { return this->empty(); }
    const char *c_str() const { return this->std::string::c_str(); }

    // Concatenação de tipos variados
    String operator+(const char *s) const { return String(std::string(*this) + s); }
    String operator+(const String &s) const { return String(std::string(*this) + std::string(s)); }
};

// --- Mock de Tipos do Arduino/ESP32 ---
class IPAddress
{
public:
    String toString() { return String("192.168.1.150"); }
};

enum wl_status_t
{
    WL_CONNECTED,
    WL_DISCONNECTED
};

class WiFiClass
{
public:
    wl_status_t status() { return WL_CONNECTED; }
    void mode(int m) {}
    void begin(const char *s, const char *p, int ch = 6) {}
    IPAddress localIP() { return IPAddress(); }
};
extern WiFiClass WiFi;

// --- Mock do WebServer do ESP32 ---
class WebServer
{
public:
    WebServer(int port) {}
    String mock_payload = "";
    int last_status = 0;
    String last_content = "";

    String arg(String name) { return mock_payload; }
    void send(int code, String type, String content)
    {
        last_status = code;
        last_content = content;
    }
    void on(String r, int method, void (*h)()) {}
    void onNotFound(void (*h)()) {}
    void begin() {}
    void handleClient() {}
    String uri() { return String("/"); }
};
extern WebServer server;

// --- Mock do Serial ---
class SerialMock
{
public:
    void begin(unsigned long baud) {}
    void print(const char *s) {}
    void print(String s) {}
    void println(const char *s) {}
    void printf(const char *format, ...) {}
};
extern SerialMock Serial;

// --- Mock do LittleFS ---
class File
{
public:
    operator bool() const { return true; }
    void print(const String &s) {}
    void close() {}
    String readString() { return String("[]"); }
};

class LittleFSMock
{
public:
    bool begin(bool formatOnFail) { return true; }
    File open(const char *path, const char *mode) { return File(); }
};
extern LittleFSMock LittleFS;

// --- Mock da classe HTTPClient ---
class HTTPClient
{
public:
    void begin(String url) {}
    void addHeader(String name, String value) {}
    int POST(String payload) { return 200; }
    void end() {}
};

// --- Funções Globais do Arduino ---
inline unsigned long millis() { return 1000; }
inline void delay(int ms) {}

// Constantes e macros de compatibilidade
#define HTTP_GET 1
#define HTTP_POST 2
#define FILE_WRITE "w"
#define FILE_READ "r"
#define WIFI_STA 1

#endif