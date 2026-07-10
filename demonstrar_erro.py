import json
import requests
import time

BASE_URL = "http://localhost:8080"
ARQUIVO_PARAMETROS = "parametros_entrada.json"

def executar_demonstracao():
    print("\n" + "="*70)
    print("🚀 INICIANDO DEMONSTRAÇÃO DE TESTES (CAMINHOS DE SUCESSO E ERRO)")
    print("="*70)

    with open(ARQUIVO_PARAMETROS, "r", encoding="utf-8") as file:
        casos_de_teste = json.load(file)

    for caso in casos_de_teste:
        print(f"\n⚙️  TESTE: {caso['funcionalidade']}")
        print(f"   🌐 Rota Acionada      : {caso['metodo']} {caso['rota']}")
        
        parametros_formatados = json.dumps(caso['parametros']) if caso['parametros'] else 'Nenhum payload'
        print(f"   📥 Dados Enviados     : {parametros_formatados}")
        
        url = f"{BASE_URL}{caso['rota']}"
        
        try:
            if caso['metodo'] == "POST":
                res = requests.post(url, json=caso['parametros'])
            else:
                res = requests.get(url)

            print(f"   📤 Resposta do C++    : HTTP {res.status_code} | {res.text.strip()}")
            
            if res.status_code == caso['status_esperado']:
                print("   ✅ STATUS DA VALIDAÇÃO: PASSOU! O sistema reagiu exatamente como esperado.")
            else:
                print(f"   ❌ STATUS DA VALIDAÇÃO: FALHOU! Esperava {caso['status_esperado']}, mas retornou {res.status_code}.")
                
        except requests.exceptions.ConnectionError:
            print("   ⚠️ ERRO FATAL: O servidor C++ crashou ou não está rodando!")
            
        time.sleep(1.5)

if __name__ == "__main__":
    try:
        requests.post(f"{BASE_URL}/estado", json={"programa":1,"velocidade":0,"posicao":{"x":0,"y":0,"z":0},"status":"executando"})
        executar_demonstracao()
    except requests.exceptions.ConnectionError:
        print("\n[ERRO] O servidor da Central de Operação (C++) não está rodando.")
        print("Ligue o servidor com: .\\build\\Debug\\central_operacao.exe")