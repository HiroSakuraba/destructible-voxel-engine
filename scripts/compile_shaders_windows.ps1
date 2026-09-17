param(
    [string]$Dxc = "dxc.exe",
    [string]$OutputDirectory = "build/shaders"
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
& $Dxc -T cs_6_0 -E main -O3 -Ges -WX `
    -Fo "$OutputDirectory/primary_brickmap_trace.dxil" `
    "shaders/primary_brickmap_trace.hlsl"
& $Dxc -T cs_6_0 -E main -O3 -Ges -WX `
    -Fo "$OutputDirectory/brick_face_extract.dxil" `
    "shaders/brick_face_extract.hlsl"
Write-Host "Compiled DVE SM 6.0 shaders into $OutputDirectory"
