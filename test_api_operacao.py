import requests
import time

BASE_URL = "http://localhost:8080"

def test_deve_iniciar_sistema_limpo():
    response = requests.get(f"{BASE_URL}/")
    data = response.json()
    assert response.status_code == 200
    assert data["emergencia"] is False

def test_deve_aceitar_estado_seguro_e_gravar_na_jornada():
    res_inicio = requests.post(f"{BASE_URL}/jornadas/inicio", json={"id_jornada": "JORNADA_TESTE"})
    assert res_inicio.status_code == 201
    
    payload_estado = {
        "programa": 1,
        "velocidade": 50,
        "posicao": {"x": 0.0, "y": 0.0, "z": 0.0},
        "status": "executando"
    }
    res_estado = requests.post(f"{BASE_URL}/estado", json=payload_estado)
    assert res_estado.status_code == 200
    assert res_estado.json()["aceito"] is True
    
    res_jornada = requests.get(f"{BASE_URL}/jornadas/detalhe?idx=0")
    assert res_jornada.json()["num_eventos"] == 1

def test_deve_detectar_anomalia_de_salto_e_disparar_emergencia():
    payload_seguro = {
        "programa": 1, "velocidade": 50, "posicao": {"x": 0.0, "y": 0.0, "z": 0.0}, "status": "executando"
    }
    requests.post(f"{BASE_URL}/estado", json=payload_seguro)
    
    payload_anomalia = {
        "programa": 1,
        "velocidade": 50,
        "posicao": {"x": 500.0, "y": 0.0, "z": 0.0}, 
        "status": "executando"
    }
    res = requests.post(f"{BASE_URL}/estado", json=payload_anomalia)
    
    assert res.status_code == 503
    data = res.json()
    assert data["aceito"] is False
    assert data["emergencia"] is True
    assert "salto brusco" in data["motivo"]

def test_sistema_em_emergencia_deve_bloquear_novos_comandos():
    payload_seguro = {
        "programa": 1, "velocidade": 10, "posicao": {"x": 0, "y": 0, "z": 0}, "status": "parado"
    }
    res = requests.post(f"{BASE_URL}/estado", json=payload_seguro)
    
    assert res.status_code == 503
    assert res.json()["erro"] == "sistema em parada de emergencia"

def test_deve_destravar_sistema_apos_reset_manual():
    res_reset = requests.post(f"{BASE_URL}/emergencia/reset")
    assert res_reset.status_code == 200
    assert res_reset.json()["emergencia"] is False
    
    res_get = requests.get(f"{BASE_URL}/")
    assert res_get.json()["emergencia"] is False