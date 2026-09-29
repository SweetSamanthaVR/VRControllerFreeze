# Shared names and helpers for the build, install, uninstall and diagnostics scripts, which dot-source
# this file; the names below are used there rather than here.
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseDeclaredVarsMoreThanAssignments', '', Justification = 'Used by the scripts that dot-source this file.')]
param()

$ProductName = "VR Controller Freeze"
# SteamVR driver name: the driver folder, driver_<name>.dll and the driver_<name> settings section.
$DriverName = 'vrcontrollerfreeze'
# CMakeLists.txt is the single source of the version number.
$ProductVersion = (Select-String -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'CMakeLists.txt') -Pattern 'VERSION\s+(\d+\.\d+\.\d+)' | Select-Object -First 1).Matches[0].Groups[1].Value

function Find-SteamVRPath {
    $candidates = New-Object System.Collections.Generic.List[string]

    if ($env:STEAMVR_PATH) { $candidates.Add($env:STEAMVR_PATH) }

    try {
        $uninstall = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 250820' -ErrorAction Stop
        if ($uninstall.InstallLocation) { $candidates.Add($uninstall.InstallLocation) }
    } catch {}
    try {
        $uninstall32 = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 250820' -ErrorAction Stop
        if ($uninstall32.InstallLocation) { $candidates.Add($uninstall32.InstallLocation) }
    } catch {}

    try {
        $steam = Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop
        if ($steam.SteamPath) { $candidates.Add((Join-Path $steam.SteamPath 'steamapps\common\SteamVR')) }
    } catch {}

    $candidates.Add('C:\Program Files (x86)\Steam\steamapps\common\SteamVR')
    $candidates.Add('C:\Program Files\Steam\steamapps\common\SteamVR')

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $expanded = [Environment]::ExpandEnvironmentVariables($candidate)
        if (Test-Path -LiteralPath (Join-Path $expanded 'bin\win64\vrpathreg.exe')) {
            return (Resolve-Path -LiteralPath $expanded).Path
        }
    }

    throw 'SteamVR could not be located. Set STEAMVR_PATH to your SteamVR install folder and retry.'
}

function Find-SteamRoot([string]$SteamVRPath) {
    if ($env:STEAM_PATH -and (Test-Path -LiteralPath $env:STEAM_PATH)) {
        return (Resolve-Path -LiteralPath $env:STEAM_PATH).Path
    }

    try {
        $steam = Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop
        if ($steam.SteamPath -and (Test-Path -LiteralPath $steam.SteamPath)) {
            return (Resolve-Path -LiteralPath $steam.SteamPath).Path
        }
    } catch {}

    # Fallback for a default/single-library install. If SteamVR lives in a secondary
    # library, set STEAM_PATH explicitly if the Steam registry entry is unavailable.
    $common = Split-Path -Parent $SteamVRPath
    $steamApps = Split-Path -Parent $common
    return (Split-Path -Parent $steamApps)
}

# True if both paths name the same folder, ignoring case and trailing separators.
function Test-SamePath([string]$A, [string]$B) {
    return [IO.Path]::GetFullPath($A).TrimEnd('\', '/') -ieq [IO.Path]::GetFullPath($B).TrimEnd('\', '/')
}

function Read-VRSettings([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return [pscustomobject]@{} }
    $raw = Get-Content -Raw -LiteralPath $Path
    if ([string]::IsNullOrWhiteSpace($raw)) { return [pscustomobject]@{} }
    return ($raw | ConvertFrom-Json)
}

# Copies steamvr.vrsettings to a timestamped backup beside it after checking it parses, keeping
# only the newest $Keep backups.
# Returns the backup path, or $null if there is no settings file yet.
function Backup-VRSettings([string]$Path, [int]$Keep = 5) {
    if (!(Test-Path -LiteralPath $Path)) { return $null }
    $null = Read-VRSettings $Path
    $prefix = "$(Split-Path -Leaf $Path).$DriverName-backup-"
    $backupPath = Join-Path (Split-Path -Parent $Path) "$prefix$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    Copy-Item -LiteralPath $Path -Destination $backupPath
    # Keep only the newest backups; the timestamp in the name sorts oldest to newest.
    Get-ChildItem -LiteralPath (Split-Path -Parent $Path) -File |
        Where-Object { $_.Name.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) } |
        Sort-Object Name -Descending |
        Select-Object -Skip $Keep |
        Remove-Item -Force
    return $backupPath
}

function Initialize-ObjectProperty($Object, [string]$Name) {
    if ($null -eq $Object.PSObject.Properties[$Name]) {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue ([pscustomobject]@{})
    }
    return $Object.$Name
}

function Set-NoteProperty($Object, [string]$Name, $Value) {
    if ($null -eq $Object.PSObject.Properties[$Name]) {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    } else {
        $Object.$Name = $Value
    }
}

function Remove-NoteProperty($Object, [string]$Name) {
    if ($null -ne $Object.PSObject.Properties[$Name]) {
        $Object.PSObject.Properties.Remove($Name)
    }
}

# Writes to a temporary file, checks it parses, then replaces the target in one move.
function Write-VRSettingsAtomic($Settings, [string]$Path) {
    $json = $Settings | ConvertTo-Json -Depth 100
    $encoding = New-Object System.Text.UTF8Encoding($false)
    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $temp = Join-Path $directory ('.vrcontrollerfreeze-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temp, $json + [Environment]::NewLine, $encoding)
        $null = Read-VRSettings $temp
        Move-Item -LiteralPath $temp -Destination $Path -Force
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
    }
}

# Returns every external driver path registered with SteamVR whose manifest names it $DriverName
# (or whose folder has that name if the manifest has since been deleted).
function Get-RegisteredDriverCopies {
    $vrpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (!(Test-Path -LiteralPath $vrpath)) { return @() }
    $paths = Get-Content -Raw -LiteralPath $vrpath | ConvertFrom-Json
    if ($null -eq $paths.PSObject.Properties['external_drivers']) { return @() }
    $found = @()
    foreach ($driver in @($paths.external_drivers)) {
        if ([string]::IsNullOrWhiteSpace($driver)) { continue }
        $manifest = Join-Path $driver 'driver.vrdrivermanifest'
        $name = $null
        if (Test-Path -LiteralPath $manifest) {
            try { $name = (Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json).name } catch {}
        } else {
            $name = Split-Path -Leaf $driver
        }
        if ($name -eq $DriverName) { $found += $driver }
    }
    return $found
}
