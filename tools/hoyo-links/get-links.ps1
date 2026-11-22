# Writes the official full-package links of a HoYoverse game (global) from the HoYoPlay API,
# ready to paste into a download manager such as Plexo, plus md5.txt for verify.ps1.
# Usage: powershell -ExecutionPolicy Bypass -File get-links.ps1 [-Game hkrpg|hk4e|nap|bh3] [-Audio en-us,ja-jp] [-Index 0]
#   hkrpg = Honkai: Star Rail, hk4e = Genshin Impact, nap = Zenless Zone Zero, bh3 = Honkai Impact 3rd
#   -Index picks one entry when the API lists several (Honkai Impact 3rd has one per region).
param(
    [ValidateSet('hkrpg', 'hk4e', 'nap', 'bh3')][string]$Game = 'hkrpg',
    [string[]]$Audio = @('en-us'),
    [int]$Index = 0
)
$ErrorActionPreference = 'Stop'
$api = 'https://sg-hyp-api.hoyoverse.com/hyp/hyp-connect/api/getGamePackages?launcher_id=VYTpXlbWo8'
$r = Invoke-RestMethod $api -TimeoutSec 20
if ($r.retcode -ne 0) { throw "API retcode $($r.retcode): $($r.message)" }
$entries = @($r.data.game_packages | Where-Object { $_.game.biz -eq "${Game}_global" })
if ($entries.Count -eq 0) { throw "${Game}_global not in the API answer" }
if ($Index -lt 0 -or $Index -ge $entries.Count) { throw "-Index must be 0..$($entries.Count - 1)" }
if ($entries.Count -gt 1) {
    for ($i = 0; $i -lt $entries.Count; $i++) { "  [$i] $((@($entries[$i].main.major.game_pkgs)[0].url) -replace '/[^/]*$', '')" }
}
$major = $entries[$Index].main.major

$audioPkgs = @($major.audio_pkgs | Where-Object { $Audio -contains $_.language })
if ($major.audio_pkgs) {
    $missing = $Audio | Where-Object { $_ -notin $audioPkgs.language }
    if ($missing) { throw "No audio pack for: $($missing -join ', ') (available: $(($major.audio_pkgs.language) -join ', '))" }
}
$pkgs = @($major.game_pkgs) + $audioPkgs

$dir = $PSScriptRoot
$pkgs.url | Set-Content -Encoding ascii "$dir\links.txt"
$pkgs | ForEach-Object { "$($_.md5.ToLower()) $([int64]$_.size) $(Split-Path $_.url -Leaf)" } | Set-Content -Encoding ascii "$dir\md5.txt"

$total = ($pkgs | Measure-Object size -Sum).Sum
"$Game $($major.version): $($pkgs.Count) files, {0:N1} GB  (audio: $(if ($audioPkgs) { $audioPkgs.language -join ', ' } else { 'none' }))" -f ($total / 1GB)
"Wrote $dir\links.txt and $dir\md5.txt"
