<#
.SYNOPSIS
  Fetches the engines and models this app needs, and checks the machine can build it.

.DESCRIPTION
  Everything this downloads is a public release from its own publisher -- no account,
  no token, about 1.27 GB in total. None of it is in git, which is why this script
  exists: a fresh clone cannot build or run until it has been run once.

  It is safe to run repeatedly. Every step checks for what it would produce and skips
  the download if it is already there, so a half-finished run is fixed by running it
  again rather than by cleaning up first.

  The VOICEVOX step is the one that looks alarming and is not: its downloader prints a
  pager panic when stdout is not a terminal, and completes anyway. This script judges
  that step by the files on disk, not by the exit code.

.PARAMETER CheckOnly
  Report what is present or missing and download nothing. Exits non-zero if anything
  needed is missing, so it doubles as an acceptance check.

.PARAMETER SkipPrerequisites
  Skip the build-tool checks (CMake, Visual Studio, the Vulkan SDK, Claude Code) and
  only deal with the downloads.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 -CheckOnly
#>
[CmdletBinding()]
param(
  [switch]$CheckOnly,
  [switch]$SkipPrerequisites
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Models = Join-Path $Root 'models'
$SherpaBin = Join-Path $Root 'spikes\stt\bin'
$VvDir = Join-Path $Root 'spikes\tts_cpu\voicevox'
$VvCore = Join-Path $VvDir 'voicevox_core'

$script:Problems = @()
$script:Generator = 'Visual Studio 17 2022'  # replaced when a newer Visual Studio is found

function Step([string]$Title) {
  Write-Host ''
  Write-Host "== $Title" -ForegroundColor Cyan
}
function Ok([string]$What) { Write-Host "   ok    $What" -ForegroundColor Green }
function Info([string]$What) { Write-Host "   ..    $What" -ForegroundColor Gray }
function Warn([string]$What) { Write-Host "   warn  $What" -ForegroundColor Yellow }
function Bad([string]$What) {
  Write-Host "   MISS  $What" -ForegroundColor Red
  $script:Problems += $What
}

# --- prerequisites ----------------------------------------------------------
# Checked, never installed. Each of these is a large interactive install with its
# own licence to accept, and a script that ran one unattended would be doing
# something the person running it did not ask for.

function Check-Prerequisites {
  Step 'Prerequisites'

  $cmake = Get-Command cmake -ErrorAction SilentlyContinue
  if ($null -eq $cmake) {
    Bad 'CMake is not on PATH. Install 3.24 or newer: https://cmake.org/download/'
  } else {
    $raw = (& cmake --version | Select-Object -First 1)
    if ($raw -match '(\d+)\.(\d+)\.(\d+)') {
      $ver = [version]("{0}.{1}.{2}" -f $Matches[1], $Matches[2], $Matches[3])
      if ($ver -lt [version]'3.24.0') {
        Bad "CMake $ver is too old; 3.24 or newer is required."
      } else {
        Ok "CMake $ver"
      }
    } else {
      Warn "CMake found but its version could not be read from: $raw"
    }
  }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName 2>$null
    if ([string]::IsNullOrWhiteSpace($vs)) {
      Bad 'Visual Studio 2022 or newer with the C++ workload ("Desktop development with C++") was not found.'
    } else {
      Ok "$vs (C++ toolchain present)"
      # The CMake generator is named after the Visual Studio major version, and
      # asking for one that is not installed fails the configure outright.
      $version = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2>$null
      if ("$version".StartsWith('18.')) { $script:Generator = 'Visual Studio 18 2026' }
    }
  } else {
    Bad 'Visual Studio Installer not found. Install Visual Studio 2022 or newer with "Desktop development with C++".'
  }

  # dxc from the Vulkan SDK compiles the shaders. The engine hard-fails without it,
  # and it fails at build time rather than configure time, which is a confusing place
  # to discover a missing SDK.
  if ([string]::IsNullOrWhiteSpace($env:VULKAN_SDK)) {
    Bad 'VULKAN_SDK is not set. Install the Vulkan SDK (it provides dxc): https://vulkan.lunarg.com/sdk/home'
  } elseif (-not (Test-Path (Join-Path $env:VULKAN_SDK 'Bin\dxc.exe'))) {
    Bad "VULKAN_SDK is set to $env:VULKAN_SDK but Bin\dxc.exe is not there."
  } else {
    Ok "Vulkan SDK at $env:VULKAN_SDK"
  }

  # The engine is a submodule. A clone made without --recurse-submodules leaves the
  # directory empty, and the failure that produces at configure time names CMake
  # rather than git, so it is worth catching here.
  if (Test-Path (Join-Path $Root 'third_party\Renderer\CMakeLists.txt')) {
    Ok 'Renderer submodule is populated'
  } else {
    Bad 'third_party\Renderer is empty. Run: git submodule update --init --recursive'
  }

  # Claude itself. The app starts without this -- the window appears and the status
  # line reports the failure -- so it is a warning, not a blocker.
  $claudeExe = $env:AII_CLAUDE_EXE
  if ([string]::IsNullOrWhiteSpace($claudeExe)) {
    $claudeExe = Join-Path $env:USERPROFILE '.local\bin\claude.exe'
  }
  if (Test-Path $claudeExe) {
    Ok "Claude Code CLI at $claudeExe"
  } else {
    Warn "Claude Code CLI not found at $claudeExe. The app will build and start, but every"
    Warn 'turn will fail until it is installed and signed in (or AII_CLAUDE_EXE points at it).'
  }
}

