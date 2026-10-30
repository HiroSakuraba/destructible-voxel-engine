param(
    [Parameter(Mandatory=$true)][string]$Model,
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../out/build/windows-msvc-player-release/Release'),
    [string]$DependencyBin = ''
)
$ErrorActionPreference = 'Stop'
$modelPath = (Resolve-Path -LiteralPath $Model).Path
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$projectPath = [System.IO.Path]::GetFullPath($Project)
if (Test-Path -LiteralPath (Join-Path $projectPath 'game.dvegame')) { throw 'Project already exists; choose a new directory.' }
foreach ($name in @('dve_cook_mesh.exe','dve_cook_model.exe','dve_player.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildPath $name))) { throw "Build $name first: $buildPath" }
}
if ($DependencyBin) { $env:PATH = (Resolve-Path -LiteralPath $DependencyBin).Path + ';' + $env:PATH }
New-Item -ItemType Directory -Force -Path (Join-Path $projectPath 'scenes'),(Join-Path $projectPath 'scripts') | Out-Null
& (Join-Path $buildPath 'dve_cook_mesh.exe') $modelPath --output (Join-Path $projectPath 'scenes/sponza.dmesh') --object-id 7101
if ($LASTEXITCODE -ne 0) { throw "Mesh import failed ($LASTEXITCODE)." }
& (Join-Path $buildPath 'dve_cook_model.exe') $modelPath --output (Join-Path $projectPath 'scenes/sponza-voxels.dvox') --mode surface --voxel-size 0.25 --max-working-voxels 32000000 --object-id 7102
if ($LASTEXITCODE -ne 0) { throw "Voxel proxy import failed ($LASTEXITCODE)." }
$template = Join-Path $PSScriptRoot '../tests/data/sponza_lighting'
foreach ($name in @('game.dvegame','game_settings.txt','scenes/main.dvoxscene.json','scripts/main.lua')) {
    Copy-Item -LiteralPath (Join-Path $template $name) -Destination (Join-Path $projectPath $name)
}
$player = Join-Path $buildPath 'dve_player.exe'
$launch = @(
    '@echo off', 'setlocal', 'set "DVE_SPLIT_RC_EXPERIMENT=1"', 'set "DVE_SPLIT_RC_PROXY_NAME=SponzaGIProxy"',
    'echo First frame may take a minute. G compares split and ambient lighting.',
    'echo Q/E rotate the sun. Z/X change its height. R resets it.',
    ('"{0}" --project "{1}" --free-camera --renderer vulkan --sky physical --ao screen --gi cascades --size 1280x720 --render-size 1280x720 --no-audio' -f $player,$projectPath),
    'if errorlevel 1 pause'
)
Set-Content -LiteralPath (Join-Path $projectPath 'Run Sponza Lighting Test.bat') -Value $launch -Encoding ascii
Write-Host "Prepared Sponza lighting project at $projectPath"
