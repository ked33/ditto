$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($visualStudio)) {
    throw 'Visual C++ build tools were not found.'
}

$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$buildDirectory = Join-Path $tempRoot "ditto-popup-menu-tests-$PID"
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$commandFile = Join-Path $buildDirectory 'build.cmd'
@"
@echo off
chcp 65001 >nul
call "$visualStudio\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
cd /d "$buildDirectory"
cl /nologo /EHsc /std:c++17 /W4 /WX /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 "$repoRoot\tests\themed_popup_menu_smoke.cpp" "$repoRoot\src\ThemedPopupMenu.cpp" /Fe:themed_popup_menu_smoke.exe /link user32.lib gdi32.lib comctl32.lib
if errorlevel 1 exit /b %errorlevel%
themed_popup_menu_smoke.exe
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $commandFile -Encoding utf8NoBOM

& $env:ComSpec /d /c "call `"$commandFile`""
if ($LASTEXITCODE -ne 0) {
    throw "Themed popup menu checks failed with exit code $LASTEXITCODE."
}
