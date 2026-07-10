import requests
import pytest

BASE_URL = "http://localhost:8080"

def test_01_rota_info_deve_retornar_200():
    """Testa o GET / (Info do servidor)"""
    res = requests.get(f"{BASE_URL}/")
    assert res.status_code == 200
    assert res.json()["servico"] == "central_operacao"

def test_02_configurar_registrador_externo_deve_retornar_200():
    """Testa o POST /config/registrador"""
    payload = {"url": "http://localhost:9999/meu-webhook"}
    res = requests.post(f"{BASE_URL}/config/registrador", json=payload)
    assert res.status_code == 200
    assert res.json()["aceito"] is True

def test_03_consultar_configuracoes_deve_conter_url_nova():
    """Testa o GET /config (Verifica se a URL salvou e lê limites)"""
    res = requests.get(f"{BASE_URL}/config")
    assert res.status_code == 200
    dados = res.json()
    assert dados["registrador_url"] == "http://localhost:9999/meu-webhook"
    assert "velocidade_max" in dados["limites"] 

def test_04_listar_presets_deve_retornar_5_estados():
    """Testa o GET /estado/presets"""
    res = requests.get(f"{BASE_URL}/estado/presets")
    assert res.status_code == 200
    assert len(res.json()["presets"]) == 5

def test_05_aplicar_preset_invalido_deve_retornar_400():
    """Testa o POST /estado/preset?id=99 (Caso de Erro)"""
    res = requests.post(f"{BASE_URL}/estado/preset?id=99")
    assert res.status_code == 400 

def test_06_aplicar_preset_valido_deve_retornar_200():
    """Testa o POST /estado/preset?id=2 (Operacional)"""
    res = requests.post(f"{BASE_URL}/estado/preset?id=2")
    assert res.status_code == 200
    assert res.json()["aceito"] is True
    assert res.json()["preset"] == "operacional"

def test_07_consultar_estado_atual_deve_bater_com_preset():
    """Testa o GET /estado"""
    res = requests.get(f"{BASE_URL}/estado")
    assert res.status_code == 200
    assert res.json()["programa"] == 2 
    assert res.json()["status"] == "executando"

def test_08_aplicar_estado_aleatorio_deve_retornar_200():
    """Testa o POST /estado/aleatorio"""
    res = requests.post(f"{BASE_URL}/estado/aleatorio")
    assert res.status_code == 200
    assert res.json()["aleatorio"] is True

def test_09_iniciar_jornada_deve_retornar_201():
    """Testa o POST /jornadas/inicio"""
    res = requests.post(f"{BASE_URL}/jornadas/inicio", json={"id_jornada": "TURNO_A"})
    assert res.status_code == 201
    assert res.json()["id_jornada"] == "TURNO_A"

def test_10_enviar_estado_json_invalido_deve_retornar_400():
    """Testa o POST /estado com formato quebrado (Caso de Erro do PDF)"""
    res = requests.post(f"{BASE_URL}/estado", data="ISSO NAO E UM JSON")
    assert res.status_code == 400

def test_11_enviar_estado_seguro_deve_gravar_na_jornada():
    """Testa o POST /estado (Fluxo normal)"""
    payload = {"programa": 1, "velocidade": 50, "posicao": {"x": 0, "y": 0, "z": 0}, "status": "executando"}
    requests.post(f"{BASE_URL}/estado", json=payload)
    res = requests.post(f"{BASE_URL}/estado", json=payload)
    assert res.status_code == 200

def test_12_encerrar_jornada_deve_retornar_200():
    """Testa o POST /jornadas/fim"""
    res = requests.post(f"{BASE_URL}/jornadas/fim", json={"id_jornada": "TURNO_A"})
    assert res.status_code == 200
    assert res.json()["duracao_s"] >= 0

def test_13_listar_jornadas_deve_conter_turno_a():
    """Testa o GET /jornadas"""
    res = requests.get(f"{BASE_URL}/jornadas")
    assert res.status_code == 200
    assert res.json()["total"] >= 1

def test_14_consultar_detalhe_da_jornada_deve_ter_eventos():
    """Testa o GET /jornadas/detalhe?idx=0"""
    res = requests.get(f"{BASE_URL}/jornadas/detalhe?idx=0")
    assert res.status_code == 200
    assert res.json()["id"] == "TURNO_A"
    assert len(res.json()["eventos"]) > 0

def test_15_emergencia_manual_deve_travar_sistema():
    """Testa o POST /emergencia"""
    res = requests.post(f"{BASE_URL}/emergencia", json={"motivo": "Botao do Panico"})
    assert res.status_code == 200
    assert res.json()["emergencia"] is True

def test_16_sistema_em_emergencia_deve_rejeitar_comandos_com_503():
    """Garante que o bloqueio funciona no POST /estado"""
    payload = {"programa": 1, "velocidade": 10, "posicao": {"x": 0, "y": 0, "z": 0}}
    res = requests.post(f"{BASE_URL}/estado", json=payload)
    assert res.status_code == 503
    assert res.json()["erro"] == "sistema em parada de emergencia"

def test_17_reset_de_emergencia_deve_liberar_sistema():
    """Testa o POST /emergencia/reset"""
    res = requests.post(f"{BASE_URL}/emergencia/reset")
    assert res.status_code == 200
    assert res.json()["emergencia"] is False

def test_18_sistema_liberado_deve_aceitar_estado_novamente():
    """Garante que o reset funcionou"""
    payload = {"programa": 1, "velocidade": 10, "posicao": {"x": 0, "y": 0, "z": 0}}
    res = requests.post(f"{BASE_URL}/estado", json=payload)
    assert res.status_code == 200

def test_19_ler_logs_deve_retornar_texto():
    """Testa o GET /logs"""
    res = requests.get(f"{BASE_URL}/logs")
    assert res.status_code == 200
    assert "Sistema iniciando" in res.text or "[US06]" in res.text

def test_20_limpar_logs_deve_retornar_200():
    """Testa o DELETE /logs"""
    res = requests.delete(f"{BASE_URL}/logs")
    assert res.status_code == 200
    
    res_get = requests.get(f"{BASE_URL}/logs")
    
    assert res_get.text.strip() == "" or "limpo" in res_get.text.lower()