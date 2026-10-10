@echo off
setlocal
set "sourceRoot=%~dp0"
set "buildRoot=%~1"
if "%buildRoot%"=="" set "buildRoot=%sourceRoot%..\..\.build\0.0.7-alpha"
for %%I in ("%buildRoot%") do set "buildRoot=%%~fI"
set "packageDirectory=%~2"
if "%packageDirectory%"=="" set "packageDirectory=%buildRoot%\packages"
for %%I in ("%packageDirectory%") do set "packageDirectory=%%~fI"
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%I in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "visualStudio=%%I"
if not defined visualStudio exit /b 1
call "%visualStudio%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
if not exist "%buildRoot%" mkdir "%buildRoot%"
cd /d "%sourceRoot%"
powershell -NoProfile -ExecutionPolicy Bypass -File localization\generate.ps1
if errorlevel 1 exit /b 1
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; [xml]$packages=Get-Content -LiteralPath 'winui_manager/packages.config'; foreach($package in $packages.packages.package){if(!(Test-Path -LiteralPath (Join-Path $env:packageDirectory ($package.id+'.'+$package.version)))){exit 1}}"
if errorlevel 1 (
    call :RestorePackages
    if errorlevel 1 exit /b 1
)
"%visualStudio%\MSBuild\Current\Bin\MSBuild.exe" winui_manager\G3Companion.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 "/p:BuildRoot=%buildRoot%" "/p:NugetPackageDirectory=%packageDirectory%" /v:minimal /nologo /fl "/flp:logfile=%buildRoot%\build.log;verbosity=normal"
if errorlevel 1 exit /b 1
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $output=Join-Path $env:buildRoot 'winui\G3Companion'; $destination=Split-Path -Parent $env:sourceRoot.TrimEnd('\'); @('G3-Companion.exe','resources.pri','Microsoft.WindowsAppRuntime.Bootstrap.dll','G3Companion.winmd') | ForEach-Object { Copy-Item -LiteralPath (Join-Path $output $_) -Destination (Join-Path $destination $_) -Force }"
exit /b %errorlevel%

:RestorePackages
if not exist "%buildRoot%\nuget.exe" powershell -NoProfile -Command "$ErrorActionPreference='Stop'; Invoke-WebRequest -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile (Join-Path $env:buildRoot 'nuget.exe')"
if errorlevel 1 exit /b 1
"%buildRoot%\nuget.exe" restore winui_manager\packages.config -PackagesDirectory "%packageDirectory%" -Source https://api.nuget.org/v3/index.json -NonInteractive
exit /b %errorlevel%
