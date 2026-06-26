import requests
import time

BASE_URL = "http://localhost:8090"

def test_estado_inicial_do_servidor():
    response = requests.get(f"{BASE_URL}/estado")
    data = response.json()

    assert response.status_code == 200
    assert data["servico"] == "atuador_central"
    assert data["parada_segura"] is False
    assert data["fila"] == 0

def test_deve_aceitar_uma_nova_programacao():
    payload = {
        "id": "TESTE_UNITARIO_01",
        "origem": "pytest",
        "prioridade": "fila",
        "movimentos": [
            {"atuador": "motor_movel", "acao": "mover", "duracao_ms": 100}
        ]
    }
    
    response = requests.post(f"{BASE_URL}/programar", json=payload)
    
    assert response.status_code == 201
    assert response.json()["aceito"] is True
    assert response.json()["id"] == "TESTE_UNITARIO_01"

def test_deve_bloquear_tudo_na_parada_segura():
    payload = {"motivo": "risco_identificado_pelo_teste"}
    
    response = requests.post(f"{BASE_URL}/parada-segura", json=payload)
    
    assert response.status_code == 200
    assert response.json()["parada_segura"] is True
    assert response.json()["motivo"] == "risco_identificado_pelo_teste"

    res_fila = requests.get(f"{BASE_URL}/fila")
    assert res_fila.json()["tamanho"] == 0

def test_deve_retomar_operacao_normal():
    response = requests.post(f"{BASE_URL}/retomar")
    
    assert response.status_code == 200
    assert response.json()["parada_segura"] is False
    assert response.json()["aceito"] is True