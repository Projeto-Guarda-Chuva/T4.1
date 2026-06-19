<#
==============================================================
 run_all.ps1  -  Compila, executa e expoe via ngrok todas as
 aplicacoes do T4.1, cada uma em uma porta diferente.

 Aplicacoes (porta):
   central_de_operacao.cpp          -> 8080  (cpp-httplib)
   atuador_central.cpp              -> 8090  (cpp-httplib)
   central_de_monitoramento.cpp     -> 8091  (cpp-httplib)
   programador_de_atuacao_local.cpp -> 8180  (winsock)
   viva_seu_video_local.cpp         -> 8190  (winsock + libcurl)

 (programador_de_atuacao.cpp e o sketch ESP32/Wokwi e roda no
  hardware/simulador, por isso nao entra aqui.)

 Pre-requisitos:
   - Um compilador C++17: g++ (MinGW-w64) ou cl (MSVC).
   - ngrok instalado e autenticado (ngrok config add-authtoken <token>)
     ou a variavel de ambiente NGROK_AUTHTOKEN definida.

 Uso:
   .\run_all.ps1                # baixa deps, compila, sobe tudo + ngrok
   .\run_all.ps1 -NoBuild       # nao recompila (usa binarios em .\build)
   .\run_all.ps1 -NoNgrok       # sobe os apps sem abrir tuneis ngrok
   .\run_all.ps1 -Only central-operacao,atuador-central
