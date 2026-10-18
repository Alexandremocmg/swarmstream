# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
<#
.SYNOPSIS
  Mede a carga a frio do mapa de teste logo apos reiniciar o Windows (F0-12).

.DESCRIPTION
  A carga so e "fria" se os arquivos nao estiverem no cache de arquivos do Windows. Ler pastas
  nao relacionadas nao garante isso (a tentativa anterior falhou); reiniciar garante.

  Fluxo:
    1. -Arm      registra UMA execucao no proximo logon (HKCU RunOnce; some sozinha depois de rodar).
    2. reinicie  (manual, ou use -Reboot junto com -Arm).
    3. -Run      roda sozinho no logon: espera -SettleSeconds (para a inicializacao assentar), executa a
                 PRIMEIRA rodada (fria) e depois -Runs rodadas quentes. Grava JSON e log em docs/medicoes.

  Sem -Arm, voce pode chamar -Run a mao logo apos reiniciar; o JSON guarda quantos segundos se
  passaram desde o boot, e e isso que prova que a carga foi a primeira.

.EXAMPLE
  .\cold_boot.ps1 -Arm -Godot C:\...\Godot_v4.7.2-stable_win64_console.exe -Python C:\...\python.exe
#>
param(
    [switch]$Arm,
    [switch]$Run,
    [switch]$Reboot,
    [Parameter(Mandatory = $true)][string]$Godot,
    [Parameter(Mandatory = $true)][string]$Python,
    [int]$SettleSeconds = 120,
    [int]$RouteSeconds = 60,
    [int]$Runs = 3,
    [ValidateSet('gpu', 'jpg')][string]$Tex = 'gpu'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out = Join-Path $repo "docs\medicoes\baseline-cold-boot-$Tex.json"
$log = Join-Path $repo "docs\medicoes\baseline-cold-boot-$Tex.log"

if ($Arm) {
    $cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Run " +
        "-Godot `"$Godot`" -Python `"$Python`" -SettleSeconds $SettleSeconds -RouteSeconds $RouteSeconds " +
        "-Runs $Runs -Tex $Tex"
    $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
    Set-ItemProperty -Path $key -Name 'SwarmstreamColdBoot' -Value $cmd
    Write-Host "Armado (RunOnce do usuario). Proximo logon executa a medicao fria."
    if ($Reboot) {
        Write-Host 'Reiniciando em 15 s...'
        Start-Sleep -Seconds 15
        Restart-Computer -Force
    }
    return
}

if ($Run) {
    "inicio $(Get-Date -Format o); aguardando $SettleSeconds s para a inicializacao assentar" | Out-File $log -Encoding utf8
    Start-Sleep -Seconds $SettleSeconds
    & $Python -I (Join-Path $repo 'sw-godot\tools\baseline.py') `
        --godot $Godot --tex $Tex --route-seconds $RouteSeconds --runs $Runs --after-boot --out $out *>> $log
    "fim $(Get-Date -Format o) codigo=$LASTEXITCODE" | Out-File $log -Append -Encoding utf8
    return
}

Write-Host 'Use -Arm (e depois reinicie) ou -Run.'
