$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
& .\build.bat build\X1-SYS-Island-optimized.exe
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
$version = (Get-Item -LiteralPath 'build/X1-SYS-Island-optimized.exe').VersionInfo.ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid executable product version' }
$release = Join-Path $PSScriptRoot "dist/X1-SYS-Island-v$version"
$archive = Join-Path $PSScriptRoot "dist/X1-SYS-Island-v$version.zip"
New-Item -ItemType Directory -Force -Path $release | Out-Null
Copy-Item -LiteralPath 'build/X1-SYS-Island-optimized.exe' -Destination (Join-Path $release 'X1-SYS-Island.exe')
Copy-Item -LiteralPath 'IntelMSR.bin','README.md','THIRD_PARTY_NOTICES.md' -Destination $release
Copy-Item -LiteralPath 'licenses' -Destination $release -Recurse -Force
Compress-Archive -Path "$release/*" -DestinationPath $archive -Force
Get-Item (Join-Path $release 'X1-SYS-Island.exe'), $archive | Select-Object FullName,Length