==============================================================
#>
[CmdletBinding()]
param(
    [switch]$NoBuild,
    [switch]$NoNgrok,
    [string[]]$Only
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = `
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Root  = $PSScriptRoot
$Deps  = Join-Path $Root 'deps'
$Build = Join-Path $Root 'build'
$Logs  = Join-Path $Root 'logs'
$OnWindows = -not ($IsLinux -or $IsMacOS)   # em PS 5.1 essas vars sao nulas

# --- Catalogo de aplicacoes -------------------------------
$Apps = @(
    [pscustomobject]@{ Name='central-operacao';      Src='central_de_operacao.cpp';          Port=8080; Kind='httplib' }
    [pscustomobject]@{ Name='atuador-central';       Src='atuador_central.cpp';              Port=8090; Kind='httplib' }
    [pscustomobject]@{ Name='central-monitoramento'; Src='central_de_monitoramento.cpp';     Port=8091; Kind='httplib' }
    [pscustomobject]@{ Name='programador-atuacao';   Src='programador_de_atuacao_local.cpp'; Port=8180; Kind='winsock' }
    [pscustomobject]@{ Name='viva-seu-video';        Src='viva_seu_video_local.cpp';         Port=8190; Kind='curl' }
)
if ($Only) { $Apps = $Apps | Where-Object { $Only -contains $_.Name } }
if (-not $Apps) { throw "Nenhuma aplicacao selecionada (verifique -Only)." }

New-Item -ItemType Directory -Force -Path $Deps,$Build,$Logs | Out-Null

# --- 1. Dependencias header-only (cpp-httplib + nlohmann/json) ---
function Ensure-Header($url, $dest) {
    if (Test-Path $dest) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    Write-Host "[deps] baixando $(Split-Path $dest -Leaf)..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
}

if (-not $NoBuild -and ($Apps.Kind -contains 'httplib')) {
    Ensure-Header 'https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h' `
                  (Join-Path $Deps 'httplib.h')
    Ensure-Header 'https://github.com/nlohmann/json/releases/latest/download/json.hpp' `
                  (Join-Path $Deps 'nlohmann\json.hpp')
}

# --- 2. Compilador ----------------------------------------

# Localiza o vcvars64.bat de uma instalacao do Visual Studio / Build Tools.
function Find-VcVars {
    $vsw = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vsw) {
        $base = & $vsw -latest -products * `
                       -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                       -property installationPath 2>$null
        if ($base) {
            $vc = Join-Path $base 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $vc) { return $vc }
        }
    }
    foreach ($r in @("${env:ProgramFiles}\Microsoft Visual Studio",
                     "${env:ProgramFiles(x86)}\Microsoft Visual Studio")) {
        if (-not (Test-Path $r)) { continue }
        $hit = Get-ChildItem $r -Recurse -Filter 'vcvars64.bat' -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

# Executa o vcvars64.bat e importa as variaveis de ambiente (PATH, INCLUDE,
# LIB, ...) para esta sessao, deixando cl.exe utilizavel.
function Import-VcVars($vcvars) {
    Write-Host "[build] inicializando ambiente MSVC..." -ForegroundColor Cyan
    $out = cmd /c "`"$vcvars`" && set"
    foreach ($line in $out) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
        }
    }
}

function Find-Compiler {
    $gpp = Get-Command g++ -ErrorAction SilentlyContinue
    if ($gpp) { return [pscustomobject]@{ Type='gpp'; Exe=$gpp.Source } }

    $cl = Get-Command cl -ErrorAction SilentlyContinue
    if (-not $cl) {
        $vc = Find-VcVars
        if ($vc) { Import-VcVars $vc; $cl = Get-Command cl -ErrorAction SilentlyContinue }
    }
    if ($cl) { return [pscustomobject]@{ Type='cl'; Exe=$cl.Source } }
    return $null
}

function Build-App($app, $cc) {
    $src = Join-Path $Root $app.Src
    $exe = Join-Path $Build "$($app.Name).exe"
    if (-not (Test-Path $src)) { Write-Warning "fonte ausente: $($app.Src)"; return $null }

    Write-Host "[build] $($app.Name) <- $($app.Src)" -ForegroundColor Yellow
    if ($cc.Type -eq 'gpp') {
        $ccArgs = @('-std=c++17','-O2','-pthread')
        # cpp-httplib exige API do Windows 10 (CreateFile2); MinGW assume um
        # alvo mais antigo por padrao, entao fixamos _WIN32_WINNT = 0x0A00.
        if ($OnWindows) { $ccArgs += '-D_WIN32_WINNT=0x0A00' }
        if ($app.Kind -eq 'httplib') { $ccArgs += @('-I', $Deps) }
        $ccArgs += @($src, '-o', $exe)
        if ($OnWindows) { $ccArgs += '-lws2_32' }
        if ($app.Kind -eq 'curl') { $ccArgs += '-lcurl' }
    }
    else {  # MSVC cl
        if ($app.Kind -eq 'curl') {
            Write-Warning "$($app.Name) requer libcurl, indisponivel no MSVC sem configuracao extra; pulando."
            return $null
        }
        $ccArgs = @('/nologo','/std:c++17','/EHsc','/O2','/Zc:__cplusplus',
                    '/D_CRT_SECURE_NO_WARNINGS', '/D_WIN32_WINNT=0x0A00', "/Fo$Build\", "/Fe$exe")
        if ($app.Kind -eq 'httplib') { $ccArgs += "/I$Deps" }
        $ccArgs += $src
    }
    # Captura a saida do compilador (senao ela "vaza" para o valor de retorno
    # da funcao, transformando $exe em um array e quebrando o Start-Process).
    $buildLog = & $cc.Exe @ccArgs
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        Write-Warning "falha ao compilar $($app.Name) (exit=$LASTEXITCODE)"
        if ($buildLog) { $buildLog | Select-Object -Last 12 | ForEach-Object { Write-Host "    $_" } }
        return $null
    }
    return $exe
}

if (-not $NoBuild) {
    $cc = Find-Compiler
    if (-not $cc) {
        throw "Nenhum compilador C++ encontrado (g++ ou cl). Instale MinGW-w64 ou abra um 'Developer Command Prompt' do Visual Studio."
    }
    Write-Host "[build] usando compilador: $($cc.Type) ($($cc.Exe))" -ForegroundColor Cyan
    foreach ($app in $Apps) {
        $exe = Build-App $app $cc
        $app | Add-Member -NotePropertyName Exe -NotePropertyValue $exe -Force
    }
} else {
    foreach ($app in $Apps) {
        $app | Add-Member -NotePropertyName Exe `
               -NotePropertyValue (Join-Path $Build "$($app.Name).exe") -Force
    }
}

