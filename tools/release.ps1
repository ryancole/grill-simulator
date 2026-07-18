<#
.SYNOPSIS
  Build the release zip, tag the commit, and create a GitHub release.

.DESCRIPTION
  Wraps tools/package.ps1 and the GitHub CLI (gh) into one release step:

    1. Reads the version from CMakeLists.txt (project VERSION) -- the tag is vX.Y.Z.
    2. Refuses to run on a dirty working tree or a non-default branch (override
       with -AllowDirty / -AllowAnyBranch) so a release always maps to a clean,
       known commit.
    3. Builds the distributable zip via package.ps1 (skip with -SkipBuild to reuse
       the existing dist/ artifact).
    4. Creates an annotated git tag at HEAD and pushes it to origin.
    5. Creates the GitHub release and uploads the zip as an asset.

  The release is created as a DRAFT unless -Publish is given: a draft is private to
  repo collaborators and goes live only when you publish it from the GitHub UI, so
  you get a chance to review the notes and the uploaded zip first.

.PARAMETER Publish
  Publish the release immediately instead of leaving it as a draft.

.PARAMETER NotesFile
  Path to a markdown file used as the release body. Defaults to auto-generated
  notes from gh (--generate-notes) when omitted.

.PARAMETER SkipBuild
  Reuse the existing dist/grill-simulator-vX.Y.Z.zip instead of rebuilding.

.PARAMETER AllowDirty
  Proceed even if the working tree has uncommitted changes.

.PARAMETER AllowAnyBranch
  Proceed even if the current branch is not the default branch (master).

.EXAMPLE
  pwsh tools/release.ps1
  # Build, tag v0.1.0, push, create a DRAFT release with generated notes.

.EXAMPLE
  pwsh tools/release.ps1 -Publish -NotesFile CHANGELOG-0.1.0.md
#>
[CmdletBinding()]
param(
    [switch]$Publish,
    [string]$NotesFile,
    [switch]$SkipBuild,
    [switch]$AllowDirty,
    [switch]$AllowAnyBranch
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$defaultBranch = 'master'

# --- Preconditions ---------------------------------------------------------

# gh must be installed and authenticated, or the release step fails late after
# having already pushed a tag. Check up front.
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "GitHub CLI (gh) not found on PATH. Install it: https://cli.github.com/"
}
& gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
    throw "gh is not authenticated. Run 'gh auth login' first."
}

# Version -> tag name, from the single source of truth.
$cmakeLists = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch '(?ms)project\s*\([^)]*?VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not find the project VERSION in CMakeLists.txt."
}
$version = $Matches[1]
$tag = "v$version"

Push-Location $repoRoot
try {
    # A release should map to a clean, known commit.
    $branch = (& git rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -ne $defaultBranch -and -not $AllowAnyBranch) {
        throw "On branch '$branch', not '$defaultBranch'. Use -AllowAnyBranch to override."
    }
    if (-not $AllowDirty) {
        $dirty = (& git status --porcelain)
        if ($dirty) {
            throw "Working tree is dirty. Commit/stash first, or pass -AllowDirty.`n$dirty"
        }
    }

    # Refuse to clobber an existing tag (locally or on the remote): re-releasing a
    # version should be a deliberate delete, not a silent overwrite here.
    if (& git tag --list $tag) {
        throw "Tag $tag already exists locally. Delete it (git tag -d $tag) to re-release."
    }
    if (& git ls-remote --tags origin "refs/tags/$tag") {
        throw "Tag $tag already exists on origin. Delete it (git push origin :refs/tags/$tag) to re-release."
    }

    Write-Host "Releasing grill-simulator $tag from $branch @ $((git rev-parse --short HEAD).Trim())" -ForegroundColor Cyan

    # --- Build the artifact ------------------------------------------------

    $packageArgs = @()
    if ($SkipBuild) { $packageArgs += '-SkipBuild' }
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'package.ps1') @packageArgs
    if ($LASTEXITCODE -ne 0) { throw "package.ps1 failed (exit $LASTEXITCODE)." }

    $zipPath = Join-Path $repoRoot "dist/grill-simulator-$tag.zip"
    if (-not (Test-Path $zipPath)) { throw "Expected artifact not found: $zipPath" }

    # --- Tag and push ------------------------------------------------------

    Write-Host "Tagging $tag and pushing to origin..." -ForegroundColor Cyan
    & git tag -a $tag -m "Release $tag"
    if ($LASTEXITCODE -ne 0) { throw "git tag failed." }
    & git push origin $tag
    if ($LASTEXITCODE -ne 0) {
        # Roll back the local tag so a retry isn't blocked by our own half-run.
        & git tag -d $tag | Out-Null
        throw "git push of tag failed; removed the local tag so you can retry."
    }

    # --- Create the release ------------------------------------------------

    $ghArgs = @(
        'release', 'create', $tag, $zipPath,
        '--title', "grill-simulator $tag",
        '--target', $branch
    )
    if ($NotesFile) {
        if (-not (Test-Path $NotesFile)) { throw "Notes file not found: $NotesFile" }
        $ghArgs += @('--notes-file', $NotesFile)
    } else {
        $ghArgs += '--generate-notes'
    }
    if (-not $Publish) { $ghArgs += '--draft' }

    Write-Host "Creating GitHub release ($(if ($Publish) {'published'} else {'draft'}))..." -ForegroundColor Cyan
    & gh @ghArgs
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed (exit $LASTEXITCODE)." }

    Write-Host ""
    if ($Publish) {
        Write-Host "Published release $tag." -ForegroundColor Green
    } else {
        Write-Host "Draft release $tag created. Review it and publish from GitHub:" -ForegroundColor Green
        Write-Host "  gh release view $tag --web" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
