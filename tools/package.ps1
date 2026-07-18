<#
.SYNOPSIS
  Build a Release configuration and pack it into a self-contained, distributable zip.

.DESCRIPTION
  The CMake build already stages every runtime the game needs (the Agility SDK,
  FMOD, PhysX, Flow, the compiled shaders and all assets) next to grill.exe under
  build/bin/Release. This script rebuilds that folder, copies it to a clean,
  version-stamped staging directory under dist/, drops the developer-only files a
  player never needs, and zips the result.

  The version is read from the project() VERSION in CMakeLists.txt so the artifact
  name stays in sync with the source of truth -- there is no version to bump here.

.PARAMETER SkipBuild
  Package whatever is already in build/bin/Release without rebuilding first.

.EXAMPLE
  pwsh tools/package.ps1
  # -> dist/grill-simulator-v0.1.0.zip

.EXAMPLE
  pwsh tools/package.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

# Resolve paths relative to the repo root (this script lives in tools/), so the
# script works regardless of the caller's current directory.
$repoRoot = Split-Path -Parent $PSScriptRoot
$releaseDir = Join-Path $repoRoot 'build/bin/Release'
$distDir = Join-Path $repoRoot 'dist'

# Pull the version straight from CMakeLists.txt: project(... VERSION x.y.z ...).
$cmakeLists = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch '(?ms)project\s*\([^)]*?VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not find the project VERSION in CMakeLists.txt."
}
$version = $Matches[1]

Write-Host "Packaging grill-simulator v$version" -ForegroundColor Cyan

# 1. Rebuild the Release configuration unless asked to reuse the existing output.
if (-not $SkipBuild) {
    Write-Host "Building Release configuration..." -ForegroundColor Cyan
    & cmake --build --preset release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)." }
}

if (-not (Test-Path (Join-Path $releaseDir 'grill.exe'))) {
    throw "No grill.exe under $releaseDir. Configure and build the 'release' preset first (or drop -SkipBuild)."
}

# 2. Stage a clean copy so we never mutate the build tree itself.
$stageName = "grill-simulator-v$version"
$stageDir = Join-Path $distDir $stageName
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
Copy-Item (Join-Path $releaseDir '*') $stageDir -Recurse

# 3. Drop developer-only files a shipped copy has no use for:
#    - d3d12SDKLayers.dll: the D3D12 debug layer, loaded only with Graphics Tools installed.
#    - *.level: a stray binary artifact not produced by the build (levels ship as .toml).
#    - controls.user.toml: a machine-local keybind override, never part of a release.
$prune = @(
    'D3D12/d3d12SDKLayers.dll'
    'assets/levels/*.level'
    'controls.user.toml'
)
foreach ($pattern in $prune) {
    Get-ChildItem (Join-Path $stageDir $pattern) -ErrorAction SilentlyContinue |
        ForEach-Object {
            Write-Host "  pruning $($_.Name)" -ForegroundColor DarkGray
            Remove-Item $_.FullName -Force
        }
}

# 4. Zip the staged folder. Compress-Archive stores paths relative to the items
#    passed in, so pointing at the stage dir itself keeps the top-level folder in
#    the archive (players unzip to a single grill-simulator-vX.Y.Z directory).
$zipPath = Join-Path $distDir "$stageName.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path $stageDir -DestinationPath $zipPath

$zipSize = "{0:N1} MB" -f ((Get-Item $zipPath).Length / 1MB)
Write-Host ""
Write-Host "Done: $zipPath ($zipSize)" -ForegroundColor Green
Write-Host "Players unzip and run grill.exe." -ForegroundColor Green
Write-Host "Note: the app links the MSVC runtime dynamically -- target machines need" -ForegroundColor Yellow
Write-Host "      the Visual C++ Redistributable (https://aka.ms/vs/17/release/vc_redist.x64.exe)." -ForegroundColor Yellow
