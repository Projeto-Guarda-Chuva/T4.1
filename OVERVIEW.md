# Componente Programador de Atuação

Este documento descreve o componente **Programador de Atuação** da Camada A (Aplicação), com foco em responsabilidades, funcionalidades e fluxo de operação.

## Objetivo

O Programador de Atuação recebe informações processadas por outros componentes da aplicação e transforma essas entradas em programações de movimento para o atuador da camada de plataforma.

## Responsabilidades

1. Programar movimentos com base nas informações recebidas.
2. Armazenar programações atuais e históricas no banco local de configuração.
3. Receber parâmetros de parametrização da aplicação.
4. Encaminhar comandos de execução para o atuador.
5. Registrar eventos operacionais para rastreabilidade.

## Funcionalidades implementadas

1. API HTTP para configuração e operação.
2. Parametrização de sensibilidade global.
3. Cadastro de programas de atuação.
4. Processamento de estímulos com regra de limiar.
5. Busca de programa por ação detectada.
6. Disparo de comando para o **Atuador Central** (camada B) quando o estímulo casa com um programa: monta uma programação no formato `{ id, origem, completa, prioridade, movimentos:[{atuador, acao, parametros, duracao_ms}] }` e a envia via `POST /programar`. O destino é configurável pela variável de ambiente `ATUADOR_CENTRAL_URL` (default `http://atuador-central:8090/programar`); o resultado do disparo (status HTTP + programação enviada) é devolvido na resposta e registrado no evento `estimulo_processado`.
7. Registro de logs para receptor externo.
8. Persistência local dos programas, parametrização e eventos no banco de configuração em JSON (`dados/programador_de_atuacao.dados.json`), com criação automática do diretório na inicialização e fallback em memória.
9. Recebimento das descobertas do Processador de Imagem (camada B) via `POST /gesture_update`, no formato de objeto plano `{"gesture": "UP", "confidence": 0.33, "speed": 1.0, "count": 1}`, com tradução interna para estímulo (`gesture` → `acao_detectada`, `confidence` → `intensidade`) e processamento pelo mesmo fluxo de limiar e busca de programa. Gesto de repouso (`REST`) é aceito sem gerar estímulo; ausência do campo `gesture` retorna erro.
10. Recebimento do resultado do reconhecimento de áudio do Processador de Áudio (camada B) via `POST /audioUpdate`, no formato `{"type": "command", "payload": "abrir|foto|descer|parar"}`, com tradução interna para estímulo (`payload` → `acao_detectada`) e processamento pelo mesmo fluxo de busca de programa. Tipos diferentes de `command` são aceitos sem gerar estímulo.

## Fluxo de funcionamento

1. Inicialização de rede, armazenamento local e serviço HTTP.
2. Recebimento de parâmetros operacionais (ex.: sensibilidade).
3. Registro e atualização de programas no banco local.
4. Recebimento de estímulo com ação detectada e intensidade.
5. Validação da intensidade pelo limiar configurado.
6. Consulta do programa associado à ação.
7. Envio da programação ao Atuador Central (camada B) via `POST /programar` quando houver mapeamento válido.
8. Envio de log do processamento para rastreabilidade.

## Endpoints da aplicação

1. `GET /health`.
2. `POST /parametrizar`.
3. `POST /adicionar-programa`.
4. `POST /processar-estimulo`.
5. `POST /gesture_update` — recebe do Processador de Imagem o resultado do reconhecimento de gestos e o traduz em estímulo.
6. `POST /audioUpdate` — recebe do Processador de Áudio o resultado do reconhecimento de áudio (`{"type": "command", "payload": "<comando>"}`) e o traduz em estímulo.

---

# Componente Central de Operação

Este documento descreve o componente **Central de Operação** da Camada A (Aplicação), com foco em responsabilidades, funcionalidades e fluxo de operação.

## Objetivo

A Central de Operação atua como camada de supervisão e segurança da operação da escultura, recebendo o estado atual em tempo real, controlando as jornadas de movimentação, detectando anomalias no funcionamento e acionando a parada segura quando necessário.

## Responsabilidades

