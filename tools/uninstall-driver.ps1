[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'steamvr-paths.ps1')

if (Get-Process vrserver -ErrorAction SilentlyContinue) {
    throw "SteamVR is running. Exit SteamVR completely before uninstalling $ProductName."
}

$root = Split-Path -Parent $PSScriptRoot
$driverRoot = Join-Path $root "build\dist\driver\$DriverName"
$steamVR = Find-SteamVRPath
$vrPathReg = Join-Path $steamVR 'bin\win64\vrpathreg.exe'
$steamRoot = Find-SteamRoot $steamVR
$settingsPath = Join-Path (Join-Path $steamRoot 'config') 'steamvr.vrsettings'

# Every registered copy of the driver, plus this folder's build even if it is not registered.
$registeredDrivers = New-Object System.Collections.Generic.List[string]
foreach ($driver in Get-RegisteredDriverCopies) { $registeredDrivers.Add($driver) }
if ((Test-Path -LiteralPath $driverRoot) -and -not ($registeredDrivers | Where-Object { Test-SamePath $_ $driverRoot })) {
    $registeredDrivers.Add($driverRoot)
}
foreach ($driver in $registeredDrivers) {
    Write-Host "Unregistering the SteamVR freeze driver: $driver" -ForegroundColor Cyan
    & $vrPathReg removedriver $driver
    if ($LASTEXITCODE -ne 0) { Write-Warning 'vrpathreg returned an error while unregistering. Configuration cleanup will continue.' }
}

# Remove the driver's settings section, backing up first and only if there is something to remove.
if (Test-Path -LiteralPath $settingsPath) {
    $settings = Read-VRSettings $settingsPath
    $section = "driver_$DriverName"
    if ($null -ne $settings.PSObject.Properties[$section]) {
        $backupPath = Backup-VRSettings $settingsPath
        $settings.PSObject.Properties.Remove($section)
        Write-VRSettingsAtomic $settings $settingsPath
        Write-Host "Settings backup: $backupPath"
    }
}

# The app's own preferences (the dark/light choice).
$preferencesKey = 'HKCU:\Software\VRControllerFreeze'
if (Test-Path -LiteralPath $preferencesKey) {
    Remove-Item -LiteralPath $preferencesKey -Recurse -Force
    Write-Host 'App preferences removed.'
}

Write-Host "$ProductName driver, its SteamVR settings and the app's preferences removed." -ForegroundColor Green
Write-Host 'activateMultipleDrivers is left unchanged because another SteamVR driver may rely on it.'
