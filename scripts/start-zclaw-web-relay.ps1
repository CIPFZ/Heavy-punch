param(
    [string]$Port = "COM5",
    [string]$BindHost = "0.0.0.0",
    [int]$HttpPort = 8787,
    [string]$ApiKeyFile = ".zclaw_web_api_key"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$workdir = Join-Path $repoRoot "vendor\zclaw_upstream"
$python = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$log = Join-Path $workdir "web-relay.log"
$err = Join-Path $workdir "web-relay.err.log"
$apiKeyPath = Join-Path $repoRoot $ApiKeyFile

if (-not (Test-Path $python)) {
    throw "Python runtime not found at $python"
}

if (-not (Test-Path $workdir)) {
    throw "zclaw upstream checkout not found at $workdir"
}

if (-not (Test-Path $apiKeyPath)) {
    $key = -join ((48..57 + 65..90 + 97..122) | Get-Random -Count 24 | ForEach-Object { [char]$_ })
    Set-Content -Path $apiKeyPath -Value $key -Encoding ascii
}

$key = (Get-Content $apiKeyPath -Raw).Trim()

if (Test-Path $log) { Remove-Item $log -Force }
if (Test-Path $err) { Remove-Item $err -Force }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $python
$psi.Arguments = "scripts/web_relay.py --host $BindHost --port $HttpPort --serial-port $Port"
$psi.WorkingDirectory = $workdir
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.Environment["ZCLAW_WEB_API_KEY"] = $key

$proc = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 2

if ($proc.HasExited) {
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    Set-Content -Path $log -Value $stdout -Encoding utf8
    Set-Content -Path $err -Value $stderr -Encoding utf8
    throw "web relay exited early with code $($proc.ExitCode)"
}

Write-Output "PID=$($proc.Id)"
Write-Output "API_KEY=$key"
Write-Output "LOG=$log"
Write-Output "ERR=$err"
