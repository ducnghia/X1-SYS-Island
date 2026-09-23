$ErrorActionPreference = 'Stop'
Start-Transcript -Path (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/verified-release.log') -Force
try {
  & (Join-Path $PSScriptRoot 'test.bat') --require-temperature
  if ($LASTEXITCODE -ne 0) { throw 'Direct temperature validation failed' }
  & (Join-Path $PSScriptRoot 'rebuild-running.ps1')
  & (Join-Path $PSScriptRoot 'test-hover.bat')
  if ($LASTEXITCODE -ne 0) { throw 'Hover timer validation failed' }
} finally {
  Stop-Transcript
}
