# Especificação de Componentes — Camadas A (Aplicação) e B (Plataforma)

Este documento especifica as funcionalidades dos quatro componentes de software das
camadas superiores do projeto, descrevendo **o que cada componente faz** e **por quê**.
Baseado no código de `programador_de_atuacao_local.cpp`, `atuador_central.cpp`,
`central_de_monitoramento.cpp` e `central_de_operacao.cpp`.

| Componente | Camada | Porta |
|---|---|---|
| Programador de Atuação | A - Aplicação | 8180 |
| Atuador Central | B - Plataforma | 8090 |
| Central de Monitoramento | B - Plataforma | 8091 |
| Central de Operação | A - Aplicação | 8080 |

---

## 1. Programador de Atuação — Camada A (Aplicação), porta 8180

### O que faz e por quê
É o **cérebro de decisão** do sistema. Recebe as descobertas dos processadores de
imagem e áudio (camada B), decide *se* e *o que* acionar consultando um banco de
programas, e encaminha a ordem ao Atuador Central. Existe para separar a **lógica de
"o que fazer"** (regras de negócio) da **execução mecânica** — a camada B só sabe
executar, não decidir.

### Funcionalidades

| # | Funcionalidade | O que faz | Por quê |
|---|---|---|---|
| 1 | Servidor HTTP próprio | Sobe um servidor de sockets multiplataforma (winsock/POSIX) na porta 8180 | Ser acionável pelos processadores e por clientes de configuração sem depender de framework pesado |
| 2 | Parametrização de sensibilidade | `POST /parametrizar` grava `sensibilidadeGlobal` | Definir um **limiar** para ignorar estímulos fracos (ruído), evitando movimento à toa |
| 3 | Cadastro de programas | `POST /adicionar-programa` guarda objetos `{acao, atuador, movimento, velocidade...}` | É o **mapa "ação detectada → movimento"**; sem ele o sistema não sabe o que fazer com um gesto |
| 4 | Processamento de estímulo | `POST /processar-estimulo` aplica o limiar e busca o programa da ação | Núcleo da decisão: valida intensidade e resolve qual programa executar |
| 5 | Busca de programa por ação | `findProgramaByAcao` percorre o banco | Traduzir uma ação abstrata ("abrir") no programa concreto cadastrado |
| 6 | **Disparo ao Atuador Central** | Monta `{movimentos:[...], prioridade}` e faz `POST /programar` | Cumpre a spec item 4 ("encaminhar comandos de execução ao atuador da plataforma") — é o elo A→B |
| 7 | Integração com Processador de Imagem | `POST /gesture_update` (objeto plano `{gesture, confidence, speed, count}`) traduz `gesture→acao`, `confidence→intensidade` | Conectar a visão computacional (YOLO) ao pipeline de decisão; gesto `REST` é aceito sem agir |
| 8 | Integração com Processador de Áudio | `POST /audioUpdate` traduz `payload→acao` | Conectar reconhecimento de voz; `type` diferente de `command` é aceito sem agir |
| 9 | Persistência local (banco de configuração) | Salva programas/parametrização/eventos em `dados/programador_de_atuacao.dados.json`, cria diretório e faz fallback em memória | Spec item 1.b: manter programações antigas e atuais entre reinícios |
| 10 | Registro de eventos | `registrarEvento` grava tipo + timestamp + payload | Rastreabilidade/auditoria de tudo que foi decidido |
| 11 | Configuração de alvo por ambiente | Lê `ATUADOR_CENTRAL_URL` na subida | Trocar o destino sem recompilar (deploy flexível) |

### Endpoints
`GET /`, `/health`, `/parametrizar`, `/programas`, `/eventos`;
`POST /parametrizar`, `/adicionar-programa`, `/processar-estimulo`, `/gesture_update`, `/audioUpdate`;
`DELETE` correspondentes.

---

## 2. Atuador Central — Camada B (Plataforma), porta 8090

### O que faz e por quê
É o **middleware de atuação**: uma interface única entre a decisão (camada A) e as
placas físicas (camada C). Gerencia uma fila de execução com prioridades e **traduz**
cada movimento para o protocolo Jellyfish V3 que as placas entendem. Existe para que a
camada A não precise conhecer IPs, protocolos ou detalhes de cada placa — ela só envia
"programações".

### Funcionalidades

