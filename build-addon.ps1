param(
    [Parameter(Mandatory=$true)][string]$GameScrcpySource,
    [Parameter(Mandatory=$true)][string]$GameScrcpyInstall,
    [string]$Zig = 'zig'
)
$ErrorActionPreference = 'Stop'
$addon = Join-Path $PSScriptRoot 'harmony-addon'
$ffmpeg = Join-Path (Resolve-Path -LiteralPath $GameScrcpySource) 'third_party\ffmpeg'
$game = (Resolve-Path -LiteralPath $GameScrcpyInstall).Path
$package = Join-Path $PSScriptRoot 'harmony-preview.gsext'
& $Zig c++ -std=c++20 -O2 -shared -target x86_64-windows-gnu "-I$addon\include" "-I$ffmpeg\include" "$addon\src\harmony.cpp" "$ffmpeg\lib\avcodec.lib" "$ffmpeg\lib\avutil.lib" -lws2_32 -o "$addon\native.dll"
if ($LASTEXITCODE) { throw 'Native extension build failed' }
foreach ($name in @('avcodec-63.dll','avutil-61.dll','swresample-7.dll','vcruntime140.dll')) {
    Copy-Item -LiteralPath (Join-Path $game $name) -Destination $addon
}
$notices = Join-Path $addon 'runtime-licenses'
New-Item -ItemType Directory -Path $notices -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $game 'licenses') -File | Copy-Item -Destination $notices -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE'),(Join-Path $PSScriptRoot 'NOTICE.md') -Destination $addon -Force
& "$game\gs.exe" validate $addon
if ($LASTEXITCODE) { throw 'Manifest validation failed' }
& "$game\gs.exe" ext test $addon --hosted
if ($LASTEXITCODE) { throw 'Hosted extension test failed' }
& "$game\gs.exe" pack $addon -o $package
if ($LASTEXITCODE) { throw 'Packaging failed' }
