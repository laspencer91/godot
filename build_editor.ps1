# build_editor.ps1 — the ONE way to build the production editor in this tree.
#
# Why this exists: multiple agent sessions share this working directory. Two
# concurrent scons runs (or runs with differing flags) interleave writes to the
# shared bin\obj\*.windows.editor.x86_64.* namespace and produce libs whose
# member lists don't match the objects that reference them (phantom LNK1120).
# This script serializes builds with a lock file, pins the canonical flag set,
# verifies the link actually replaced the binary (scons's exit code can be
# masked by shell pipelines), and restores the godot.exe / godot-editor.exe
# hardlinks that relinking always severs.
#
# Usage:  .\build_editor.ps1 [-Jobs 24]
# Exit codes: 0 = built + verified + hardlinks restored; 1 = failure.

param(
    [int]$Jobs = 24
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binDir = Join-Path $repoRoot "bin"
$lockPath = Join-Path $binDir ".build.lock"
$targetExe = Join-Path $binDir "godot.windows.editor.x86_64.exe"
$lockAcquired = $false

# Canonical production editor build. Do not vary these flags per session:
# every flag flip forces a large rebuild of the shared object tree.
# - production=yes: optimized, LTO, non-dev (this IS the "prod build").
# - target=editor: the editor binary itself (templates are a separate concern).
# - angle=no: ANGLE deps are not installed on this machine.
# - cppdefines silences the MSVC 14.51 STL1011 <experimental/coroutine> error
#   so the WinRT TTS backend stays enabled (preferred over winrt=no).
$sconsArgs = @(
    "platform=windows", "target=editor", "production=yes", "arch=x86_64",
    "angle=no", "cppdefines=_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS",
    "-j$Jobs"
)

function Get-ForeignSconsProcesses {
    # Any SCons run in this repo not started by this script (e.g. a session
    # bypassing the wrapper). We must not build while one is active.
    Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match "SCons" -and $_.CommandLine -match [regex]::Escape($repoRoot) }
}

function Wait-ForBuildSlot {
    $deadline = (Get-Date).AddMinutes(90)
    while ($true) {
        $foreign = @(Get-ForeignSconsProcesses)
        if ($foreign.Count -eq 0) {
            try {
                New-Item -ItemType File -Path $lockPath -ErrorAction Stop | Out-Null
                Set-Content -Path $lockPath -Value "$PID $(Get-Date -Format o)" -Encoding utf8
                return
            } catch {
                # Lock held: reap it if the owning process is gone.
                $ownerPid = $null
                try { $ownerPid = [int]((Get-Content $lockPath -ErrorAction Stop) -split " ")[0] } catch {}
                if ($ownerPid -and -not (Get-Process -Id $ownerPid -ErrorAction SilentlyContinue)) {
                    Write-Host "Removing stale build lock (owner PID $ownerPid is gone)."
                    Remove-Item $lockPath -Force -ErrorAction SilentlyContinue
                    continue
                }
            }
        } else {
            Write-Host ("Waiting: scons already running in this tree (PID {0})..." -f ($foreign[0].ProcessId))
        }
        if ((Get-Date) -gt $deadline) {
            throw "Timed out after 90 minutes waiting for the build slot."
        }
        Start-Sleep -Seconds 15
    }
}

function Restore-Hardlinks {
    foreach ($linkName in @("godot.exe", "godot-editor.exe")) {
        $linkPath = Join-Path $binDir $linkName
        try {
            if (Test-Path $linkPath) { Remove-Item $linkPath -Force -ErrorAction Stop }
            New-Item -ItemType HardLink -Path $linkPath -Target $targetExe -ErrorAction Stop | Out-Null
            Write-Host "Hardlink restored: $linkName"
        } catch {
            Write-Warning "Could not restore hardlink $linkName (in use?): $($_.Exception.Message)"
        }
    }
}

try {
    Wait-ForBuildSlot
    $lockAcquired = $true

    $beforeStamp = $null
    if (Test-Path $targetExe) { $beforeStamp = (Get-Item $targetExe).LastWriteTimeUtc }

    Push-Location $repoRoot
    try {
        & scons @sconsArgs
        $sconsExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    if ($sconsExit -ne 0) {
        throw "scons failed with exit code $sconsExit."
    }

    # scons can report success while the final link silently failed (locked
    # exe + masked error). The binary timestamp is the ground truth.
    if (-not (Test-Path $targetExe)) {
        throw "Build reported success but $targetExe does not exist."
    }
    $afterStamp = (Get-Item $targetExe).LastWriteTimeUtc
    if ($null -ne $beforeStamp -and $afterStamp -eq $beforeStamp) {
        throw "Build reported success but the binary was NOT relinked (timestamp unchanged). Is an editor instance holding the exe lock? Check: Get-Process godot*"
    }

    Restore-Hardlinks
    Write-Host "Build verified: $targetExe @ $((Get-Item $targetExe).LastWriteTime)"
    exit 0
} catch {
    Write-Error $_
    exit 1
} finally {
    if ($lockAcquired) { Remove-Item $lockPath -Force -ErrorAction SilentlyContinue }
}
