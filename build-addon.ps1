param(
    [Parameter(Mandatory=$true)][string]$GameScrcpySource,
    [Parameter(Mandatory=$true)][string]$GameScrcpyInstall,
    [string]$Zig = 'zig',
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$addon = Join-Path $PSScriptRoot 'harmony-addon'
$ffmpeg = Join-Path (Resolve-Path -LiteralPath $GameScrcpySource) 'third_party\ffmpeg'
$game = (Resolve-Path -LiteralPath $GameScrcpyInstall).Path
$zigExe = (Get-Command $Zig -CommandType Application).Source
$zigRoot = Split-Path -Parent $zigExe
$build = Join-Path $PSScriptRoot ('.build\' + [guid]::NewGuid().ToString('N'))
$portable = Join-Path $build 'portable'
$native = Join-Path $portable 'addon'
$licenses = Join-Path $native 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
# Use relative compiler input paths and strip build-machine debug information.
Push-Location -LiteralPath $PSScriptRoot
try {
    & $zigExe c++ -std=c++20 -O2 -g0 -s -shared -target x86_64-windows-gnu -Iharmony-addon/include "-I$ffmpeg\include" harmony-addon/src/harmony.cpp "$ffmpeg\lib\avcodec.lib" "$ffmpeg\lib\avutil.lib" -lws2_32 -o "$build\native.dll"
    if ($LASTEXITCODE) { throw 'Native extension build failed' }
} finally { Pop-Location }
Copy-Item -LiteralPath "$build\native.dll" -Destination $native
Copy-Item -LiteralPath "$addon\manifest.json" -Destination $native
New-Item -ItemType Directory -Path "$native\tests" | Out-Null
Copy-Item -LiteralPath "$addon\tests\basic.json" -Destination "$native\tests"
Copy-Item -LiteralPath "$PSScriptRoot\LICENSE","$PSScriptRoot\NOTICE.md" -Destination $native
Copy-Item -LiteralPath "$zigRoot\LICENSE" -Destination "$licenses\Zig.txt"
Copy-Item -LiteralPath "$zigRoot\lib\libc\mingw\COPYING" -Destination "$licenses\mingw-w64.txt"
foreach ($library in 'libcxx','libcxxabi','libunwind') {
    Copy-Item -LiteralPath "$zigRoot\lib\$library\LICENSE.TXT" -Destination "$licenses\$library.txt"
}
foreach ($name in 'portable.py','Harmony-USB.cmd','bridge.py','studio_bridge.py','LICENSE','NOTICE.md','README.md') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $portable
}
New-Item -ItemType Directory -Path "$portable\studio-bridge" | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot\studio-bridge\HarmonyBridge.java","$PSScriptRoot\studio-bridge\HosLauncher.java" -Destination "$portable\studio-bridge"
$localPackage = Join-Path $PSScriptRoot 'harmony-preview.gsext'
& $Python -B "$portable\portable.py" --prepare --game "$game\GameScrcpy.exe" --output $localPackage
if ($LASTEXITCODE) { throw 'Preparing or validating the local package failed' }
$asset = Join-Path $PSScriptRoot 'GameScrcpy-Harmony-Windows-x64.zip'
Compress-Archive -Path "$portable\*" -DestinationPath $asset -Force
Write-Output $asset
