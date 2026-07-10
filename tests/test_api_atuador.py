import requests
import pytest

BASE_URL = "http://localhost:8090"

def test_01_rota_info_deve_retornar_200():
    """Testa o GET / (Informações do serviço)"""
    res = requests.get(f"{BASE_URL}/")
    assert res.status_code == 200
    assert "servico" in res.json() or "info" in res.text.lower()

def test_02_consultar_estado_inicial_deve_retornar_200():
    """Testa o GET /estado (Programação atual e estatísticas)"""
    res = requests.get(f"{BASE_URL}/estado")
    assert res.status_code == 200
    assert isinstance(res.json(), dict)

def test_03_consultar_fila_inicial_deve_retornar_200():
    """Testa o GET /fila (Itens da fila)"""
    res = requests.get(f"{BASE_URL}/fila")
    assert res.status_code == 200
    assert isinstance(res.json(), (list, dict))

def test_04_configurar_atuadores_deve_retornar_200():
    """Testa o POST /config/atuadores (Define URLs da Camada C)"""
    payload = {
        "motor_movel": "http://localhost:9001",
        "iluminacao": "http://localhost:9002"
    }
    res = requests.post(f"{BASE_URL}/config/atuadores", json=payload)
    assert res.status_code == 200

def test_05_consultar_configuracoes_deve_conter_urls_novas():
    """Testa o GET /config/atuadores (Lê URLs da Camada C)"""
    res = requests.get(f"{BASE_URL}/config/atuadores")
    assert res.status_code == 200
    assert "motor_movel" in str(res.json())

def test_06_programacao_invalida_deve_retornar_400():
    """Testa o POST /programar com payload quebrado"""
    res = requests.post(f"{BASE_URL}/programar", data="ISSO_NAO_E_JSON")
    assert res.status_code == 400

def test_07_programacao_valida_deve_enfileirar():
    """Testa o POST /programar (Fluxo normal)"""
    payload = {
        "programa": 1,
        "prioridade": "fim"
    }
    res = requests.post(f"{BASE_URL}/programar", json=payload)
    assert res.status_code in [200, 201]

def test_08_programacao_prioridade_imediata_deve_aceitar():
    """Testa o POST /programar (Prioridade Imediata)"""
    payload = {
        "programa": 2,
        "prioridade": "imediata"
    }
    res = requests.post(f"{BASE_URL}/programar", json=payload)
    assert res.status_code in [200, 201]

def test_09_rota_compatibilidade_atuador_deve_aceitar():
    """Testa o POST /atuador (Rota alternativa de compatibilidade)"""
    payload = {
        "comando": "piscar",
        "detalhes": {"acao": "piscar_led", "intensidade": 100}
    }
    res = requests.post(f"{BASE_URL}/atuador", json=payload)
    assert res.status_code in [200, 201]

def test_10_consultar_fila_deve_conter_itens_enfileirados():
    """Testa o GET /fila para garantir que as programações entraram"""
    res = requests.get(f"{BASE_URL}/fila")
    assert res.status_code == 200
    assert len(res.text) > 5

def test_11_acionar_parada_segura_deve_travar_atuador():
    """Testa o POST /parada-segura"""
    res = requests.post(f"{BASE_URL}/parada-segura", json={})
    assert res.status_code == 200

def test_12_programar_durante_parada_segura_deve_retornar_erro():
    """Testa o POST /programar (Recusar novas programações)"""
    payload = {
        "programa": 3, 
        "prioridade": "imediata"
    }
    res = requests.post(f"{BASE_URL}/programar", json=payload)
    assert res.status_code == 503

def test_13_acionar_retomar_deve_liberar_atuador():
    """Testa o POST /retomar"""
    res = requests.post(f"{BASE_URL}/retomar", json={})
    assert res.status_code == 200

def test_14_programar_apos_retomar_deve_funcionar():
    """Garante que o bloqueio foi removido após o reset"""
    payload = {
        "programa": 4, 
        "prioridade": "fim"
    }
    res = requests.post(f"{BASE_URL}/programar", json=payload)
    assert res.status_code in [200, 201]

def test_15_ler_logs_deve_retornar_texto():
    """Testa o GET /logs"""
    res = requests.get(f"{BASE_URL}/logs")
    assert res.status_code == 200
    assert len(res.text) > 0

def test_16_limpar_logs_deve_retornar_200():
    """Testa o DELETE /logs"""
    res = requests.delete(f"{BASE_URL}/logs")
    assert res.status_code == 200
    
    res_get = requests.get(f"{BASE_URL}/logs")
    assert res_get.text.strip() == "" or "limpo" in res_get.text.lower()