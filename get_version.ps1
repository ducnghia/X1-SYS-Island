$ver = (Get-Item 'X1-SYS-Island.exe').VersionInfo
Write-Output "File Version: $($ver.FileVersion)"
Write-Output "Product Version: $($ver.ProductVersion)"
