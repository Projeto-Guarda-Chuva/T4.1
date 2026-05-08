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
6. Disparo de comando para atuador externo.
7. Registro de logs para receptor externo.
8. Persistência local dos programas com fallback em memória.

## Fluxo de funcionamento

1. Inicialização de rede, armazenamento local e serviço HTTP.
2. Recebimento de parâmetros operacionais (ex.: sensibilidade).
3. Registro e atualização de programas no banco local.
4. Recebimento de estímulo com ação detectada e intensidade.
5. Validação da intensidade pelo limiar configurado.
6. Consulta do programa associado à ação.
7. Envio do comando ao atuador quando houver mapeamento válido.
8. Envio de log do processamento para rastreabilidade.

## Endpoints da aplicação

1. `GET /health`.
2. `POST /parametrizar`.
3. `POST /adicionar-programa`.
4. `POST /processar-estimulo`.

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
5. Parada segura automática em caso de anomalia, com bloqueio de novas operações.
6. Disparo manual de emergência por endpoint dedicado.
7. Reset de emergência após inspeção do sistema.
8. Cinco estados predefinidos (`inicial`, `operacional`, `alta_velocidade`, `manutencao`, `demonstracao`) e gerador de estado aleatório.
9. Notificação externa do registrador via webhook HTTP no encerramento de jornadas e em paradas de emergência.
10. Buffer de logs temporário em memória RAM, exposto como txt via endpoint dedicado.
11. Configuração em tempo de execução da URL do registrador externo.
12. Parametrização interna de limites operacionais (velocidade máxima, zona segura de posição, programas válidos e delta de posição).

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
4. `GET /estado/presets` — listagem dos cinco estados predefinidos.
5. `POST /estado/preset?id=N` — aplicação do preset identificado por N (1 a 5).
6. `POST /estado/aleatorio` — aplicação de estado aleatório dentro dos limites válidos.
7. `GET /jornadas` — listagem resumida das jornadas em memória.
8. `GET /jornadas/detalhe?idx=N` — detalhamento de uma jornada específica com seus eventos.
9. `POST /jornadas/inicio` — abertura de uma nova jornada de movimentação.
10. `POST /jornadas/fim` — encerramento da jornada aberta.
11. `POST /emergencia` — acionamento manual da parada de segurança.
12. `POST /emergencia/reset` — reativação do sistema após emergência.
13. `GET /config` — leitura dos limites operacionais e da URL do registrador.
14. `POST /config/registrador` — atualização da URL de notificação externa.
15. `GET /logs` — leitura do buffer de logs em texto puro.
16. `DELETE /logs` — limpeza do buffer de logs.