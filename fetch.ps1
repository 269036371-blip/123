#requires -Version 5.1
<#
  fetch.ps1 - robust multi-source downloader
  Usage: powershell -ExecutionPolicy Bypass -File fetch.ps1 -Sources "u1;u2;u3" -Out "path"
  Output is pure ASCII so it survives any console codepage.
  Exit 0 = file present and non-empty, 1 = all sources failed.
#>
param(
    [Parameter(Mandatory = $true)][string]$Sources,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Continue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Say($m) { Write-Host ("  " + $m) }

$list = @($Sources -split ';' | Where-Object { $_ -ne '' })
if ($list.Count -eq 0) { Say "no source given"; exit 1 }

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path -LiteralPath $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

# Reuse a previously downloaded file (resume / offline rerun)
if (Test-Path -LiteralPath $Out) {
    $len0 = (Get-Item -LiteralPath $Out).Length
    if ($len0 -gt 0) {
        Say ("reuse cached file ({0} MB)" -f [math]::Round($len0 / 1MB, 1))
        exit 0
    }
    Remove-Item -LiteralPath $Out -Force -ErrorAction SilentlyContinue
}

function Done {
    if (Test-Path -LiteralPath $Out) {
        $len = (Get-Item -LiteralPath $Out).Length
        if ($len -gt 0) {
            Say ("downloaded {0} MB" -f [math]::Round($len / 1MB, 1))
            return $true
        }
    }
    return $false
}

$curl = Get-Command curl.exe -ErrorAction SilentlyContinue

foreach ($u in $list) {
    Say ("source: " + $u)

    if ($curl) {
        $q = '"' + $Out + '"'
        $variants = @(
            "-L --retry 5 --retry-all-errors --retry-delay 2 --ssl-no-revoke --max-time 900 -C - -o $q",
            "-L --retry 3 --retry-delay 1 --ssl-no-revoke --max-time 900 -o $q",
            "-L --retry 3 --noproxy * --ssl-no-revoke --max-time 900 -o $q"
        )
        foreach ($v in $variants) {
            $raw = & cmd /c "curl.exe $v ""$u"" 2>&1"
            if ($LASTEXITCODE -eq 0 -or (Done)) {
                if (Done) { exit 0 }
            }
            $txt = ($raw | ForEach-Object { $_.ToString() }) -join " "
            if ($txt -match 'curl: \(\d+\)') {
                Say ("curl: " + ($Matches[0]))
            }
            if (Test-Path -LiteralPath $Out) {
                Remove-Item -LiteralPath $Out -Force -ErrorAction SilentlyContinue
            }
        }
    }

    # BITS fallback
    try {
        $bits = Get-Command Start-BitsTransfer -ErrorAction SilentlyContinue
        if ($bits) {
            Start-BitsTransfer -Source $u -Destination $Out -ErrorAction Stop
            if (Done) { exit 0 }
        }
    } catch {
        Say ("bits: " + $_.Exception.Message)
    }

    # WebClient fallback (synchronous so errors are catchable)
    try {
        $wc = New-Object Net.WebClient
        $wc.Headers.Add("User-Agent", "KeyMacro-installer")
        $wc.DownloadFile($u, $Out)
        if (Done) { exit 0 }
    } catch {
        Say ("webclient: " + $_.Exception.Message)
    }

    if (Test-Path -LiteralPath $Out) {
        Remove-Item -LiteralPath $Out -Force -ErrorAction SilentlyContinue
    }
}

Say "FAILED: all sources exhausted"
exit 1
