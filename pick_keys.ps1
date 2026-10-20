#requires -Version 5.1
<#
  pick_keys.ps1 - write the key list into KeyMacro.ini
  Usage: powershell -ExecutionPolicy Bypass -File pick_keys.ps1 -Keys "1 2 3 4 5 C E R G J K"
  Keys live in the ini, so changing them never requires a recompile.
#>
param(
    [Parameter(Mandatory = $true)][string]$Keys
)

$ErrorActionPreference = 'Stop'

$app = $env:APPDATA
if (-not $app) { Write-Host "  ERROR: APPDATA missing"; exit 1 }

$dir = Join-Path $app 'KeyMacro'
if (-not (Test-Path -LiteralPath $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}
$ini = Join-Path $dir 'KeyMacro.ini'

$keysLine = "list = " + $Keys
$utf8 = New-Object Text.UTF8Encoding($false)

if (Test-Path -LiteralPath $ini) {
    $lines = Get-Content -LiteralPath $ini -Encoding UTF8
    $out   = New-Object Collections.Generic.List[string]
    $inKeys = $false
    $found  = $false
    foreach ($l in $lines) {
        if ($l -match '^\s*\[keys\]') {
            $inKeys = $true
            $found  = $true
            $out.Add('[keys]')
            $out.Add($keysLine)
            continue
        }
        if ($l -match '^\s*\[') {
            $inKeys = $false
            $out.Add($l)
            continue
        }
        if ($inKeys -and $l -match '^\s*list\s*=') { continue }
        $out.Add($l)
    }
    if (-not $found) {
        $out.Add('')
        $out.Add('[keys]')
        $out.Add($keysLine)
    }
    [IO.File]::WriteAllLines($ini, $out, $utf8)
} else {
    $content = @(
        '; KeyMacro config'
        '; Author: AYJY'
        '; Edit and use tray -> Reload config. No restart, no recompile.'
        ''
        '[keys]'
        $keysLine
        ''
        '[timing]'
        'interval_ms = 5'
        'press_ms = 4'
    )
    [IO.File]::WriteAllLines($ini, $content, $utf8)
}

Write-Host ("  keys -> " + $Keys)
exit 0