| # | Funcionalidade | O que faz | Por quê |
|---|---|---|---|
| 1 | Fila com prioridades | Enfileira programação como `imediata` / `proxima` / `fila` | Spec item d: novas ordens podem chegar a qualquer momento e precisam de ordem de execução |
| 2 | Interrupção da atual | Prioridade `imediata` seta `g_abort_atual` e fura a fila | Permitir reação urgente (ex.: sobrepor um movimento em curso) |
| 3 | Execução sequencial (worker) | Thread dedicada consome a fila movimento a movimento | Equivalente ao `loop()` do firmware: executa sem travar o servidor HTTP |
| 4 | **Tradução Jellyfish V3** | `traduzir_jellyfish` converte `atuador+acao+parametros` em `{"id":N,...}` | Falar o protocolo real das placas; sem isso a camada C ignoraria os comandos |
| 5 | Envio HTTP às placas + modo simulação | `POST` no endpoint configurado; sem URL, só loga o payload | Interface com camada C; a simulação permite operar/testar sem hardware |
| 6 | **Parada segura** | `POST /parada-segura` limpa a fila e manda PARAR (Jellyfish) a todas as placas | Segurança: a Central de Operação interrompe tudo em caso de risco |
| 7 | Retomada | `POST /retomar` libera após inspeção | Voltar à operação normal só por decisão explícita |
| 8 | Configuração de URLs | Env `ATUADOR_*_URL` na subida + `POST /config/atuadores` | Mapear cada placa (IP/endpoint) sem recompilar |
| 9 | Programação completa ou parcial | Aceita sequência inteira ou trecho | Spec item c: nem toda programação vai do início ao fim |
| 10 | Compatibilidade `/atuador` | Aceita `{comando, detalhes}` e traduz p/ 1 movimento | Retrocompatibilidade com o payload antigo do Programador |
| 11 | Buffer de logs | Registro em RAM exposto como txt (`GET/DELETE /logs`) | Diagnóstico sem depender de infraestrutura externa |

### Tradução para o protocolo Jellyfish V3

Cada movimento (`atuador` + `acao` + `parametros`) é convertido no corpo JSON
`{ "id": N, ... }`. Se o movimento já trouxer um `id` numérico em `parametros`, ele é
repassado sem alteração (*pass-through*). Atuadores fora do protocolo (ex.: `ptz`)
mantêm o formato genérico interno.

| `atuador` (camada B) | `acao` | Payload Jellyfish enviado à placa |
|---|---|---|
| `grafico` / `iluminacao` | `cor` / `cor_customizada` (com `r,g,b`) | `{"id":10,"r":..,"g":..,"b":..}` |
| `grafico` / `iluminacao` | `vermelho` / `verde` / `azul` / `preto`(ou `limpar`) / `branco` | `{"id":11..15}` |
| `motor_fixo` | `abrir` / `fechar` / `parar` | `{"id":21}` / `{"id":22}` / `{"id":20}` |
| `motor_movel` | `subir` / `descer` / `parar` | `{"id":31}` / `{"id":32}` / `{"id":30}` |
| `audio` | `reproduzir`(ou `tocar`, com `file`) / `volume`(com `volume`) / `parar` | `{"id":41,"file":..}` / `{"id":42,"volume":..}` / `{"id":40}` |

Na **parada segura**, cada atuador recebe o comando de PARAR da sua família:
`grafico` → `{"id":14}` (limpar), `motor_fixo` → `{"id":20}`, `motor_movel` → `{"id":30}`,
`audio` → `{"id":40}`.

### Endpoints
`GET /`, `/estado`, `/fila`;
`POST /programar`, `/atuador`, `/parada-segura`, `/retomar`;
`GET`/`POST /config/atuadores`; `GET`/`DELETE /logs`.

---

## 3. Central de Monitoramento — Camada B (Plataforma), porta 8091

### O que faz e por quê
É o **filtro/agregador de telemetria**: recebe amostras de todos os monitores da camada
C, consolida o estado da escultura e repassa à Central de Operação **apenas o que
importa**. Existe para proteger a camada A de sobrecarga — em vez de mil amostras
irrelevantes, ela recebe só mudanças significativas.

### Funcionalidades

| # | Funcionalidade | O que faz | Por quê |
|---|---|---|---|
| 1 | API REST (C++17) | Servidor httplib na porta 8091 | Ponto de coleta para os monitores da camada C |
| 2 | Recebimento e buffer | `POST /monitoramento` guarda até 64 amostras recentes | Manter histórico curto para inspeção (`GET /buffer`) |
| 3 | Agregação por origem | Guarda o último valor de cada monitor (Jetson/Wemos/P4) | Ter uma visão consolidada por fonte, não só a última amostra global |
| 4 | Regra de importância | Marca como importante: estímulo de segurança, anomalia, status de falha, mudança de programa, velocidade acima do limiar | Decidir o que merece subir para a camada A — o coração do "repasse só do relevante" |
| 5 | Consolidação de estado | Junta programa, posição (x,y,z), velocidade, status, timestamp | Manter o "estado atual" único da escultura |
| 6 | Repasse imediato | `POST /estado` à Central de Operação quando amostra importante + programa conhecido | Levar informação crítica sem esperar o heartbeat |
| 7 | Heartbeat periódico | A cada ~5 s envia só se o estado mudou | Garantir "vida"/atualização sem inundar a camada A |
| 8 | Retenção sem programa | Não repassa enquanto o programa for desconhecido | Evitar enviar estados inválidos (`programa=0`) |
| 9 | Config do destino | Env `CENTRAL_OPERACAO_URL` + `POST /config/central-operacao` | Apontar para a Central de Operação sem recompilar |
| 10 | Parametrização de limiar | `POST /config/limiares` ajusta o limite de velocidade "alta" | Calibrar a sensibilidade do filtro |
| 11 | Thread de background | Processa fila de repasse + heartbeat a cada 10 ms | Não bloquear o recebimento de amostras |
| 12 | Buffer de logs | Exposto como txt | Diagnóstico local |

