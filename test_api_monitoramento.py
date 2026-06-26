import requests
import time

BASE_URL = "http://localhost:8091"

def test_estado_inicial_do_servidor():
    response = requests.get(f"{BASE_URL}/")
    data = response.json()
    assert response.status_code == 200
    assert data["servico"] == "central_monitoramento"
    assert data["buffer"] == 0

def test_deve_configurar_url_da_central_de_operacao():
    payload = {"url": "http://localhost:9999/estado-falso"}
    response = requests.post(f"{BASE_URL}/config/central-operacao", json=payload)
    
    assert response.status_code == 200
    assert response.json()["aceito"] is True
    assert response.json()["central_op_url"] == "http://localhost:9999/estado-falso"

def test_deve_bufferizar_amostra_normal_sem_repassar():
    requests.post(f"{BASE_URL}/monitoramento", json={
        "origem": "monitor_jetson", "tipo": "estado", "dados": {"programa": 1}
    })
    payload = {
        "origem": "monitor_jetson",
        "tipo": "estado",
        "dados": {"programa": 1, "velocidade": 10, "posicao": {"x": 10, "y": 20, "z": 0}}
    }
    response = requests.post(f"{BASE_URL}/monitoramento", json=payload)
    
    assert response.status_code == 200
    assert response.json()["aceito"] is True
    assert response.json()["repassado"] is False

def test_deve_disparar_repasse_se_velocidade_for_alta():
    payload = {
        "origem": "monitor_jetson",
        "tipo": "estado",
        "dados": {"velocidade": 160}
    }
    response = requests.post(f"{BASE_URL}/monitoramento", json=payload)
    
    assert response.status_code == 200
    assert response.json()["repassado"] is True
    assert "velocidade alta" in response.json()["motivo"]

def test_o_estado_agregado_deve_ter_a_ultima_posicao_e_velocidade_alta():
    response = requests.get(f"{BASE_URL}/agregado")
    
    assert response.status_code == 200
    data = response.json()
    assert data["valido"] is True
    assert data["consolidado"]["programa"] == 1
    assert data["consolidado"]["velocidade"] == 160
    assert data["consolidado"]["posicao"]["x"] == 10