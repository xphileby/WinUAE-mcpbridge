# Sync a fresh release into the plugin submodule (plugin/).
#
# Source of truth lives in THIS repo:
#   - tools/mcp-winuae-bridge.py  -> plugin/server/
#   - out/winuae64.exe            -> plugin/bin/   (build FullRelease x64 first!)
#
# Usage:  .\tools\release-plugin.ps1 [-Version 1.0.1]
# Then review, commit inside plugin/, push, and bump the submodule ref here.

param([string]$Version = "")

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$plugin = Join-Path $root "plugin"

if (-not (Test-Path "$plugin\.claude-plugin\plugin.json")) {
    throw "plugin submodule not initialized: run 'git submodule update --init'"
}

$exe = Join-Path $root "out\winuae64.exe"
if (-not (Test-Path $exe)) { throw "out\winuae64.exe not found - build FullRelease x64 first" }
# Warn if the binary looks like a Test build (Test x64 is ~46MB, FullRelease ~28MB).
$sizeMB = [math]::Round((Get-Item $exe).Length / 1MB, 1)
if ($sizeMB -gt 35) {
    Write-Warning "out\winuae64.exe is ${sizeMB}MB - this looks like a Test build, not FullRelease."
}

Copy-Item (Join-Path $root "tools\mcp-winuae-bridge.py") (Join-Path $plugin "server\") -Force
Copy-Item $exe (Join-Path $plugin "bin\") -Force
Write-Host "copied relay + winuae64.exe (${sizeMB}MB) into plugin/"

if ($Version) {
    $pj = Join-Path $plugin ".claude-plugin\plugin.json"
    $json = Get-Content $pj -Raw | ConvertFrom-Json
    $json.version = $Version
    $json | ConvertTo-Json -Depth 8 | Set-Content $pj -Encoding utf8
    Write-Host "plugin.json version -> $Version"
}

Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  cd plugin"
Write-Host "  git add -A; git commit -m 'Release <version>'; git push"
Write-Host "  cd ..; git add plugin; git commit -m 'Bump plugin submodule'; git push"
