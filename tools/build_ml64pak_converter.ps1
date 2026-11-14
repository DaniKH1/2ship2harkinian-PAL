param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'ml64pak_converter'
$buildRoot = Join-Path $env:TEMP ('2ship-ml64pak-build-' + [guid]::NewGuid().ToString('N'))
$dist = Join-Path $buildRoot 'dist'
$work = Join-Path $buildRoot 'work'
$spec = Join-Path $buildRoot 'spec'
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $output = Join-Path $PSScriptRoot 'ML64PakConverter.exe'
} else {
    $output = [System.IO.Path]::GetFullPath($OutputPath)
    $outputDirectory = Split-Path -Parent $output
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

New-Item -ItemType Directory -Path $buildRoot | Out-Null
try {
    & python -c "import PyInstaller, numpy, PIL"
    if ($LASTEXITCODE -ne 0) {
        throw 'Python build dependencies are missing. Install them with: python -m pip install -r tools\ml64pak_converter\requirements-build.txt'
    }

    & python -m PyInstaller --noconfirm --clean --onefile --console `
        --name ML64PakConverter `
        --paths $source `
        --collect-submodules ml64_to_soh `
        --hidden-import PIL.ImageTk `
        --distpath $dist --workpath $work --specpath $spec `
        (Join-Path $source 'convert_pak.py')
    if ($LASTEXITCODE -ne 0) {
        throw "PyInstaller failed with exit code $LASTEXITCODE."
    }

    $builtExe = Join-Path $dist 'ML64PakConverter.exe'
    if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
        throw 'PyInstaller reported success but did not produce ML64PakConverter.exe.'
    }

    Copy-Item -LiteralPath $builtExe -Destination $output -Force
    Write-Output "Built $output"
}
finally {
    if (Test-Path -LiteralPath $buildRoot) {
        Remove-Item -LiteralPath $buildRoot -Recurse -Force
    }
}
