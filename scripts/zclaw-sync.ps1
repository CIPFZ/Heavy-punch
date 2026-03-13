param(
    [string]$RepoUrl = "https://github.com/tnm/zclaw.git",
    [string]$Ref = "main",
    [string]$Destination = "vendor/zclaw_upstream"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$destPath = Join-Path $repoRoot $Destination
$destParent = Split-Path $destPath -Parent

if (-not (Test-Path $destParent)) {
    New-Item -ItemType Directory -Path $destParent | Out-Null
}

if (-not (Test-Path $destPath)) {
    Write-Host "Cloning zclaw into $destPath"
    git clone --depth 1 --branch $Ref $RepoUrl $destPath
    exit $LASTEXITCODE
}

Write-Host "Updating zclaw in $destPath"
git -C $destPath fetch --depth 1 origin $Ref
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

git -C $destPath checkout FETCH_HEAD
exit $LASTEXITCODE
