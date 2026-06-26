import requests
import pytest

BASE_URL = "http://localhost:8180"

@pytest.fixture(autouse=True)
def limpar_memoria_do_servidor():
    requests.delete(f"{BASE_URL}/parametrizar")
    requests.delete(f"{BASE_URL}/programas")
    yield 
    requests.delete(f"{BASE_URL}/parametrizar")
    requests.delete(f"{BASE_URL}/programas")

def test_deve_retornar_saude_do_sistema():
    res = requests.get(f"{BASE_URL}/health")
    assert res.status_code == 200
    assert res.json()["status"] == "online"

def test_deve_atualizar_sensibilidade():
    payload = {"sensibilidade": 0.8}
    res = requests.post(f"{BASE_URL}/parametrizar", json=payload)
    assert res.status_code == 200
    assert "salva" in res.json()["mensagem"]

def test_deve_adicionar_novo_programa():
    payload = {
        "acao": "palmas",
        "velocidade": 100
    }
    res = requests.post(f"{BASE_URL}/adicionar-programa", json=payload)
    assert res.status_code == 201
    assert "salva" in res.json()["mensagem"]

def test_deve_ignorar_estimulo_abaixo_da_sensibilidade():
    requests.post(f"{BASE_URL}/parametrizar", json={"sensibilidade": 0.8})
    
    payload = {"acao_detectada": "palmas", "intensidade": 0.2}
    res = requests.post(f"{BASE_URL}/processar-estimulo", json=payload)
    
    assert res.status_code == 200
    assert res.json()["mensagem"] == "Estimulo ignorado"

def test_deve_processar_estimulo_valido():
    requests.post(f"{BASE_URL}/parametrizar", json={"sensibilidade": 0.8})
    requests.post(f"{BASE_URL}/adicionar-programa", json={"acao": "palmas", "velocidade": 100})
    
    payload = {"acao_detectada": "palmas", "intensidade": 0.9}
    res = requests.post(f"{BASE_URL}/processar-estimulo", json=payload)
    
    assert res.status_code == 200
    assert res.json()["mensagem"] == "Estimulo processado"

def test_deve_rejeitar_estimulo_nao_mapeado():
    requests.post(f"{BASE_URL}/parametrizar", json={"sensibilidade": 0.8})
    
    payload = {"acao_detectada": "assobio", "intensidade": 0.9}
    res = requests.post(f"{BASE_URL}/processar-estimulo", json=payload)
    
    assert res.status_code == 404
    assert "nao mapeada" in res.json()["erro"]