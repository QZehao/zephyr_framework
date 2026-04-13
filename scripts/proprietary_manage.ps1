#!/usr/bin/env pwsh
# =============================================================================
# Proprietary Modules Management Script (PowerShell)
# =============================================================================
# Usage:
#   .\proprietary_manage.ps1 -Enable          # Enable proprietary modules
#   .\proprietary_manage.ps1 -Disable         # Disable proprietary modules
#   .\proprietary_manage.ps1 -Status          # Show status
#   .\proprietary_manage.ps1 -Update          # Update proprietary modules
# =============================================================================

param(
    [switch]$Enable,
    [switch]$Disable,
    [switch]$Status,
    [switch]$Update,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

# Color output functions
function Write-Info { Write-Host "[INFO] $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Warning { Write-Host "[WARN] $args" -ForegroundColor Yellow }
function Write-ErrorMsg { Write-Host "[ERROR] $args" -ForegroundColor Red }

# Project paths
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ProprietaryDir = Join-Path $ProjectRoot "src\proprietary"
$ProprietaryCommonCmake = Join-Path $ProprietaryDir "proprietary_modules_common.cmake"

function Show-Help {
    Write-Host @"

Proprietary Modules Management Script

Usage:
    .\proprietary_manage.ps1 -Enable      Enable proprietary modules (init submodule)
    .\proprietary_manage.ps1 -Disable     Disable proprietary modules (remove submodule content)
    .\proprietary_manage.ps1 -Status      Show proprietary modules status
    .\proprietary_manage.ps1 -Update      Update proprietary modules to latest version
    .\proprietary_manage.ps1 -Help        Show this help

Description:
    The proprietary modules directory (src/proprietary) is a Git submodule.
    - Enabled: Can configure and use proprietary module features during build
    - Disabled: Project can still compile normally without proprietary modules

"@
}

function Test-GitAvailable {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-ErrorMsg "Git is not installed or not in PATH"
        exit 1
    }
}

function Test-IsGitRepo {
    Test-GitAvailable
    Push-Location $ProjectRoot
    try {
        git rev-parse --git-dir | Out-Null
        return $true
    }
    catch {
        return $false
    }
    finally {
        Pop-Location
    }
}

function Get-SubmoduleStatus {
    Test-GitAvailable
    Push-Location $ProjectRoot
    try {
        $status = git submodule status -- "src/proprietary" 2>$null
        return $status
    }
    finally {
        Pop-Location
    }
}

function Enable-ProprietaryModules {
    Write-Info "Enabling proprietary modules..."

    if (-not (Test-IsGitRepo)) {
        Write-ErrorMsg "Current directory is not a Git repository"
        exit 1
    }

    Push-Location $ProjectRoot
    try {
        # Check if submodule is configured
        $submoduleConfig = git config --file .gitmodules --get "submodule.src/proprietary.url" 2>$null
        if (-not $submoduleConfig) {
            Write-ErrorMsg "Proprietary modules submodule not configured, check .gitmodules file"
            exit 1
        }

        # Initialize and update submodule
        Write-Info "Initializing submodule..."
        git submodule update --init --recursive -- "src/proprietary"

        if ($LASTEXITCODE -ne 0) {
            Write-ErrorMsg "Submodule initialization failed"
            exit 1
        }

        # Verify
        if (Test-Path $ProprietaryCommonCmake) {
            Write-Success "Proprietary modules enabled"
            Write-Info "Directory: $ProprietaryDir"

            # List available modules
            $modules = Get-ChildItem -Path $ProprietaryDir -Directory |
                       Where-Object { Test-Path (Join-Path $_.FullName "Kconfig") }

            if ($modules) {
                Write-Info "Available modules:"
                $modules | ForEach-Object {
                    Write-Host "  - $($_.Name)" -ForegroundColor White
                }
            }
        }
        else {
            Write-Warning "Proprietary modules directory exists but files are incomplete"
        }
    }
    finally {
        Pop-Location
    }
}

function Disable-ProprietaryModules {
    Write-Info "Disabling proprietary modules..."

    if (-not (Test-IsGitRepo)) {
        Write-ErrorMsg "Current directory is not a Git repository"
        exit 1
    }

    Push-Location $ProjectRoot
    try {
        # Check if directory exists
        if (-not (Test-Path $ProprietaryDir)) {
            Write-Warning "Proprietary modules directory does not exist, nothing to disable"
            return
        }

        # Check for uncommitted changes
        Push-Location $ProprietaryDir
        try {
            $status = git status --porcelain 2>$null
            if ($status) {
                Write-Warning "Proprietary modules directory has uncommitted changes:"
                Write-Host $status
                $confirm = Read-Host "Continue with disable? (y/N)"
                if ($confirm -ne "y" -and $confirm -ne "Y") {
                    Write-Info "Operation cancelled"
                    return
                }
            }
        }
        finally {
            Pop-Location
        }

        # Remove submodule content (but keep config)
        Write-Info "Removing proprietary modules content..."

        # Use git submodule deinit
        git submodule deinit -f -- "src/proprietary"

        if ($LASTEXITCODE -eq 0) {
            Write-Success "Proprietary modules disabled"
            Write-Info "Project can now compile without proprietary modules"
            Write-Info "To re-enable, run: .\proprietary_manage.ps1 -Enable"
        }
        else {
            Write-ErrorMsg "Disable failed"
            exit 1
        }
    }
    finally {
        Pop-Location
    }
}

function Show-Status {
    Write-Info "Proprietary modules status:"
    Write-Host ""

    # Check if directory exists
    if (-not (Test-Path $ProprietaryDir)) {
        Write-Warning "Proprietary modules directory does not exist (src/proprietary)"
        Write-Info "Status: Not enabled"
        Write-Info "Run '.\proprietary_manage.ps1 -Enable' to enable"
        return
    }

    # Check if key files exist
    if (-not (Test-Path $ProprietaryCommonCmake)) {
        Write-Warning "Proprietary modules directory exists but content is incomplete"
        Write-Info "Status: Needs update"
        Write-Info "Run '.\proprietary_manage.ps1 -Update' to fix"
        return
    }

    Write-Success "Directory: $ProprietaryDir"
    Write-Host ""

    # Get submodule status
    $submoduleStatus = Get-SubmoduleStatus
    if ($submoduleStatus) {
        $statusChar = $submoduleStatus[0]
        switch ($statusChar) {
            ' ' { Write-Info "Git status: Initialized" }
            '-' { Write-Warning "Git status: Not initialized" }
            '+' { Write-Warning "Git status: Updates available" }
            default { Write-Info "Git status: $submoduleStatus" }
        }
    }

    Write-Host ""
    Write-Info "Available modules:"
    Write-Host ""

    $modules = Get-ChildItem -Path $ProprietaryDir -Directory |
               Where-Object { Test-Path (Join-Path $_.FullName "Kconfig") }

    if ($modules) {
        $modules | ForEach-Object {
            $modulePath = $_.FullName
            $kconfigPath = Join-Path $modulePath "Kconfig"

            # Check if it's a submodule
            $isSubmodule = Test-Path (Join-Path $modulePath ".git")
            $submoduleIndicator = if ($isSubmodule) { " [submodule]" } else { "" }

            Write-Host "  $($_.Name)$submoduleIndicator" -ForegroundColor White

            # Try to read module description
            if (Test-Path $kconfigPath) {
                $content = Get-Content $kconfigPath -Raw
                if ($content -match 'menu\s+"([^"]+)"') {
                    Write-Host "    Description: $($Matches[1])" -ForegroundColor Gray
                }
            }
        }
    }
    else {
        Write-Warning "  No proprietary modules found"
    }
}

function Update-ProprietaryModules {
    Write-Info "Updating proprietary modules..."

    if (-not (Test-IsGitRepo)) {
        Write-ErrorMsg "Current directory is not a Git repository"
        exit 1
    }

    Push-Location $ProjectRoot
    try {
        # Update main submodule
        Write-Info "Updating main proprietary module..."
        git submodule update --init --recursive --remote -- "src/proprietary"

        if ($LASTEXITCODE -ne 0) {
            Write-ErrorMsg "Update failed"
            exit 1
        }

        # Update internal submodules
        if (Test-Path $ProprietaryDir) {
            Push-Location $ProprietaryDir
            try {
                if (Test-Path ".gitmodules") {
                    Write-Info "Updating internal submodules..."
                    git submodule update --init --recursive --remote
                }
            }
            finally {
                Pop-Location
            }
        }

        Write-Success "Proprietary modules updated"
    }
    finally {
        Pop-Location
    }
}

# Main logic
if ($Help -or (-not ($Enable -or $Disable -or $Status -or $Update))) {
    Show-Help
    exit 0
}

if ($Enable) {
    Enable-ProprietaryModules
}
elseif ($Disable) {
    Disable-ProprietaryModules
}
elseif ($Status) {
    Show-Status
}
elseif ($Update) {
    Update-ProprietaryModules
}
