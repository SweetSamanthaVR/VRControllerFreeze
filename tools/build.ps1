<#
.SYNOPSIS
Builds VR Controller Freeze into build\dist, optionally running every test and code-signing the binaries.

.PARAMETER Configuration
Release (default) or Debug.

.PARAMETER Clean
Deletes the build folder first.

.PARAMETER Test
After building, runs the static checks, the app logic unit tests and the driver harness (standalone and
behind a calibrator-style hook). Stops at the first failure. The harness refuses to run while SteamVR
or the app is open.

.PARAMETER CertificateThumbprint
Signs VRControllerFreeze.exe and the driver DLL with this code-signing certificate from the
current user's or local machine's certificate store, with an RFC 3161 timestamp.

.PARAMETER TimestampUrl
Timestamp server used when signing.
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Clean,
    [switch]$Test,
    [string]$CertificateThumbprint,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'
. (Join-Path $PSScriptRoot 'steamvr-paths.ps1')

function Resolve-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = New-Object System.Collections.Generic.List[string]

    if ($env:VSINSTALLDIR) {
        $candidates.Add((Join-Path $env:VSINSTALLDIR 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
    }

    $vswhere = $null
    if (${env:ProgramFiles(x86)}) {
        $candidateVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $candidateVswhere) {
            $vswhere = $candidateVswhere
        }
    }

    if ($vswhere) {
        $installations = & $vswhere -products * -property installationPath 2>$null
        foreach ($installation in $installations) {
            if ($installation) {
                $candidates.Add((Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
            }
        }
    }

    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        $visualStudioRoot = Join-Path $base 'Microsoft Visual Studio'
        if (-not (Test-Path -LiteralPath $visualStudioRoot)) { continue }

        foreach ($versionDir in Get-ChildItem -LiteralPath $visualStudioRoot -Directory -ErrorAction SilentlyContinue) {
            foreach ($editionDir in Get-ChildItem -LiteralPath $versionDir.FullName -Directory -ErrorAction SilentlyContinue) {
                $candidates.Add((Join-Path $editionDir.FullName 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
            }
        }
    }

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw @'
CMake could not be found.

Install the Visual Studio "Desktop development with C++" workload with
"C++ CMake tools for Windows", or install Kitware CMake and make it
available on PATH.
'@
}

# signtool.exe from the newest installed Windows SDK.
function Resolve-SignTool {
    $sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $tool = Get-ChildItem -LiteralPath $sdkBin -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (-not $tool) { throw 'signtool.exe was not found. Install the Windows SDK to sign the build.' }
    return $tool
}

# Runs a native command and stops the build if it fails.
function Invoke-Checked([string]$Description, [scriptblock]$Command) {
    Write-Host $Description -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Description failed (exit code $LASTEXITCODE)." }
}

# Windows locks a running program's files, so staging would fail part-way with a copy error.
$distDriver = Join-Path $buildDir "dist\driver\$DriverName"
if ((Get-Process vrserver -ErrorAction SilentlyContinue) -and (@(Get-RegisteredDriverCopies | Where-Object { Test-SamePath $_ $distDriver }).Count -gt 0)) {
    throw 'SteamVR is running with the driver from this build folder loaded, so the build cannot replace it. Exit SteamVR completely, then build again.'
}
$distApp = Join-Path $buildDir 'dist\VRControllerFreeze.exe'
if (Get-Process VRControllerFreeze -ErrorAction SilentlyContinue | Where-Object { $_.Path -and (Test-SamePath $_.Path $distApp) }) {
    throw "$ProductName is running from this build folder. Close it, then build again."
}

$cmake = Resolve-CMake
Write-Host "Using CMake: $cmake" -ForegroundColor DarkGray
& $cmake --version | Select-Object -First 1 | Write-Host -ForegroundColor DarkGray

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

Invoke-Checked "Configuring $ProductName ($Configuration)..." { & $cmake -S $root -B $buildDir -A x64 }
Invoke-Checked 'Building...' { & $cmake --build $buildDir --config $Configuration --target stage }

$dist = Join-Path $buildDir 'dist'
$binaries = @(
    (Join-Path $dist 'VRControllerFreeze.exe'),
    (Join-Path $dist "driver\$DriverName\bin\win64\driver_$DriverName.dll")
)

if ($CertificateThumbprint) {
    $signTool = Resolve-SignTool
    Invoke-Checked 'Signing...' {
        & $signTool sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 /d $ProductName @binaries
    }
}

if ($Test) {
    Invoke-Checked 'Static checks...' { & python (Join-Path $root 'tests\static_validate.py') }
    Invoke-Checked 'Building tests...' { & $cmake --build $buildDir --config $Configuration --target app_logic_tests driver_harness }
    $testDir = Join-Path $buildDir $Configuration
    Invoke-Checked 'App logic unit tests...' { & (Join-Path $testDir 'app_logic_tests.exe') | Select-Object -Last 1 }
    Invoke-Checked 'Driver harness (standalone)...' { & (Join-Path $testDir 'driver_harness.exe') $binaries[1] | Select-Object -Last 1 }
    Invoke-Checked 'Driver harness (behind a calibrator-style hook)...' {
        & (Join-Path $testDir 'driver_harness.exe') $binaries[1] --with-calibrator | Select-Object -Last 1
    }
    Write-Host 'All tests passed.' -ForegroundColor Green
}

Write-Host "Build complete: $dist" -ForegroundColor Green
Write-Host 'Next: close SteamVR, then run install-driver.cmd' -ForegroundColor Yellow