1. Receber o estado atual da escultura para acompanhamento da execução.
2. Manter o controle das jornadas de movimentação realizadas, armazenando os logs recentes.
3. Verificar continuamente se existe alguma anomalia no funcionamento, comportamento ou estado recebido.
4. Realizar a parada segura do sistema quando uma anomalia for detectada.
5. Acionar o registrador externo ao final de cada jornada para salvar informações relevantes.
6. Atuar como componente de controle operacional, garantindo estabilidade e segurança.

## Funcionalidades implementadas

1. API HTTP REST assíncrona executando sobre ESP32.
2. Recebimento e armazenamento do estado da escultura (US06).
3. Gestão de jornadas com registro de eventos individuais por amostra recebida (US07).
4. Detecção contínua de anomalias por velocidade, posição, programa autorizado, status reportado e variação brusca de posição.
5. Parada segura automática em caso de anomalia, com bloqueio de novas operações e comando de **parada segura ao Atuador Central** (camada B) via `POST /parada-segura` (em thread destacada, para não bloquear o fluxo de emergência).
6. Disparo manual de emergência por endpoint dedicado.
7. Reset de emergência após inspeção do sistema.
8. Cinco estados predefinidos (`inicial`, `operacional`, `alta_velocidade`, `manutencao`, `demonstracao`) e gerador de estado aleatório.
9. Notificação externa do registrador via webhook HTTP no encerramento de jornadas e em paradas de emergência.
10. Buffer de logs temporário em memória RAM, exposto como txt via endpoint dedicado.
11. Configuração em tempo de execução da URL do registrador externo (`POST /config/registrador`) e da URL do Atuador Central para parada segura (`POST /config/atuador`). Ambas também são inicializadas por variáveis de ambiente na subida: `REGISTRADOR_URL` e `ATUADOR_CENTRAL_URL`.
12. Parametrização interna de limites operacionais (velocidade máxima, zona segura de posição, programas válidos e delta de posição).
13. Recebimento do resultado do reconhecimento de gestos enviado pelo Processador de Imagem (camada B) via HTTP POST, no formato de objeto plano `{"gesture": "UP", "confidence": 0.33, "speed": 1.0, "count": 1}`, com armazenamento do último gesto para consulta.
14. Recebimento do resultado do reconhecimento de áudio enviado pelo Processador de Áudio (camada B) via HTTP POST, no formato `{"type": "command", "payload": "abrir|foto|descer|parar"}`, com armazenamento do último comando para consulta.

## Fluxo de funcionamento

1. Inicialização da Serial, conexão WiFi e subida do servidor HTTP assíncrono.
2. Registro de rotas REST e início do servidor.
3. Recebimento de requisições da camada inferior ou de clientes externos.
4. Validação do estado recebido contra os limites de anomalia configurados.
5. Atualização do estado interno e registro de evento na jornada aberta, quando houver.
6. Disparo de parada segura e bloqueio de operações ao detectar anomalia.
7. Notificação assíncrona ao registrador externo no encerramento de jornadas e em emergências.
8. Verificação periódica de timeout do estado, marcando `sem_sinal` quando ultrapassado.
9. Persistência dos eventos relevantes no buffer de logs temporário.

## Endpoints da aplicação

1. `GET /` — informações do servidor e listagem das rotas disponíveis.
2. `GET /estado` — leitura do estado atual da escultura.
3. `POST /estado` — atualização do estado da escultura.
4. `POST /gesture_update` — recebe do Processador de Imagem o resultado do reconhecimento de gestos (`{"gesture": "UP", "confidence": 0.33, "speed": 1.0, "count": 1}`).
5. `GET /gesto` — leitura do último gesto recebido do Processador de Imagem.
6. `POST /audioUpdate` — recebe do Processador de Áudio o resultado do reconhecimento de áudio (`{"type": "command", "payload": "<comando>"}`).
7. `GET /audio` — leitura do último comando de áudio recebido do Processador de Áudio.
8. `GET /estado/presets` — listagem dos cinco estados predefinidos.
9. `POST /estado/preset?id=N` — aplicação do preset identificado por N (1 a 5).
10. `POST /estado/aleatorio` — aplicação de estado aleatório dentro dos limites válidos.
11. `GET /jornadas` — listagem resumida das jornadas em memória.
12. `GET /jornadas/detalhe?idx=N` — detalhamento de uma jornada específica com seus eventos.
13. `POST /jornadas/inicio` — abertura de uma nova jornada de movimentação.
14. `POST /jornadas/fim` — encerramento da jornada aberta.
15. `POST /emergencia` — acionamento manual da parada de segurança.
16. `POST /emergencia/reset` — reativação do sistema após emergência.
17. `GET /config` — leitura dos limites operacionais e das URLs do registrador e do Atuador Central.
18. `POST /config/registrador` — atualização da URL de notificação externa.
19. `POST /config/atuador` — atualização da URL do Atuador Central acionado na parada segura.
20. `GET /logs` — leitura do buffer de logs em texto puro.
21. `DELETE /logs` — limpeza do buffer de logs.

