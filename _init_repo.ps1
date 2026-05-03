$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Test-Path .git)) {
    git init -b master
}

git add -A

git commit -m "Initial commit: ESP-IDF matrix apps, README, gitignore" 2>$null
if ($LASTEXITCODE -ne 0) {
    git commit -m "Initial commit: ESP-IDF matrix apps, README, gitignore"
}

$remoteUrl = 'git@github.com:jinweijie/esp32-s3-matrix.git'
$hasOrigin = git remote get-url origin 2>$null
if ($LASTEXITCODE -ne 0) {
    git remote add origin $remoteUrl
} else {
    git remote set-url origin $remoteUrl
}

git branch -M master
git push -u origin master
