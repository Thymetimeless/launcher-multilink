# Checks the downloaded files against md5.txt (written by get-links.ps1).
# Usage: powershell -ExecutionPolicy Bypass -File verify.ps1 -Folder <Plexo download folder>
param([Parameter(Mandatory)][string]$Folder)
$ErrorActionPreference = 'Stop'
$bad = 0
foreach ($line in Get-Content "$PSScriptRoot\md5.txt") {
    $md5, $size, $name = $line -split ' ', 3
    $path = Join-Path $Folder $name
    if (-not (Test-Path -LiteralPath $path)) { "MISSING  $name"; $bad++; continue }
    $len = (Get-Item -LiteralPath $path).Length
    if ($len -ne [int64]$size) { "SIZE     $name  ($len, expected $size)"; $bad++; continue }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm MD5).Hash.ToLower()
    if ($hash -ne $md5) { "BAD MD5  $name"; $bad++ } else { "OK       $name" }
}
if ($bad) { "$bad file(s) need re-downloading"; exit 1 } else { 'All files OK' }