# --- 3. Sobe os processos das aplicacoes ------------------
$Procs = @()
function Stop-All {
    foreach ($p in $Procs) {
        if ($p -and -not $p.HasExited) {
            try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch {}
        }
    }
    Get-Process ngrok -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

try {
    foreach ($app in $Apps) {
        if (-not $app.Exe -or -not (Test-Path $app.Exe)) {
            Write-Warning "pulando $($app.Name): binario nao disponivel"
            continue
        }
        $out = Join-Path $Logs "$($app.Name).out.log"
        $err = Join-Path $Logs "$($app.Name).err.log"
        $p = Start-Process -FilePath $app.Exe -PassThru -WindowStyle Hidden `
                           -RedirectStandardOutput $out -RedirectStandardError $err
        $app | Add-Member -NotePropertyName Proc -NotePropertyValue $p -Force
        $Procs += $p
        Write-Host ("[run]  {0,-22} http://localhost:{1}  (pid {2})" -f $app.Name,$app.Port,$p.Id) -ForegroundColor Green
    }

    if (-not $Procs) { throw "Nenhuma aplicacao foi iniciada." }

    if ($NoNgrok) {
        Write-Host "`n[ok] Apps no ar (sem ngrok). Ctrl+C encerra tudo." -ForegroundColor Cyan
        while ($true) { Start-Sleep -Seconds 1 }
    }

    # --- 4. Gera ngrok.yml (1 tunel por porta) e sobe o ngrok ---
    $ngrok = Get-Command ngrok -ErrorAction SilentlyContinue
    if (-not $ngrok) { throw "ngrok nao encontrado no PATH. Instale em https://ngrok.com/download" }

    $yml = Join-Path $Root 'ngrok.yml'
    $sb  = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine('version: "2"')
    if ($env:NGROK_AUTHTOKEN) { [void]$sb.AppendLine("authtoken: $($env:NGROK_AUTHTOKEN)") }
    [void]$sb.AppendLine('tunnels:')
    foreach ($app in ($Apps | Where-Object { $_.Proc })) {
        [void]$sb.AppendLine("  $($app.Name):")
        [void]$sb.AppendLine("    proto: http")
        [void]$sb.AppendLine("    addr: $($app.Port)")
    }
    Set-Content -Path $yml -Value $sb.ToString() -Encoding ascii

    # Inclui tambem o config padrao do ngrok (que costuma guardar o authtoken)
    $defaultCfg = Join-Path $env:LOCALAPPDATA 'ngrok\ngrok.yml'
    $cfgArgs = @('start','--all','--config', $yml)
    if (Test-Path $defaultCfg) { $cfgArgs += @('--config', $defaultCfg) }
    elseif (-not $env:NGROK_AUTHTOKEN) {
        Write-Warning "Sem authtoken: rode 'ngrok config add-authtoken <token>' ou defina `$env:NGROK_AUTHTOKEN."
    }

    Write-Host "`n[ngrok] iniciando tuneis..." -ForegroundColor Cyan
    $ng = Start-Process -FilePath $ngrok.Source -ArgumentList $cfgArgs -PassThru `
                        -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Logs 'ngrok.log') `
                        -RedirectStandardError (Join-Path $Logs 'ngrok.err.log')
    $Procs += $ng

    # --- 5. Consulta a API local do ngrok e imprime as URLs publicas ---
    $tunnels = $null
    for ($i = 0; $i -lt 20 -and -not $tunnels; $i++) {
        Start-Sleep -Milliseconds 500
        try {
            $resp = Invoke-RestMethod -Uri 'http://127.0.0.1:4040/api/tunnels' -ErrorAction Stop
            if ($resp.tunnels.Count -gt 0) { $tunnels = $resp.tunnels }
        } catch {}
    }

    Write-Host "`n================  TUNEIS PUBLICOS  ================" -ForegroundColor Magenta
    if ($tunnels) {
        foreach ($app in ($Apps | Where-Object { $_.Proc })) {
            $needle = "*:$($app.Port)"
            $t = $tunnels | Where-Object { $_.config.addr -like $needle -and $_.proto -eq 'https' } | Select-Object -First 1
            if (-not $t) { $t = $tunnels | Where-Object { $_.config.addr -like $needle } | Select-Object -First 1 }
            $url = if ($t) { $t.public_url } else { '(nao publicado)' }
            Write-Host ("  {0,-22} {1,-7} -> {2}" -f $app.Name, $app.Port, $url)
        }
    } else {
        Write-Warning "Nao foi possivel ler as URLs em http://127.0.0.1:4040 - veja logs\ngrok.err.log"
    }
    Write-Host "==================================================" -ForegroundColor Magenta
    Write-Host "Painel ngrok: http://127.0.0.1:4040   |   Ctrl+C encerra tudo.`n"

    Wait-Process -Id $ng.Id
}
finally {
    Write-Host "`n[cleanup] encerrando aplicacoes..." -ForegroundColor Cyan
    Stop-All
}
