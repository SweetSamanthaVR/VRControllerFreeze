[CmdletBinding()]
param(
    [string]$Output = (Join-Path (Get-Location) 'VRControllerFreeze-diagnostics.txt')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'steamvr-paths.ps1')
$steamVR = Find-SteamVRPath
$steamRoot = Find-SteamRoot $steamVR
$settingsPath = Join-Path (Join-Path $steamRoot 'config') 'steamvr.vrsettings'
$logDirs = @((Join-Path $steamRoot 'logs'), (Join-Path $steamVR 'logs'))
$appLogDir = Join-Path $env:LOCALAPPDATA 'VRControllerFreeze\logs'

# The report is meant to be posted publicly, so user folder names (in any path form, including
# 8.3 short names and JSON's doubled backslashes), the Windows user name and the computer name are
# replaced with placeholders.
function Remove-PrivateDetails([string]$Text) {
    $Text = $Text -replace '(?i)([\\/]+Users[\\/]+)[^\\/"''\r\n]+', '$1<user>'
    if ($env:USERPROFILE) {
        $Text = $Text -ireplace [regex]::Escape($env:USERPROFILE), '<user folder>'
        $Text = $Text -ireplace [regex]::Escape($env:USERPROFILE.Replace('\', '\\')), '<user folder>'
    }
    if ($env:USERNAME) { $Text = $Text -ireplace "\b$([regex]::Escape($env:USERNAME))\b", '<user>' }
    if ($env:COMPUTERNAME) { $Text = $Text -ireplace "\b$([regex]::Escape($env:COMPUTERNAME))\b", '<computer>' }
    return $Text
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("$ProductName v$ProductVersion diagnostics - $(Get-Date -Format o)")
$lines.Add("SteamVR: $steamVR")
$lines.Add('')

$lines.Add('=== Registered copies of the driver (openvrpaths.vrpath) ===')
$registered = @(Get-RegisteredDriverCopies)
if ($registered.Count -eq 0) { $lines.Add('None registered.') }
foreach ($driver in $registered) { $lines.Add($driver) }
if ($registered.Count -gt 1) { $lines.Add('WARNING: more than one copy of the driver is registered. Run install-driver.cmd to fix.') }
$lines.Add('')

$lines.Add('=== Relevant steamvr.vrsettings ===')
if (Test-Path -LiteralPath $settingsPath) {
    $settings = Read-VRSettings $settingsPath
    $subset = [ordered]@{}
    if ($settings.PSObject.Properties['steamvr']) { $subset.steamvr = $settings.steamvr }
    $section = "driver_$DriverName"
    if ($settings.PSObject.Properties[$section]) { $subset[$section] = $settings.$section }
    $lines.Add(($subset | ConvertTo-Json -Depth 20))
} else {
    $lines.Add('steamvr.vrsettings not found.')
}

# The current and previous SteamVR sessions, since a failure often prompts a SteamVR restart.
# The driver's own log lines start with "VRControllerFreeze:"; the pattern also matches its driver name.
foreach ($logName in 'vrserver.txt', 'vrserver.previous.txt') {
    $lines.Add('')
    $lines.Add("=== $logName lines for this driver (last 250 matches) ===")
    $log = $logDirs | ForEach-Object { Join-Path $_ $logName } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($log) {
        $hits = Select-String -LiteralPath $log -Pattern $DriverName -SimpleMatch -CaseSensitive:$false | Select-Object -Last 250
        foreach ($hit in $hits) { $lines.Add($hit.Line) }
    } else {
        $lines.Add("$logName not found in the expected SteamVR/Steam log locations.")
    }
}

foreach ($logName in 'VRControllerFreeze.log', 'VRControllerFreeze.previous.log') {
    $lines.Add('')
    $lines.Add("=== Application log $logName (last 500 lines) ===")
    $appLog = Join-Path $appLogDir $logName
    if (Test-Path -LiteralPath $appLog) {
        foreach ($line in (Get-Content -LiteralPath $appLog | Select-Object -Last 500)) { $lines.Add($line) }
    } else {
        $lines.Add("$logName not found.")
    }
}

$report = [string[]]($lines | ForEach-Object { Remove-PrivateDetails $_ })
[IO.File]::WriteAllLines($Output, $report, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Diagnostics written to $Output" -ForegroundColor Green
Write-Host 'User and computer names have been replaced with <user> and <computer>, so the file can be shared.'
