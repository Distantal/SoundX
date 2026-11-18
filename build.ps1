param(
    [string]$Config = "Release",
    [switch]$Configure,
    [switch]$Run
)

$ErrorActionPreference = "Stop"
$vs = "E:\VSBuildTools"
$cmakeDir = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
$root = $PSScriptRoot

# import the MSVC x64 environment into this PowerShell session
$envDump = cmd /c "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process") }
}
$env:Path = "$cmakeDir\CMake\bin;$cmakeDir\Ninja;" + $env:Path

# Keep the C++ runtime that SoundX.exe links against next to the packaged program: the installer
# embeds dist\runtime\*.dll and writes them beside the executable (app-local deployment), so a
# machine without the "Microsoft Visual C++ 2015-2022 Redistributable" can still run SoundX.
if ($env:VCToolsRedistDir) {
    $crtDir = Get-ChildItem -Directory (Join-Path $env:VCToolsRedistDir "x64") -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
              Select-Object -Last 1
    if ($crtDir) {
        $runtime = "$root\dist\runtime"
        New-Item -ItemType Directory -Force $runtime | Out-Null
        foreach ($dll in @("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
            $src = Join-Path $crtDir.FullName $dll
            if (Test-Path $src) { Copy-Item $src $runtime -Force }
        }
    }
}

$build = "$root\build"
if ($Configure -or -not (Test-Path "$build\build.ninja")) {
    cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DSOUNDX_VST2=ON -DSOUNDX_ASIO=ON
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Run) { & "$build\SoundX_artefacts\$Config\SoundX.exe" }
