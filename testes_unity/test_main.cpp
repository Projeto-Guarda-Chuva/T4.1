#include "mocks_hardware.h"
#include "ArduinoJson.h"

// INSTANCIAÇÃO REAL DOS MOCKS (O arquivo original precisa deles e não os cria)
WiFiClass WiFi;
SerialMock Serial;
LittleFSMock LittleFS;

// APENAS O SERVER É EXTERN (Porque ele foi criado fisicamente no programador_de_atuacao.cpp)
extern WebServer server;

// Vincula as variáveis globais do seu componente real para os testes
extern float sensibilidadeGlobal;

// Importa o framework Unity com o wrapper C
extern "C"
{
#include "unity.h"
}

// Declaracao das funcoes do componente que vamos testar
void handleParametrizar();
void handleRoot();

// Configuracoes padrao antes de cada teste executado pelo Unity
void setUp(void)
{
    sensibilidadeGlobal = 0.5;
    server.mock_payload = "";
    server.last_status = 0;
    server.last_content = "";
}

void tearDown(void) {}

// --- CASOS DE TESTE COM UNITY ---

// Teste 1: Valida se o condicional aceita uma parametrizacao valida
void test_handle_parametrizar_sucesso(void)
{
    server.mock_payload = "{\"sensibilidade\":0.85}";

    handleParametrizar();

    TEST_ASSERT_EQUAL_FLOAT(0.85, sensibilidadeGlobal);
    TEST_ASSERT_EQUAL_INT(200, server.last_status);
}

// Teste 2: Valida se o condicional barra requisicoes com JSON quebrado (Erro 400)
void test_handle_parametrizar_json_invalido(void)
{
    server.mock_payload = "{\"sensibilidade\":";

    handleParametrizar();

    // A sensibilidade deve continuar intacta (0.5) e deve disparar Bad Request (400)
    TEST_ASSERT_EQUAL_FLOAT(0.5, sensibilidadeGlobal);
    TEST_ASSERT_EQUAL_INT(400, server.last_status);
}

// Teste 3: Valida a montagem do JSON de resposta da raiz
void test_handle_root_status_online(void)
{
    handleRoot();

    TEST_ASSERT_EQUAL_INT(200, server.last_status);

    // Verificacao simples se o retorno nao veio vazio
    TEST_ASSERT_TRUE(server.last_content.length() > 0);
}

// --- MAIN DO UNITY ---
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_handle_parametrizar_sucesso);
    RUN_TEST(test_handle_parametrizar_json_invalido);
    RUN_TEST(test_handle_root_status_online);

    return UNITY_END();
}