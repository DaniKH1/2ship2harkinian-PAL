$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$submodulePath = Join-Path $repositoryRoot 'libultraship'
$patchPath = Join-Path $repositoryRoot 'patches\libultraship\0001-pal-gui-textures-and-controller-defaults.patch'

function Test-PatchApplication([bool]$Reverse) {
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($Reverse) {
            $output = & git -C $submodulePath apply --reverse --check $patchPath 2>&1
        } else {
            $output = & git -C $submodulePath apply --check $patchPath 2>&1
        }
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $submodulePath '.git'))) {
    throw "libultraship is not initialized. Run 'git submodule update --init libultraship' first."
}

$applyCheckExitCode = Test-PatchApplication $false
if ($applyCheckExitCode -eq 0) {
    $applyOutput = & git -C $submodulePath apply $patchPath 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to apply the PAL libultraship patch.'
    }
    Write-Host 'Applied the PAL libultraship patch.'
    exit 0
}

$reverseCheckExitCode = Test-PatchApplication $true
if ($reverseCheckExitCode -eq 0) {
    Write-Host 'The PAL libultraship patch is already applied.'
    exit 0
}

throw 'The PAL libultraship patch does not apply to this submodule revision.'