# --- downloads --------------------------------------------------------------
#
# M21.3 (review finding 34). Three things were wrong here and all three are
# below:
#
#   1. Nothing checked what 1.27 GB of HTTP actually delivered. Every download
#      now carries a SHA-256 and a file that does not match it is deleted, not
#      extracted.
#   2. `tar` writing into the final directory meant a failure part way through
#      an archive left a tree that the next run's "already installed" test
#      accepted. Extraction now happens in a sibling temp directory and is
#      moved into place only after tar exits 0.
#   3. `Verify` did not name two files `kokoro_tts.cpp` opens, so a tree
#      missing them passed the acceptance check and failed at runtime.
#
# Where the hashes came from, exactly
# -----------------------------------
# They were computed on the machine this app was developed on, from the file
# that tree was actually built from -- not from a publisher's checksum page,
# because none of these releases publishes one.
#
#   sherpa-onnx ...tar.bz2   the archive still in %TEMP% on that machine. It is
#                            provably the one installed there: the
#                            sherpa-onnx-c-api.dll inside it is byte-identical
#                            (SHA-256 F86ED157...) to the installed one.
#   download-windows-x64.exe the VOICEVOX downloader in spikes\tts_cpu\voicevox.
#
# The Nemotron and Kokoro archives were deleted after extraction and are not on
# that machine any more, so there is nothing honest to pin them to yet. They
# are left unpinned *and say so*: the script prints the hash it got and asks
# for it to be filled in here. Pinning a hash computed from a re-download would
# be pinning whatever the network happened to serve, which is the thing this
# change exists to stop. Anyone who runs a fresh setup should paste the two
# hashes it prints into the table below.

$script:Sha = @{
  Sherpa   = '3E971A04B2E0BA4DFA53D381A006367CE8C9F5F09B4AE00043E9845C2BADED22'
  Nemotron = ''   # not pinned; see above
  Kokoro   = ''   # not pinned; see above
  VvDownloader = '4F0AE2758F3149F084CC91556065009553BF81010F58498C89ACB6E2289546B6'
}

function Test-Sha256([string]$File, [string]$Expected, [string]$Label) {
  $actual = (Get-FileHash -Path $File -Algorithm SHA256).Hash
  if ([string]::IsNullOrWhiteSpace($Expected)) {
    Warn "$Label is not pinned. Its SHA-256 is $actual"
    Warn "  -- paste it into `$script:Sha in scripts\setup.ps1 so the next machine is checked."
    return $true
  }
  if ($actual -ne $Expected.ToUpper()) {
    Bad "$Label does not match its pinned SHA-256."
    Info "  expected $($Expected.ToUpper())"
    Info "  got      $actual"
    return $false
  }
  Ok "$Label matches its pinned SHA-256"
  return $true
}

