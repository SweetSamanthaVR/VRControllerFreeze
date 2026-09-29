[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'steamvr-paths.ps1')

if (Get-Process vrserver -ErrorAction SilentlyContinue) {
    throw "SteamVR is running. Exit SteamVR completely before installing $ProductName."
}

$driverRoot = Join-Path $AppRoot "driver\$DriverName"
$driverDll = Join-Path $driverRoot "bin\win64\driver_$DriverName.dll"
if (!(Test-Path -LiteralPath $driverDll)) { throw 'Built driver not found. Run build.cmd first.' }

$steamVR = Find-SteamVRPath
$vrPathReg = Join-Path $steamVR 'bin\win64\vrpathreg.exe'
if (!(Test-Path -LiteralPath $vrPathReg)) { throw "vrpathreg.exe not found at $vrPathReg" }
$steamRoot = Find-SteamRoot $steamVR
$configDir = Join-Path $steamRoot 'config'
$settingsPath = Join-Path $configDir 'steamvr.vrsettings'
New-Item -ItemType Directory -Force -Path $configDir | Out-Null

$backupPath = Backup-VRSettings $settingsPath

# Everything below is undone if any step fails.
$unregistered = New-Object System.Collections.Generic.List[string]
$registered = $false
try {
    # A copy of the driver registered from another folder would load alongside this one.
    foreach ($other in Get-RegisteredDriverCopies) {
        if (Test-SamePath $other $driverRoot) { continue }
        Write-Host "Unregistering another copy of the driver: $other" -ForegroundColor Yellow
        & $vrPathReg removedriver $other
        if ($LASTEXITCODE -ne 0) { throw "vrpathreg could not unregister $other." }
        $unregistered.Add($other)
    }

    Write-Host 'Registering the SteamVR freeze driver...' -ForegroundColor Cyan
    & $vrPathReg adddriver $driverRoot
    if ($LASTEXITCODE -ne 0) { throw "vrpathreg failed with exit code $LASTEXITCODE." }
    $registered = $true

    # SteamVR only loads a second driver alongside the headset's with activateMultipleDrivers,
    # and reads a driver's own settings from the driver_<name> section.
    $settings = Read-VRSettings $settingsPath
    $steamvrSettings = Initialize-ObjectProperty $settings 'steamvr'
    Set-NoteProperty $steamvrSettings 'activateMultipleDrivers' $true
    $driverSettings = Initialize-ObjectProperty $settings "driver_$DriverName"
    Set-NoteProperty $driverSettings 'enable' $true

    Write-VRSettingsAtomic $settings $settingsPath
    $null = Read-VRSettings $settingsPath
} catch {
    Write-Warning 'Installation failed. Rolling back all changes.'
    if ($registered) { & $vrPathReg removedriver $driverRoot | Out-Null }
    foreach ($other in $unregistered) { & $vrPathReg adddriver $other | Out-Null }
    if ($backupPath -and (Test-Path -LiteralPath $backupPath)) {
        Copy-Item -LiteralPath $backupPath -Destination $settingsPath -Force
    } elseif (-not $backupPath -and (Test-Path -LiteralPath $settingsPath)) {
        # There was no settings file before this install, so remove the one it created.
        Remove-Item -LiteralPath $settingsPath -Force
    }
    throw
}

Write-Host ''
Write-Host "$ProductName v$ProductVersion installed." -ForegroundColor Green
Write-Host "SteamVR: $steamVR"
Write-Host "Driver:  $driverRoot"
Write-Host "Config:  $settingsPath"
if ($backupPath) { Write-Host "Backup:  $backupPath" }
Write-Host ''
Write-Host 'Connect the Quest through Steam Link, then start VRControllerFreeze.exe. Keep the Live Log visible for the first test.' -ForegroundColor Yellow
