# Installs CameraShake.ofx.bundle into VEGAS Pro's OFX plug-in folder.
# Needs administrator rights because the target is under Program Files.
# Usage (elevated):  powershell -ExecutionPolicy Bypass -File install.ps1
[CmdletBinding()]
param(
    # Override if your VEGAS install lives elsewhere.
    [string]$VegasOfxDir,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$bundleName = 'CameraShake.ofx.bundle'
$source = Join-Path $PSScriptRoot "build\$bundleName"

if (-not $VegasOfxDir) {
    $candidates = @(
        'C:\Program Files\BorisFX\Vegas Pro 2026\OFX Video Plug-Ins',
        'C:\Program Files\VEGAS\VEGAS Pro 22.0\OFX Video Plug-Ins',
        'C:\Program Files\Common Files\OFX\Plugins'
    ) + (Get-ChildItem 'C:\Program Files\BorisFX','C:\Program Files\VEGAS' -Directory -ErrorAction SilentlyContinue |
         ForEach-Object { Join-Path $_.FullName 'OFX Video Plug-Ins' })

    $VegasOfxDir = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $VegasOfxDir) { throw "Could not find a VEGAS 'OFX Video Plug-Ins' folder. Pass -VegasOfxDir explicitly." }

$dest = Join-Path $VegasOfxDir $bundleName

if ($Uninstall) {
    if (Test-Path $dest) {
        Remove-Item $dest -Recurse -Force
        Write-Host "Removed $dest"
    } else {
        Write-Host "Nothing to remove at $dest"
    }
    return
}

if (-not (Test-Path $source)) { throw "Build output not found at $source - run build.bat first." }

if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
Copy-Item $source $dest -Recurse -Force

Write-Host "Installed to $dest"
Get-ChildItem $dest -Recurse -File | ForEach-Object { "  " + $_.FullName.Substring($dest.Length + 1) }
Write-Host ""
Write-Host "Restart VEGAS Pro, then look for 'Camera Shake' in Video FX (group: Distort)."
