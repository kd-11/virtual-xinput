# Builds both architectures and assembles the dist folder.
#
#   .\build.ps1              build, test, and assemble dist\
#
# dist\games.yml and dist\profiles\ are the user's, and survive every rebuild.
#   .\build.ps1 -NoTests     skip the test run
#   .\build.ps1 -NoAliases   only xinput1_3.dll, no extra XInput version copies
#   .\build.ps1 -Clean       delete build\ and dist\ first

[CmdletBinding()]
param(
    [switch]$NoTests,
    [switch]$NoAliases,
    [switch]$Clean,
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$dist = Join-Path $root "dist"

# dist is rebuilt from scratch every time so removed files never linger, but the
# game list and the profile library belong to the user and are put back
# afterwards.
#
# These have to be saved before ANY deletion, -Clean included, or the user's
# work goes with them.
$gamesFile   = Join-Path $dist "games.yml"
$profilesDir = Join-Path $dist "profiles"
$stash       = Join-Path ([System.IO.Path]::GetTempPath()) ("vx-dist-stash-" + [guid]::NewGuid())

$savedGames = $null
if (Test-Path $gamesFile) { $savedGames = Get-Content -Raw $gamesFile }

$savedProfiles = $false
if (Test-Path $profilesDir) {
    New-Item -ItemType Directory -Force $stash | Out-Null
    Copy-Item -Recurse -Force (Join-Path $profilesDir "*") $stash -ErrorAction SilentlyContinue
    $savedProfiles = $true
}

# Everything from the first deletion to the restore runs inside try/finally.
# Without it a failing compile or a failing test takes the user's game library
# and profiles with it: dist is already deleted by then, and the restore at the
# bottom is never reached. Losing somebody's work because a build broke is not
# an acceptable way for a build script to fail.
try {

if ($Clean) {
    foreach ($d in @((Join-Path $root "build"), $dist)) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    }
}

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }

$aliases = if ($NoAliases) { "OFF" } else { "ON" }

$targets = @(
    @{ Name = "x86"; Platform = "Win32" },
    @{ Name = "x64"; Platform = "x64" }
)

foreach ($t in $targets) {
    $buildDir = Join-Path $root "build\$($t.Name)"

    Write-Host ""
    Write-Host "=== Configuring $($t.Name) ===" -ForegroundColor Cyan
    cmake -S $root -B $buildDir -G $Generator -A $t.Platform -DVX_BUILD_ALIASES=$aliases | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "configure failed for $($t.Name)" }

    Write-Host "=== Building $($t.Name) ===" -ForegroundColor Cyan
    cmake --build $buildDir --config Release | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build failed for $($t.Name)" }

    if (-not $NoTests) {
        Write-Host "=== Testing $($t.Name) ===" -ForegroundColor Cyan

        & (Join-Path $buildDir "Release\vx_tests.exe") | Select-Object -Last 1
        if ($LASTEXITCODE -ne 0) { throw "unit tests failed for $($t.Name)" }

        $dll = Join-Path $buildDir "Release\xinput1_3.dll"
        & (Join-Path $buildDir "Release\vx_dll_host.exe") $dll | Select-Object -Last 1
        if ($LASTEXITCODE -ne 0) { throw "DLL smoke test failed for $($t.Name)" }
    }

    Write-Host "=== Installing $($t.Name) ===" -ForegroundColor Cyan
    cmake --install $buildDir --config Release --prefix $dist | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "install failed for $($t.Name)" }
}

} finally {
    # The dist folder may not exist at all if the build died early, so recreate
    # it rather than assuming cmake --install got that far.
    if ($savedGames -or $savedProfiles) {
        New-Item -ItemType Directory -Force $dist | Out-Null
    }
    if ($savedGames) { Set-Content -Path $gamesFile -Value $savedGames -NoNewline }

    if ($savedProfiles) {
        New-Item -ItemType Directory -Force $profilesDir | Out-Null
        Copy-Item -Recurse -Force (Join-Path $stash "*") $profilesDir -ErrorAction SilentlyContinue
        Remove-Item -Recurse -Force $stash -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "=== dist ===" -ForegroundColor Green
Get-ChildItem -Recurse -File $dist |
    ForEach-Object { "  {0,-40} {1,8:N0} bytes" -f $_.FullName.Substring($dist.Length + 1), $_.Length }

Write-Host ""
Write-Host "Ready. Run dist\virtual-xinput-config.exe to add and install games." -ForegroundColor Green
