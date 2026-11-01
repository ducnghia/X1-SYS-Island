$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $PSScriptRoot 'X1-SYS-Island.exe'
$target = Join-Path $root 'X1-SYS-Island.exe'
$log = Join-Path $PSScriptRoot 'replace-running.log'

try {
    $running = @(Get-CimInstance Win32_Process -Filter "Name='X1-SYS-Island.exe'")
    $paths = @($running | ForEach-Object { if (-not $_.ExecutablePath) { throw 'Cannot determine running app path' }; $_.ExecutablePath } | Select-Object -Unique)

    # Use our own project path as the target when multiple locations exist, or fall back to discovered location
    $projectTarget = Join-Path (Split-Path $PSScriptRoot -Parent) 'X1-SYS-Island.exe'

    if ($paths.Count -gt 1) { throw 'Multiple running app locations; replacement cancelled' }
    if ($paths.Count -eq 1) { $target = $paths[0] } else { $target = $projectTarget }

    if (-not (Test-Path -LiteralPath $source)) { throw 'Built executable not found' }
    foreach ($app in $running) { Stop-Process -Id $app.ProcessId -Force; Wait-Process -Id $app.ProcessId -ErrorAction SilentlyContinue }
    Copy-Item -LiteralPath $source -Destination $target -Force

    # Also copy to project directory so it is always up-to-date
    if ($target -ne $projectTarget) { Copy-Item -LiteralPath $source -Destination $projectTarget -Force }

    Start-Process -FilePath $target -WorkingDirectory (Split-Path $target -Parent)

    "SUCCESS: $target" | Set-Content -LiteralPath $log
} catch {
    $_ | Out-String | Set-Content -LiteralPath $log
    exit 1
}
