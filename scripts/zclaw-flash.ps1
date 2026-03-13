param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [ValidateSet("esp32", "esp32c3", "esp32c6", "esp32s3")]
    [string]$Target = "esp32",
    [string]$ProjectPath = "vendor/zclaw_upstream",
    [switch]$BuildFirst,
    [switch]$SyncFirst,
    [int]$Baud = 460800,
    [switch]$Monitor
)

$ErrorActionPreference = "Stop"

function Resolve-EspIdfSetup {
    $candidates = @()

    if ($env:IDF_PATH) {
        $candidates += @{
            IdfPath = $env:IDF_PATH
            ToolsPath = $(if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { $null })
        }
    }

    $candidates += @(
        @{ IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.3"; ToolsPath = "C:\Espressif" },
        @{ IdfPath = "C:\Espressif\v5.5.2\esp-idf"; ToolsPath = "C:\Espressif" },
        @{ IdfPath = "C:\Espressif\v5.4\esp-idf"; ToolsPath = "C:\Espressif" },
        @{ IdfPath = (Join-Path $HOME "esp\esp-idf"); ToolsPath = (Join-Path $HOME "esp") },
        @{ IdfPath = (Join-Path $HOME "esp\esp-idf-v5.5.2"); ToolsPath = (Join-Path $HOME "esp") }
    )

    foreach ($candidate in $candidates) {
        $idfPath = $candidate.IdfPath
        $toolsPath = $candidate.ToolsPath
        if (-not $idfPath) {
            continue
        }

        $idfToolsPy = Join-Path $idfPath "tools\idf_tools.py"
        if (-not (Test-Path $idfToolsPy)) {
            continue
        }

        if (-not $toolsPath) {
            $toolsPath = Split-Path (Split-Path $idfPath -Parent) -Parent
            if (-not $toolsPath) {
                $toolsPath = Split-Path $idfPath -Parent
            }
        }

        $pythonCandidates = @(
            (Join-Path $toolsPath "python_env\idf5.5_py3.11_env\Scripts\python.exe"),
            (Join-Path $toolsPath "python_env\idf5.4_py3.11_env\Scripts\python.exe"),
            (Join-Path $toolsPath "python_env\idf5.5_py3.11_env\Scripts\python3.exe"),
            (Join-Path $toolsPath "python_env\idf5.4_py3.11_env\Scripts\python3.exe")
        )

        foreach ($pythonPath in $pythonCandidates) {
            if (Test-Path $pythonPath) {
                return @{
                    IdfPath = ((Resolve-Path $idfPath).Path).Trim()
                    ToolsPath = ((Resolve-Path $toolsPath).Path).Trim()
                    PythonPath = ((Resolve-Path $pythonPath).Path).Trim()
                }
            }
        }
    }

    throw "ESP-IDF not found. Install ESP-IDF first and make sure tools\\idf_tools.py and a python_env\\...\\Scripts\\python.exe exist."
}

function New-IdfCommand {
    param(
        [string]$PythonPath,
        [string]$IdfPath,
        [string]$ToolsPath,
        [string[]]$Arguments
    )

    $previousIdfPath = $env:IDF_PATH
    $previousToolsPath = $env:IDF_TOOLS_PATH
    $previousPythonEnvPath = $env:IDF_PYTHON_ENV_PATH

    $env:IDF_PATH = $IdfPath
    $env:IDF_TOOLS_PATH = $ToolsPath
    $env:IDF_PYTHON_ENV_PATH = Split-Path $PythonPath -Parent | Split-Path -Parent

    try {
        $exportRaw = & $PythonPath (Join-Path $IdfPath "tools\idf_tools.py") export --format key-value
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to export ESP-IDF environment."
        }
    }
    finally {
        $env:IDF_PATH = $previousIdfPath
        $env:IDF_TOOLS_PATH = $previousToolsPath
        $env:IDF_PYTHON_ENV_PATH = $previousPythonEnvPath
    }

    $envMap = @{}
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
        $envMap[$name] = $value
    }

    $envMap["IDF_PATH"] = $IdfPath.Trim()
    $envMap["IDF_TOOLS_PATH"] = $ToolsPath.Trim()
    $envMap["IDF_PYTHON_ENV_PATH"] = ((Split-Path $PythonPath -Parent | Split-Path -Parent)).Trim()

    $cmdParts = @()
    foreach ($kv in $envMap.GetEnumerator()) {
        $escaped = $kv.Value.Replace("^", "^^").Replace("&", "^&").Replace('"', '\"')
        $cmdParts += "set `"$($kv.Key)=$escaped`""
    }

    $idfPy = Join-Path $IdfPath "tools\idf.py"
    $argString = ($Arguments | ForEach-Object { $_ }) -join " "
    $cmdParts += "`"$PythonPath`" `"$idfPy`" $argString"
    return ($cmdParts -join " && ")
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$projectRoot = Join-Path $repoRoot $ProjectPath

if ($SyncFirst) {
    & (Join-Path $PSScriptRoot "zclaw-sync.ps1") -Destination $ProjectPath
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if ($BuildFirst) {
    & (Join-Path $PSScriptRoot "zclaw-build.ps1") -Target $Target -ProjectPath $ProjectPath
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if (-not (Test-Path $projectRoot)) {
    throw "zclaw project not found at $projectRoot. Run scripts\\zclaw-sync.ps1 first."
}

$idfSetup = Resolve-EspIdfSetup
$commands = @(
    (New-IdfCommand -PythonPath $idfSetup.PythonPath -IdfPath $idfSetup.IdfPath -ToolsPath $idfSetup.ToolsPath -Arguments @("set-target", $Target)),
    (New-IdfCommand -PythonPath $idfSetup.PythonPath -IdfPath $idfSetup.IdfPath -ToolsPath $idfSetup.ToolsPath -Arguments @("-p", $Port, "-b", "$Baud", "flash"))
)

if ($Monitor) {
    $commands += (New-IdfCommand -PythonPath $idfSetup.PythonPath -IdfPath $idfSetup.IdfPath -ToolsPath $idfSetup.ToolsPath -Arguments @("-p", $Port, "monitor"))
}

Write-Host "Flashing zclaw for $Target on $Port"
Write-Host "ESP-IDF: $($idfSetup.IdfPath)"
Write-Host "Baud: $Baud"
Push-Location $projectRoot
try {
    foreach ($command in $commands) {
        cmd /c $command
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
