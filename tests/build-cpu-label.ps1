$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
Start-Transcript -Path (Join-Path $root 'build/cpu-label-validation.log') -Force
try {
    & .\build.bat build\X1-SYS-Island-cpu-label.exe
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    & .\tests\test.bat
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    Write-Output 'PASS: CPU label build and diagnostics'
} finally {
    Stop-Transcript
}
