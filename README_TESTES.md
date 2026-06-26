# Instruções para Execução dos Testes de Integração (API)

Este documento explica como compilar os componentes da Camada B (Plataforma) e rodar a suíte de testes automatizados localmente antes do envio para a esteira de CI/CD do GitHub.

## 🛠️ 1. Pré-requisitos

Antes de iniciar, certifique-se de ter instalado na sua máquina:

* **Compilador C++:** Ferramentas de Build do Visual Studio (com a carga de trabalho "Desenvolvimento para desktop com C++").
* **CMake:** Versão 3.15 ou superior (garantir que está adicionado ao PATH do sistema).
* **Python:** Versão 3.10 ou superior.

Instale as bibliotecas de teste do Python executando o comando no terminal:

```bash
pip install pytest requests
```

## 📁 2. Estrutura de Arquivos de Teste

Os testes foram estruturados utilizando a abordagem de caixa-preta (testes de API/Componente):

* `CMakeLists.txt`: Gerencia a compilação de todos os executáveis C++ e o download automático das dependências (`nlohmann/json` e `cpp-httplib`).
* `test_api_atuador.py`: Valida as rotas de fila, programação e parada segura do Atuador Central (Porta 8090).
* `test_api_monitoramento.py`: Valida a bufferização e regras de importância da Central de Monitoramento (Porta 8091).
* `test_api_operacao.py`: Valida o controle de jornadas e a detecção de anomalias/emergências da Central de Operação (Porta 8080).
* `test_api_programador.py`: Valida a parametrização de sensibilidade e estímulos usando o simulador local do Programador de Atuação (Porta 8180).

## 🔨 3. Passo 1: Compilação do Projeto

Para baixar as dependências e compilar todos os quatro componentes C++ de uma vez, limpe a pasta `build` antiga (se houver) e execute:

```powershell
cmake -S . -B build ; cmake --build build
```

Os executáveis serão gerados dentro da pasta `build/Debug/`.

## 🚀 4. Passo 2: Execução dos Componentes e Testes

Os testes exigem que o servidor correspondente esteja rodando antes do disparo do script Python.

### Cenário A: Atuador Central (Porta 8090)

1. Em um terminal, inicie o servidor:

```powershell
.\build\Debug\atuador_central.exe
```

2. Em um novo terminal, rode o teste:

```powershell
python -m pytest test_api_atuador.py -v
```

### Cenário B: Central de Monitoramento (Porta 8091)

1. Certifique-se de que nenhum outro servidor está usando a porta e inicie o componente:

```powershell
.\build\Debug\central_monitoramento.exe
```

2. Em um novo terminal, rode o teste:

```powershell
python -m pytest test_api_monitoramento.py -v
```

> **Nota:** Este componente retém o estado em memória RAM. Caso queira rodar a suíte novamente do zero, lembre-se de reiniciar o executável C++.

### Cenário C: Central de Operação (Porta 8080)

1. Inicie o servidor:

```powershell
.\build\Debug\central_operacao.exe
```

2. Em um novo terminal, rode o teste:

```powershell
python -m pytest test_api_operacao.py -v
```

> **Nota:** O script de testes possui um tratamento automático para o comportamento defasado do buffer de `g_estado_anterior` do código original, enviando estados duplicados para garantir a consistência das validações de anomalia.

### Cenário D: Programador de Atuação Local (Porta 8180)

1. Certifique-se de que a pasta local chamada `ESP` existe na raiz do diretório para permitir a persistência de dados do Mock.
2. Inicie o servidor simulado:

```powershell
.\build\Debug\programador_local.exe
```

3. Em um novo terminal, rode o teste:

```powershell
python -m pytest test_api_programador.py -v
```

> **Nota:** Este teste utiliza fixtures automáticas do pytest que disparam requisições `DELETE` para limpar o estado do servidor entre cada caso de teste, eliminando a necessidade de reiniciar o executável manualmente.