### Endpoints
`GET /`, `/agregado`, `/buffer`, `/config`;
`POST /monitoramento`, `/config/central-operacao`, `/config/limiares`;
`GET`/`DELETE /logs`.

---

## 4. Central de Operação — Camada A (Aplicação), porta 8080

### O que faz e por quê
É o **supervisor de segurança**: acompanha o estado em tempo real, controla as jornadas
de movimentação, detecta anomalias e aciona a **parada segura**. Existe como camada de
controle independente da decisão de atuação — mesmo que o Programador ordene um
movimento, a Central de Operação pode vetá-lo se detectar risco.

### Funcionalidades

| # | Funcionalidade | O que faz | Por quê |
|---|---|---|---|
| 1 | API REST assíncrona | Servidor httplib na 8080 + thread de background | Receber estado e comandos sem bloquear |
| 2 | Estado da escultura (US06) | `GET`/`POST /estado` lê/atualiza o estado atual | Base para toda a supervisão |
| 3 | Gestão de jornadas (US07) | Abre/fecha jornadas e registra evento por amostra | Rastrear cada ciclo de movimentação para histórico/vídeo |
| 4 | Detecção de anomalias | Verifica velocidade, zona segura de posição, programa autorizado, status reportado e variação brusca (delta) | Núcleo de segurança: identificar comportamento fora do esperado |
| 5 | **Parada segura automática** | Bloqueia novas operações e comanda `POST /parada-segura` ao Atuador Central (thread destacada) | Spec item 2.c: interromper com segurança em caso de anomalia |
| 6 | Emergência manual | `POST /emergencia` dispara parada por comando | Permitir intervenção humana imediata |
| 7 | Reset de emergência | `POST /emergencia/reset` reativa após inspeção | Só voltar a operar por decisão explícita |
| 8 | Presets + estado aleatório | 5 estados predefinidos + gerador aleatório | Testes e demonstração sem hardware |
| 9 | Notificação do registrador | Webhook HTTP ao encerrar jornada e em emergências (`REGISTRADOR_URL`) | Spec item 2.d: transmitir histórico/vídeo ao registrador |
| 10 | Buffer de logs | txt em RAM (`GET`/`DELETE /logs`) | Diagnóstico e histórico recente |
| 11 | Configuração de destinos | `REGISTRADOR_URL`/`ATUADOR_CENTRAL_URL` por env + `POST /config/registrador` e `/config/atuador` | Ligar aos alvos sem recompilar |
| 12 | Parametrização de limites | Velocidade máx., zona segura, programas válidos, delta | Calibrar o que conta como anomalia |
| 13 | Recebimento de gestos | `POST /gesture_update` **armazena** o último gesto (`GET /gesto`) | Contexto de supervisão — observa, não aciona (o Programador é quem aciona) |
| 14 | Recebimento de áudio | `POST /audioUpdate` **armazena** o último comando (`GET /audio`) | Idem: contexto de supervisão |
| 15 | Timeout de estado | Marca `sem_sinal` se não chega estado no prazo | Detectar perda de comunicação com a camada inferior |

### Endpoints
`GET /`, `/estado`, `/gesto`, `/audio`, `/estado/presets`, `/jornadas`, `/jornadas/detalhe`, `/config`, `/logs`;
`POST /estado`, `/gesture_update`, `/audioUpdate`, `/estado/preset`, `/estado/aleatorio`, `/jornadas/inicio`, `/jornadas/fim`, `/emergencia`, `/emergencia/reset`, `/config/registrador`, `/config/atuador`;
`DELETE /logs`.

---

## Visão de conjunto — por que essa divisão existe

- **Programador (A) decide** o que fazer → **Atuador Central (B) executa/traduz** para as
  placas → **placas (C) atuam**. Esse é o fluxo **descendente** de atuação.
- **Monitores (C) reportam** → **Central de Monitoramento (B) filtra/agrega** →
  **Central de Operação (A) supervisiona** e, se preciso, **freia via Atuador Central**.
  Esse é o fluxo **ascendente** de monitoramento/segurança.

A chave arquitetural: **decisão (Programador) e segurança (Central de Operação) são
separadas e ambas na camada A**, enquanto a **plataforma (B) só faz mediação**
(traduzir/filtrar) — nenhuma delas fala diretamente com hardware, sempre pelas
interfaces da camada B.