function Get-Archive([string]$Url, [string]$ToFile, [string]$Sha256) {
  $leaf = Split-Path -Leaf $ToFile
  if (Test-Path $ToFile) {
    Info "already downloaded: $leaf"
    # Checked again rather than trusted: a cached archive is exactly the thing
    # that can have been truncated by a previous interrupted run.
    if (-not (Test-Sha256 $ToFile $Sha256 $leaf)) {
      Remove-Item $ToFile -Force
      throw "$leaf was corrupt and has been deleted. Run this script again to fetch it."
    }
    return
  }
  Info "downloading $leaf"
  $partial = "$ToFile.partial"
  if (Test-Path $partial) { Remove-Item $partial -Force }
  & curl.exe -L --retry 5 --fail -o $partial $Url
  if ($LASTEXITCODE -ne 0) {
    if (Test-Path $partial) { Remove-Item $partial -Force }
    throw "Download failed ($Url). curl exited $LASTEXITCODE."
  }
  if (-not (Test-Sha256 $partial $Sha256 $leaf)) {
    Remove-Item $partial -Force
    throw "$leaf did not match its pinned SHA-256 and has been deleted. Nothing was extracted."
  }
  Move-Item $partial $ToFile
}

# Extracts into a temp directory beside the destination and moves the result in
# only if tar succeeded, so a failure part way through an archive leaves the
# destination exactly as it was rather than a half tree the next run calls
# installed.
function Expand-TarArchive([string]$Archive, [string]$Into) {
  if (-not (Test-Path $Into)) { New-Item -ItemType Directory -Path $Into -Force | Out-Null }
  $staging = Join-Path $Into (".extracting-" + [System.IO.Path]::GetRandomFileName())
  New-Item -ItemType Directory -Path $staging -Force | Out-Null
  try {
    Info "extracting into $staging"
    & tar.exe xjf $Archive -C $staging
    if ($LASTEXITCODE -ne 0) { throw "tar failed on $Archive (exit $LASTEXITCODE). Nothing was installed." }
    foreach ($item in Get-ChildItem -LiteralPath $staging -Force) {
      $dest = Join-Path $Into $item.Name
      if (Test-Path $dest) {
        # Only reachable if a previous run installed this and the caller's
        # "already installed" test did not see it; the extracted copy is the
        # one that was just checked, so it wins.
        Remove-Item -LiteralPath $dest -Recurse -Force
      }
      Move-Item -LiteralPath $item.FullName -Destination $dest
    }
    Info "moved into $Into"
  } finally {
    if (Test-Path $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
  }
}

function Install-Sherpa {
  Step 'sherpa-onnx prebuilt (recognition + Kokoro runtime), 20 MB'
  $target = Join-Path $SherpaBin 'sherpa-onnx-v1.13.8-win-x64-shared-MD-Release'
  if (Test-Path (Join-Path $target 'lib\sherpa-onnx-c-api.dll')) { Ok 'already installed'; return }
  $url = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/sherpa-onnx-v1.13.8-win-x64-shared-MD-Release.tar.bz2'
  $archive = Join-Path $env:TEMP 'sherpa-onnx-v1.13.8.tar.bz2'
  Get-Archive $url $archive $script:Sha.Sherpa
  Expand-TarArchive $archive $SherpaBin
  Ok 'installed'
}

function Install-Nemotron {
  Step 'Recognition model: Nemotron-3.5 streaming, 475 MB'
  $target = Join-Path $Models 'sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11'
  if (Test-Path (Join-Path $target 'encoder.int8.onnx')) { Ok 'already installed'; return }
  $url = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11.tar.bz2'
  $archive = Join-Path $env:TEMP 'nemotron-3.5.tar.bz2'
  Get-Archive $url $archive $script:Sha.Nemotron
  Expand-TarArchive $archive $Models
  Ok 'installed'
}

function Install-Kokoro {
  Step 'English voice: Kokoro-82M multi-lang v1.0, 350 MB'
  # v1.0 deliberately, not v1.1: v1.1 lacks af_heart and af_bella, which are the
  # voices the app defaults to.
  $target = Join-Path $Models 'kokoro-multi-lang-v1_0'
  if (Test-Path (Join-Path $target 'model.onnx')) { Ok 'already installed'; return }
  $url = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-multi-lang-v1_0.tar.bz2'
  $archive = Join-Path $env:TEMP 'kokoro-multi-lang-v1_0.tar.bz2'
  Get-Archive $url $archive $script:Sha.Kokoro
  Expand-TarArchive $archive $Models
  Ok 'installed'
}

function Install-Voicevox {
  Step 'Japanese voice: VOICEVOX Core 0.17.0, about 250 MB'
  $dictOk = Test-Path (Join-Path $Models 'voicevox\dict\open_jtalk_dic_utf_8-1.11')
  $vvmOk = Test-Path (Join-Path $Models 'voicevox\models\vvms\0.vvm')
  $coreOk = Test-Path (Join-Path $VvCore 'c_api\lib\voicevox_core.dll')
  if ($dictOk -and $vvmOk -and $coreOk) { Ok 'already installed'; return }

  if (-not (Test-Path $VvDir)) { New-Item -ItemType Directory -Path $VvDir -Force | Out-Null }
  $downloader = Join-Path $VvDir 'download-windows-x64.exe'
  if (-not (Test-Path $downloader)) {
    Get-Archive 'https://github.com/VOICEVOX/voicevox_core/releases/download/0.17.0/download-windows-x64.exe' $downloader $script:Sha.VvDownloader
  }

  # The bare release holds only the C API; this downloader also fetches the matching
  # ONNX Runtime, the Open JTalk dictionary and the voice model, and shows the licence
  # terms, which the 'y' accepts. --exclude additional-libraries skips the DirectML and
  # CUDA builds: this app runs VOICEVOX on the CPU.
  #
  # Its pager panics when stdout is not a terminal and the download still completes,
  # so the exit code is not trusted here -- the files on disk are checked instead.
  Info 'running the VOICEVOX downloader (it prints its licence terms; a panic from its pager is harmless)'
  Push-Location $VvDir
  try {
    # The 'y' goes through cmd: a PowerShell pipe can prefix it with a UTF-8 BOM, which
    # the downloader rejects as an invalid answer and then downloads nothing.
    cmd /c "echo y| `"$downloader`" -o .\voicevox_core --models-pattern 0.vvm --exclude additional-libraries"
  } catch {
    Warn "the VOICEVOX downloader reported: $($_.Exception.Message)"
  } finally {
    Pop-Location
  }

  if (-not (Test-Path (Join-Path $VvCore 'c_api\lib\voicevox_core.dll'))) {
    Bad 'the VOICEVOX downloader did not produce voicevox_core\c_api\lib\voicevox_core.dll'
    return
  }

  # The dictionary and the voice models move to models\, which is where the app looks
  # for them; the C API and its ONNX Runtime stay where CMake expects them.
  $vvModels = Join-Path $Models 'voicevox'
  if (-not (Test-Path $vvModels)) { New-Item -ItemType Directory -Path $vvModels -Force | Out-Null }
  foreach ($leaf in @('dict', 'models')) {
    $from = Join-Path $VvCore $leaf
    $to = Join-Path $vvModels $leaf
    if ((Test-Path $from) -and (-not (Test-Path $to))) {
      Move-Item $from $to
      Info "moved $leaf into models\voicevox\"
    }
  }
  Ok 'installed'
}

# --- verification -----------------------------------------------------------
# Deliberately a separate pass over paths taken from the source, not from what the
# steps above believe they did. This is what -CheckOnly runs on its own, and it is
# the acceptance check for a fresh machine.

function Verify {
  Step 'Verifying what the app will look for at runtime'

  $sherpaLib = 'spikes\stt\bin\sherpa-onnx-v1.13.8-win-x64-shared-MD-Release\lib'
  $asr = 'models\sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11'
  $vv = 'spikes\tts_cpu\voicevox\voicevox_core'

  $expected = @(
    @{ Path = "$sherpaLib\sherpa-onnx-c-api.dll"; What = 'sherpa-onnx runtime' },
    @{ Path = "$sherpaLib\sherpa-onnx-c-api.lib"; What = 'sherpa-onnx import library' },
    @{ Path = "$sherpaLib\onnxruntime.dll"; What = 'ONNX Runtime' },
    @{ Path = "$asr\encoder.int8.onnx"; What = 'recogniser encoder' },
    @{ Path = "$asr\decoder.int8.onnx"; What = 'recogniser decoder' },
    @{ Path = "$asr\joiner.int8.onnx"; What = 'recogniser joiner' },
    @{ Path = "$asr\tokens.txt"; What = 'recogniser tokens' },
    @{ Path = 'models\kokoro-multi-lang-v1_0\model.onnx'; What = 'English voice model' },
    @{ Path = 'models\kokoro-multi-lang-v1_0\voices.bin'; What = 'English voice styles' },
    # M21.3. These two were missing from this list and are not optional:
    # src\tts\kokoro_tts.cpp builds its config from model.onnx, voices.bin,
    # tokens.txt, espeak-ng-data and lexicon-us-en.txt, so a tree without them
    # passed this check and then failed to synthesise a word. (lexicon-zh.txt is
    # deliberately absent: that one is tested for at runtime and appended only
    # if present.)
    @{ Path = 'models\kokoro-multi-lang-v1_0\tokens.txt'; What = 'English voice tokens' },
    @{ Path = 'models\kokoro-multi-lang-v1_0\lexicon-us-en.txt'; What = 'English lexicon' },
    @{ Path = 'models\kokoro-multi-lang-v1_0\espeak-ng-data'; What = 'English phonemiser data' },
    @{ Path = 'models\voicevox\dict\open_jtalk_dic_utf_8-1.11'; What = 'Open JTalk dictionary' },
    @{ Path = 'models\voicevox\models\vvms\0.vvm'; What = 'Japanese voice model' },
    @{ Path = "$vv\c_api\lib\voicevox_core.dll"; What = 'VOICEVOX core' },
    @{ Path = "$vv\c_api\lib\voicevox_core.lib"; What = 'VOICEVOX import library' },
    @{ Path = "$vv\c_api\include\voicevox_core.h"; What = 'VOICEVOX header' },
    @{ Path = "$vv\onnxruntime\lib\voicevox_onnxruntime.dll"; What = 'VOICEVOX ONNX Runtime' }
  )

  foreach ($e in $expected) {
    if (Test-Path (Join-Path $Root $e.Path)) {
      Ok $e.What
    } else {
      Bad "$($e.What) -- expected at $($e.Path)"
    }
  }
}

# --- run --------------------------------------------------------------------

Write-Host "AIInterface setup -- $Root"

if (-not $SkipPrerequisites) { Check-Prerequisites }

if (-not $CheckOnly) {
  if (-not (Test-Path $Models)) { New-Item -ItemType Directory -Path $Models -Force | Out-Null }
  Install-Sherpa
  Install-Nemotron
  Install-Kokoro
  Install-Voicevox
}

Verify

Write-Host ''
if ($script:Problems.Count -gt 0) {
  Write-Host "Not ready: $($script:Problems.Count) item(s) outstanding." -ForegroundColor Red
  foreach ($p in $script:Problems) { Write-Host "  - $p" -ForegroundColor Red }
  Write-Host ''
  Write-Host 'Fix the items above and run this script again; it skips what is already done.'
  exit 1
}

Write-Host 'Ready. Next:' -ForegroundColor Green
Write-Host "    cmake -S . -B build -G `"$script:Generator`" -A x64"
Write-Host '    cmake --build build --config Release'
Write-Host '    build\bin\Release\avatar.exe'
exit 0