---

# Componente Central de Monitoramento

Este documento descreve o componente **Central de Monitoramento** da Camada B (Plataforma), com foco em responsabilidades, funcionalidades e fluxo de operação.

## Objetivo

A Central de Monitoramento atua como middleware entre os monitores da camada de Biblioteca (C) e a Central de Operação da camada de Aplicação (A). Ela recebe amostras de monitoramento de forma síncrona, bufferiza, agrega e repassa à camada superior apenas as informações consideradas importantes, evitando sobrecarga com dados irrelevantes.

## Responsabilidades

1. Receber de forma síncrona amostras de monitoramento dos monitores da camada inferior (Monitor Jetson, Monitor Wemos, Monitor P4).
2. Bufferizar as amostras recentes e agregar o último valor reportado por cada origem.
3. Avaliar a importância de cada amostra recebida com base em regras predefinidas.
4. Repassar à Central de Operação apenas as informações consideradas importantes.
5. Manter e consolidar o estado atual da escultura (programa, posição, velocidade, status).
6. Reter repasses enquanto o programa da escultura não for conhecido, evitando estados inválidos.

## Funcionalidades implementadas

1. API HTTP REST executando em C++17 sobre Jetson Orin Nano, porta 8091.
2. Recebimento e bufferização de até 64 amostras recentes dos monitores.
3. Agregação de amostras por origem (último valor registrado por monitor).
4. Regra de importância: estímulos de segurança, anomalia sinalizada, status de falha (`erro`, `falha`, `pânico`), mudança de programa e velocidade acima do limiar configurado.
5. Consolidação de estado: programa, posição (x, y, z), velocidade, status e timestamp.
6. Repasse imediato à Central de Operação ao detectar amostra importante com programa já conhecido.
7. Heartbeat periódico (padrão: 5 s) com envio à Central de Operação somente se houver mudança relevante no estado consolidado.
8. Retenção de repasse enquanto o programa da escultura não for conhecido, evitando enviar `programa=0`.
9. Configuração em tempo de execução da URL da Central de Operação (`POST /config/central-operacao`), também inicializada na subida pela variável de ambiente `CENTRAL_OPERACAO_URL` (default `http://central-operacao:8080/estado`).
10. Parametrização do limiar de velocidade considerado alto.
11. Buffer de logs temporário em memória exposto como texto puro via endpoint dedicado.

## Fluxo de funcionamento

1. Inicialização do servidor HTTP e registro de rotas REST.
2. Criação de thread de background para heartbeat periódico e processamento da fila de repasse HTTP.
3. Recebimento de amostras via `POST /monitoramento` com campos `origem`, `tipo` e `dados`.
4. Bufferização e atualização da agregação por origem.
5. Avaliação de importância da amostra antes da consolidação (para detectar mudanças de estado).
6. Consolidação do estado com os campos presentes na amostra recebida.
7. Agendamento de repasse imediato à Central de Operação se a amostra for importante e o programa já for conhecido.
8. Thread de background executa o repasse HTTP pendente e verifica o heartbeat a cada 10 ms.
9. No heartbeat, agenda repasse apenas se o estado consolidado mudou desde o último envio.

## Endpoints da aplicação

1. `GET /` — informações do servidor, estatísticas e listagem das rotas disponíveis.
2. `POST /monitoramento` — recebe amostras dos monitores da camada C.
3. `GET /agregado` — estado consolidado atual e último valor reportado por cada origem.
4. `GET /buffer` — amostras recentes armazenadas no buffer.
5. `GET /config` — leitura das configurações atuais (URL da Central de Operação, limiares, período de repasse).
6. `POST /config/central-operacao` — atualização da URL de destino do repasse.
7. `POST /config/limiares` — atualização do limiar de velocidade considerado alto.
8. `GET /logs` — leitura do buffer de logs em texto puro.
9. `DELETE /logs` — limpeza do buffer de logs.

