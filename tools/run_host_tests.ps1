# Builds and runs the host unit tests (tests/host) with MSVC + AddressSanitizer.
# Needs Visual Studio 2022 Build Tools (C++ workload) and the ESP-IDF tools' CMake and Ninja.
#   powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 [-Examples]
param([switch]$Examples)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs) { $vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs; install VS Build Tools with the C++ workload" }

$tools = Join-Path $env:USERPROFILE ".espressif\tools"
$cmake = Get-ChildItem $tools -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
$ninja = Get-ChildItem $tools -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $cmake -or -not $ninja) { throw "cmake/ninja not found under $tools" }

$build = Join-Path $root "build\host"
$run = if ($Examples) { "`"$build\slp_examples.exe`"" } else { "`"$build\host_tests.exe`"" }
$cmd = "call `"$vcvars`" >nul 2>&1 && `"$cmake`" -S `"$root\tests\host`" -B `"$build`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=Debug >nul && `"$cmake`" --build `"$build`" && $run"
cmd /c $cmd
exit $LASTEXITCODE
