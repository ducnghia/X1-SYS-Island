$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
& .\build.bat build\X1-SYS-Island-optimized.exe
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
$release = Join-Path $PSScriptRoot 'dist/X1-SYS-Island-v0.6.3'
New-Item -ItemType Directory -Force -Path $release | Out-Null
Copy-Item -LiteralPath 'build/X1-SYS-Island-optimized.exe' -Destination (Join-Path $release 'X1-SYS-Island.exe')
Copy-Item -LiteralPath 'IntelMSR.bin','README.md','THIRD_PARTY_NOTICES.md' -Destination $release
Copy-Item -LiteralPath 'licenses' -Destination $release -Recurse -Force
Compress-Archive -Path "$release/*" -DestinationPath (Join-Path $PSScriptRoot 'dist/X1-SYS-Island-v0.6.3.zip') -Force
Get-Item (Join-Path $release 'X1-SYS-Island.exe'), (Join-Path $PSScriptRoot 'dist/X1-SYS-Island-v0.6.3.zip') | Select-Object FullName,Length