---

# Componente Atuador Central

Este documento descreve o componente **Atuador Central** da Camada B (Plataforma), responsável por oferecer uma interface centralizada de comando dos atuadores da camada inferior (C - Biblioteca).

## Objetivo

O Atuador Central atua como *middleware* entre a camada de Aplicação (A) e a camada de Biblioteca (C). Ele recebe programações da camada superior, gerencia uma fila de execução com prioridades e traduz cada movimento para o **protocolo Jellyfish V3** (`{ "id": N, ... }` sobre HTTP POST) esperado pelos atuadores/placas da camada inferior.

## Responsabilidades

1. Oferecer uma interface centralizada para comando dos atuadores.
2. Ser acionado pelo **Programador de atuação** (para realizar uma programação) ou pela **Central de operação** (para parada segura em caso de risco).
3. Aceitar programação **completa** (planejada do início ao fim) ou **parcial**.
4. Receber novas programações a qualquer momento, respeitando a prioridade na fila: executar **imediatamente**, como **próximo** movimento, ou **após** a fila.
5. Comandar os atuadores da camada C por meio de uma linguagem básica unificada.

## Integração com as demais camadas

1. **Camada superior (A - Aplicação):**
   - `Programador de atuação` → `POST /programar` (ou `POST /atuador`, compatível com o payload `{comando, detalhes}` já enviado por ele).
   - `Central de operação` → `POST /parada-segura` para interromper a execução em caso de risco.
2. **Camada inferior (C - Biblioteca):** traduz cada movimento para o **protocolo Jellyfish V3** e o envia à placa correspondente (`grafico`/`iluminacao`, `motor_fixo`, `motor_movel`, `audio`). As URLs das placas são configuradas por variáveis de ambiente na subida (`ATUADOR_GRAFICO_URL`, `ATUADOR_MOTOR_FIXO_URL`, `ATUADOR_MOTOR_MOVEL_URL`, `ATUADOR_AUDIO_URL`, `ATUADOR_ILUMINACAO_URL`, `ATUADOR_PTZ_URL`) ou em tempo de execução via `POST /config/atuadores`. Sem URL configurada, o atuador opera em modo simulação (apenas log, exibindo o payload Jellyfish que seria enviado).

### Tradução para o protocolo Jellyfish V3

Cada movimento (`atuador` + `acao` + `parametros`) é convertido no corpo JSON `{ "id": N, ... }`. Se o movimento já trouxer um `id` numérico em `parametros`, ele é repassado sem alteração (*pass-through*). Atuadores fora do protocolo (ex.: `ptz`) mantêm o formato genérico interno.

| `atuador` (camada B) | `acao` | Payload Jellyfish enviado à placa |
|---|---|---|
| `grafico` / `iluminacao` | `cor` / `cor_customizada` (com `r,g,b`) | `{"id":10,"r":..,"g":..,"b":..}` |
| `grafico` / `iluminacao` | `vermelho` / `verde` / `azul` / `preto`(ou `limpar`) / `branco` | `{"id":11..15}` |
| `motor_fixo` | `abrir` / `fechar` / `parar` | `{"id":21}` / `{"id":22}` / `{"id":20}` |
| `motor_movel` | `subir` / `descer` / `parar` | `{"id":31}` / `{"id":32}` / `{"id":30}` |
| `audio` | `reproduzir`(ou `tocar`, com `file`) / `volume`(com `volume`) / `parar` | `{"id":41,"file":..}` / `{"id":42,"volume":..}` / `{"id":40}` |

Na **parada segura**, cada atuador recebe o comando de PARAR da sua família: `grafico` → `{"id":14}` (limpar), `motor_fixo` → `{"id":20}`, `motor_movel` → `{"id":30}`, `audio` → `{"id":40}`.

## Funcionalidades implementadas

