# =============================================================
#  Dockerfile unico para todos os apps C++ do T4.1
#
#  Args de build:
#    APP_SRC  — arquivo .cpp a compilar  (obrigatorio)
#    KIND     — httplib | winsock | curl  (default: httplib)
#    APP_ARGS — argumentos passados ao binario no CMD
# =============================================================

# ── Estagio de build ─────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ARG APP_SRC
ARG KIND=httplib

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ curl ca-certificates libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Dependencias header-only (httplib + nlohmann/json)
RUN mkdir -p deps/nlohmann && \
    curl -fsSL \
      https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h \
      -o deps/httplib.h && \
    curl -fsSL \
      https://github.com/nlohmann/json/releases/latest/download/json.hpp \
      -o deps/nlohmann/json.hpp

COPY ${APP_SRC} ./src.cpp

RUN EXTRA_LIBS=""; \
    EXTRA_FLAGS=""; \
    if [ "$KIND" = "httplib" ]; then EXTRA_FLAGS="-I deps"; fi; \
    if [ "$KIND" = "curl" ];    then EXTRA_FLAGS="-I deps"; EXTRA_LIBS="-lcurl"; fi; \
    g++ -std=c++17 -O2 -pthread $EXTRA_FLAGS src.cpp -o app $EXTRA_LIBS

# ── Estagio de runtime ───────────────────────────────────────
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/app /usr/local/bin/app

ENTRYPOINT ["/usr/local/bin/app"]
