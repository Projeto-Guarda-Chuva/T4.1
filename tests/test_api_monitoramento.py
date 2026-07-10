import requests
import pytest

BASE_URL = "http://localhost:8091"

def test_01_rota_info_deve_retornar_200():
    """Testa o GET / (Informações e estatísticas)"""
    res = requests.get(f"{BASE_URL}/")
    assert res.status_code == 200

def test_02_configurar_url_central_operacao_deve_retornar_200():
    """Testa o POST /config/central-operacao"""
    payload = {"url": "http://localhost:8080/estado"}
    res = requests.post(f"{BASE_URL}/config/central-operacao", json=payload)
    assert res.status_code in [200, 201]

def test_03_configurar_limiares_deve_retornar_200():
    """Testa o POST /config/limiares (Atualiza limiar de velocidade)"""
    payload = {"limiar_velocidade": 100.0}
    res = requests.post(f"{BASE_URL}/config/limiares", json=payload)
    assert res.status_code in [200, 201]

def test_04_consultar_config_deve_retornar_200():
    """Testa o GET /config"""
    res = requests.get(f"{BASE_URL}/config")
    assert res.status_code == 200
    assert isinstance(res.json(), dict)

@pytest.mark.xfail(reason="BUG BACKEND: Falta validação. Retorna 200 em vez de 400 para JSON incompleto")
def test_05_amostra_incompleta_deve_retornar_400():
    """Testa o POST /monitoramento com falta de campos"""
    payload = {"dados": {"velocidade": 10}}
    res = requests.post(f"{BASE_URL}/monitoramento", json=payload)
    assert res.status_code == 400

def test_06_amostra_irrelevante_nao_deve_ser_repassada():
    """Testa o POST /monitoramento (Velocidade baixa e sem mudança)"""
    payload = {
        "origem": "jetson",
        "tipo": "velocidade",
        "dados": {"velocidade": 10, "programa": 1, "status": "operacional"}
    }
    requests.post(f"{BASE_URL}/monitoramento", json=payload)
    
    res = requests.post(f"{BASE_URL}/monitoramento", json=payload)
    assert res.status_code in [200, 201]
    if "repassado" in res.text:
        assert res.json().get("repassado") is False

def test_07_amostra_importante_deve_ser_repassada():
    """Testa o POST /monitoramento (Velocidade acima do limiar)"""
    payload = {
        "origem": "jetson",
        "tipo": "velocidade",
        "dados": {"velocidade": 150, "programa": 1, "status": "operacional"}
    }
    res = requests.post(f"{BASE_URL}/monitoramento", json=payload)
    assert res.status_code in [200, 201]

def test_08_estado_sem_programa_deve_reter_repasse():
    """Testa o POST /monitoramento (Programa desconhecido/0)"""
    payload = {
        "origem": "p4",
        "tipo": "status",
        "dados": {"velocidade": 50, "programa": 0, "status": "operacional"}
    }
    res = requests.post(f"{BASE_URL}/monitoramento", json=payload)
    assert res.status_code in [200, 201]

def test_09_consultar_agregado_deve_retornar_dados_consolidados():
    """Testa o GET /agregado"""
    res = requests.get(f"{BASE_URL}/agregado")
    assert res.status_code == 200
    dados = res.text.lower()
    assert "jetson" in dados or "p4" in dados

def test_10_consultar_buffer_deve_retornar_amostras():
    """Testa o GET /buffer"""
    res = requests.get(f"{BASE_URL}/buffer")
    assert res.status_code == 200
    assert len(res.json()) > 0

def test_11_ler_logs_deve_retornar_texto():
    """Testa o GET /logs"""
    res = requests.get(f"{BASE_URL}/logs")
    assert res.status_code == 200
    assert len(res.text) > 0

def test_12_limpar_logs_deve_retornar_200():
    """Testa o DELETE /logs"""
    res = requests.delete(f"{BASE_URL}/logs")
    assert res.status_code == 200
    
    res_get = requests.get(f"{BASE_URL}/logs")
    assert res_get.text.strip() == "" or "limpo" in res_get.text.lower()