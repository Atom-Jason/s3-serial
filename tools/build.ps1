[CmdletBinding()]
param(
    [ValidateSet('build', 'reconfigure', 'menuconfig', 'size')]
    [string]$Action = 'build',
    [string]$IdfPath = $env:IDF_PATH,
    [string]$ToolsPath = $env:IDF_TOOLS_PATH,
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$localRoot = Join-Path $projectRoot '.local'

if (-not $IdfPath) { throw 'Activate ESP-IDF v5.5.1 first or pass -IdfPath and -ToolsPath.' }
if (-not $ToolsPath) { $ToolsPath = Join-Path $env:USERPROFILE '.espressif' }
if (-not $BuildDir) { $BuildDir = Join-Path $projectRoot 'build' }
$IdfPath = [IO.Path]::GetFullPath($IdfPath)
$ToolsPath = [IO.Path]::GetFullPath($ToolsPath)
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if (-not (Test-Path -LiteralPath (Join-Path $IdfPath 'tools\idf.py'))) {
    throw "ESP-IDF not found at '$IdfPath'. Install ESP-IDF v5.5.1 or pass -IdfPath and -ToolsPath."
}

# All environment changes are local to this process; restore the caller afterwards.
$previousEnvironment = @{}
foreach ($entry in Get-ChildItem Env:) { $previousEnvironment[$entry.Name] = $entry.Value }
try {
    $env:IDF_PATH = $IdfPath
    $env:IDF_TOOLS_PATH = $ToolsPath
    $env:IDF_TARGET = 'esp32s3'
    if (-not $env:PIP_CACHE_DIR) { $env:PIP_CACHE_DIR = Join-Path $localRoot 'pip-cache' }
    if (-not $env:IDF_COMPONENT_CACHE_PATH) { $env:IDF_COMPONENT_CACHE_PATH = Join-Path $localRoot 'component-cache' }
    if (-not $env:CCACHE_DIR) { $env:CCACHE_DIR = Join-Path $localRoot 'ccache' }
    . (Join-Path $IdfPath 'export.ps1')
    $pythonExe = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $pythonExe)) { throw 'The ESP-IDF Python environment is not ready.' }
    $version = & $pythonExe (Join-Path $IdfPath 'tools\idf.py') --version
    if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF version check failed.' }
    if ($version -notmatch 'ESP-IDF v5\.5\.1(?:\s|$)') { throw "Expected ESP-IDF v5.5.1; found '$version'." }
    Write-Host $version
    & $pythonExe (Join-Path $IdfPath 'tools\idf.py') -C $projectRoot -B $BuildDir $Action
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF '$Action' failed with exit code $LASTEXITCODE." }
} finally {
    foreach ($entry in Get-ChildItem Env:) {
        if (-not $previousEnvironment.ContainsKey($entry.Name)) { Remove-Item -LiteralPath "Env:$($entry.Name)" }
    }
    foreach ($entry in $previousEnvironment.GetEnumerator()) { Set-Item -LiteralPath "Env:$($entry.Key)" -Value $entry.Value }
}