1. Fila de programação com prioridades (imediata / próxima / fim de fila).
2. Interrupção e substituição da programação atual por uma programação imediata.
3. Execução sequencial dos movimentos de cada programação por uma thread dedicada (equivalente ao `loop()`).
4. Tradução de cada movimento para o protocolo Jellyfish V3 (`{"id": N, ...}`) e envio à placa correspondente via HTTP POST.
5. Parada segura acionada pela Central de operação, com limpeza da fila e comando de PARAR (Jellyfish) a todos os atuadores.
6. Configuração das URLs dos atuadores da camada C por variável de ambiente na subida e em tempo de execução (`POST /config/atuadores`).
7. Buffer de logs temporário em RAM, exposto como txt.

## Endpoints da aplicação

1. `GET /` — informações do serviço e listagem das rotas.
2. `GET /estado` — programação atual, fila e estatísticas.
3. `GET /fila` — itens da fila de programação.
4. `POST /programar` — recebe uma programação (com prioridade) do Programador de atuação.
5. `POST /atuador` — compatibilidade com o payload `{comando, detalhes}` atual do Programador de atuação.
6. `POST /parada-segura` — parada segura acionada pela Central de operação.
7. `POST /retomar` — libera o atuador após a parada segura.
8. `GET /config/atuadores` — URLs configuradas dos atuadores da camada C.
9. `POST /config/atuadores` — define a URL HTTP de cada atuador da camada C.
10. `GET /logs` / `DELETE /logs` — leitura e limpeza do buffer de logs.

---

# Integração entre os componentes

Esta seção resume como os componentes se conectam. Cada serviço expõe uma API HTTP própria e envia a alvos configuráveis por **variável de ambiente** na subida (sobrescrevíveis em runtime pelos endpoints `POST /config/*`). Sem alvo configurado, cada elo degrada de forma segura (log / modo simulação).

## Fluxo de atuação (descendente): estímulo → movimento

1. **Processador de Imagem / Áudio (B)** → `Programador de atuação (A)`: `POST /gesture_update` e `POST /audioUpdate`.
2. **Programador de atuação (A)** → `Atuador Central (B)`: ao casar um estímulo com um programa, monta a programação e envia `POST /programar`. Alvo: `ATUADOR_CENTRAL_URL`.
3. **Atuador Central (B)** → **Placas (C)**: enfileira por prioridade, executa cada movimento, traduz para **Jellyfish V3** (`{"id": N, ...}`) e faz `POST` na placa. Alvos: `ATUADOR_*_URL`.

## Fluxo de monitoramento e segurança (ascendente)

1. **Monitores (C)** → `Central de Monitoramento (B)`: `POST /monitoramento`.
2. **Central de Monitoramento (B)** → `Central de Operação (A)`: repassa o estado consolidado relevante via `POST /estado`. Alvo: `CENTRAL_OPERACAO_URL`.
3. **Central de Operação (A)**: ao detectar anomalia, aciona `POST /parada-segura` no `Atuador Central (B)` (alvo `ATUADOR_CENTRAL_URL`) e notifica o `Registrador` via webhook (alvo `REGISTRADOR_URL`).

## Mapa de portas e variáveis de ambiente

| Componente | Camada | Porta | Envia para | Variável de ambiente |
|---|---|---|---|---|
| Programador de atuação | A | 8180 | Atuador Central `/programar` | `ATUADOR_CENTRAL_URL` |
| Central de Operação | A | 8080 | Atuador Central `/parada-segura`; Registrador | `ATUADOR_CENTRAL_URL`, `REGISTRADOR_URL` |
| Atuador Central | B | 8090 | Placas da camada C (Jellyfish V3) | `ATUADOR_GRAFICO_URL`, `ATUADOR_MOTOR_FIXO_URL`, `ATUADOR_MOTOR_MOVEL_URL`, `ATUADOR_AUDIO_URL`, `ATUADOR_ILUMINACAO_URL`, `ATUADOR_PTZ_URL` |
| Central de Monitoramento | B | 8091 | Central de Operação `/estado` | `CENTRAL_OPERACAO_URL` |

> Nota: o protocolo Jellyfish V3 define os endpoints das placas como `/tela` (Família 10), `/motor_fixo` (20), `/motor_movel` (30) e `/audio` (40). O caminho faz parte da URL configurada em cada `ATUADOR_*_URL`.
