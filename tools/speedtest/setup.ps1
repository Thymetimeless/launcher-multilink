# Fetches Plexo's source at the commit the speed test was written against, then builds dist\speedtest.mjs.
# Usage: powershell -ExecutionPolicy Bypass -File setup.ps1   (needs git and Node.js 20+)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$commit = 'ec6dc5d7548c24c31c96e356db4a723ae861a01e'
if (-not (Test-Path plexo\.git)) { git clone -q https://github.com/anmolkapil/plexo plexo }
git -C plexo fetch -q origin $commit
git -C plexo checkout -q $commit
npm install --no-audit --no-fund
npm run build
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
'Ready: node dist\speedtest.mjs'
