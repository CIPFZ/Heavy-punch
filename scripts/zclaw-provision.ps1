param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [Parameter(Mandatory = $true)]
    [string]$WifiSsid,
    [string]$WifiPassword = "",
    [ValidateSet("anthropic", "openai", "openrouter", "ollama")]
    [string]$Backend = "openai",
    [Parameter(Mandatory = $true)]
    [string]$ApiKey,
    [string]$Model = "gpt-5.4",
    [string]$ApiUrl = "",
    [string]$ProjectPath = "vendor/zclaw_upstream"
)

$ErrorActionPreference = "Stop"

function Resolve-EspIdfSetup {
    $candidates = @(
        @{ IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.3"; ToolsPath = "C:\Espressif" },
        @{ IdfPath = "C:\Espressif\v5.5.2\esp-idf"; ToolsPath = "C:\Espressif" },
        @{ IdfPath = "C:\Espressif\v5.4\esp-idf"; ToolsPath = "C:\Espressif" }
    )

    if ($env:IDF_PATH) {
        $candidates = ,@{
            IdfPath = $env:IDF_PATH
            ToolsPath = $(if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { "C:\Espressif" })
        } + $candidates
    }

    foreach ($candidate in $candidates) {
        $idfPath = $candidate.IdfPath
        $toolsPath = $candidate.ToolsPath
        if (-not $idfPath) {
            continue
        }

        $nvsGen = Join-Path $idfPath "components\nvs_flash\nvs_partition_generator\nvs_partition_gen.py"
        $parttool = Join-Path $idfPath "components\partition_table\parttool.py"
        $pythonPath = Join-Path $toolsPath "python_env\idf5.5_py3.11_env\Scripts\python.exe"

        if ((Test-Path $nvsGen) -and (Test-Path $parttool) -and (Test-Path $pythonPath)) {
            return @{
                IdfPath = ((Resolve-Path $idfPath).Path).Trim()
                ToolsPath = ((Resolve-Path $toolsPath).Path).Trim()
                PythonPath = ((Resolve-Path $pythonPath).Path).Trim()
                NvsGenPath = ((Resolve-Path $nvsGen).Path).Trim()
                ParttoolPath = ((Resolve-Path $parttool).Path).Trim()
            }
        }
    }

    throw "ESP-IDF setup not found under C:\Espressif."
}

function Set-IdfEnv {
    param(
        [string]$IdfPath,
        [string]$ToolsPath,
        [string]$PythonPath
    )

    $env:IDF_PATH = $IdfPath
    $env:IDF_TOOLS_PATH = $ToolsPath
    $env:IDF_PYTHON_ENV_PATH = (Split-Path $PythonPath -Parent | Split-Path -Parent).Trim()

    $exportRaw = & $PythonPath (Join-Path $IdfPath "tools\idf_tools.py") export --format key-value
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to export ESP-IDF environment."
    }

    foreach ($line in $exportRaw) {
        if ($line -notmatch "=") {
            continue
        }
        $name, $value = $line -split "=", 2
        $name = $name.Trim()
        $value = $value.Trim()
        if ($name -eq "PATH") {
            $value = $value -replace "%PATH%", $env:Path
        }
        Set-Item -Path "Env:$name" -Value $value
    }
}

function CsvEscape {
    param([string]$Value)

    $escaped = $Value.Replace('"', '""').Replace("`r", " ").Replace("`n", " ")
    return '"' + $escaped + '"'
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$projectRoot = Join-Path $repoRoot $ProjectPath
if (-not (Test-Path $projectRoot)) {
    throw "zclaw project not found at $projectRoot."
}

if (-not $Model) {
    $Model = switch ($Backend) {
        "openai" { "gpt-5.4" }
        "openrouter" { "openrouter/auto" }
        "anthropic" { "claude-sonnet-4-6" }
        "ollama" { "qwen3:8b" }
    }
}

$idfSetup = Resolve-EspIdfSetup
Set-IdfEnv -IdfPath $idfSetup.IdfPath -ToolsPath $idfSetup.ToolsPath -PythonPath $idfSetup.PythonPath

$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("zclaw-provision-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmpDir | Out-Null

try {
    $csvPath = Join-Path $tmpDir "nvs.csv"
    $binPath = Join-Path $tmpDir "nvs.bin"

    $rows = @(
        "key,type,encoding,value",
        "zclaw,namespace,,",
        "boot_count,data,string,""0""",
        "wifi_ssid,data,string,$(CsvEscape $WifiSsid)",
        "wifi_pass,data,string,$(CsvEscape $WifiPassword)",
        "llm_backend,data,string,$(CsvEscape $Backend)",
        "api_key,data,string,$(CsvEscape $ApiKey)",
        "llm_model,data,string,$(CsvEscape $Model)"
    )

    if ($ApiUrl) {
        $rows += "llm_api_url,data,string,$(CsvEscape $ApiUrl)"
    }

    Set-Content -Path $csvPath -Value $rows -Encoding ascii

    & $idfSetup.PythonPath $idfSetup.NvsGenPath generate $csvPath $binPath 0x4000
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to generate NVS image."
    }

    Push-Location $projectRoot
    try {
        & $idfSetup.PythonPath $idfSetup.ParttoolPath --port $Port write_partition --partition-name nvs --input $binPath
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to write NVS partition."
        }
    }
    finally {
        Pop-Location
    }

    Write-Host "Provisioning complete on $Port"
    Write-Host "Wi-Fi SSID: $WifiSsid"
    Write-Host "Backend: $Backend"
    Write-Host "Model: $Model"
    if ($ApiUrl) {
        Write-Host "API URL: $ApiUrl"
    }
}
finally {
    if (Test-Path $tmpDir) {
        Remove-Item -Recurse -Force $tmpDir
    }
}
