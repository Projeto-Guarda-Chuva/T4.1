cmake_minimum_required(VERSION 3.15)
project(AtuadorCentral)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

add_executable(atuador_central atuador_central.cpp)

if(WIN32)
    target_link_libraries(atuador_central PRIVATE nlohmann_json::nlohmann_json httplib Ws2_32)
else()
    target_link_libraries(atuador_central PRIVATE nlohmann_json::nlohmann_json httplib)
endif()

add_executable(central_monitoramento central_de_monitoramento.cpp)

if(WIN32)
    target_link_libraries(central_monitoramento PRIVATE nlohmann_json::nlohmann_json httplib Ws2_32)
else()
    target_link_libraries(central_monitoramento PRIVATE nlohmann_json::nlohmann_json httplib)
endif()

add_executable(central_operacao central_de_operacao.cpp)

if(WIN32)
    target_link_libraries(central_operacao PRIVATE nlohmann_json::nlohmann_json httplib Ws2_32)
else()
    target_link_libraries(central_operacao PRIVATE nlohmann_json::nlohmann_json httplib)
endif()

add_executable(programador_local programador_de_atuacao_local.cpp)

if(WIN32)
    target_link_libraries(programador_local PRIVATE Ws2_32)
endif()