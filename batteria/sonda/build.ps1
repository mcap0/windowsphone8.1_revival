# build.ps1 (runs in the VM, from C:\lumia\src\sonda) — native ARM exe for WP8.1: VS2015 cross compiler, static CRT,
# import libraries generated from the phone's own DLL exports (defs\*.def), never the desktop kernel32.lib.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$vc = 'C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC'
cmd /c "`"$vc\vcvarsall.bat`" x86_arm && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
New-Item -ItemType Directory -Force libs, obj, out | Out-Null
Remove-Item obj\*.obj, libs\*.lib, out\sonda.exe -Force -ErrorAction SilentlyContinue
Get-ChildItem defs\*.def | ForEach-Object {
  & lib.exe /nologo /machine:arm /def:$($_.FullName) /out:libs\$($_.BaseName).lib 2>&1 | Select-Object -Last 4
  if ($LASTEXITCODE -ne 0) { "IMPORT LIBRARY FAILED: $($_.Name), exit $LASTEXITCODE"; exit 1 }
}
$cl = '/nologo','/c','/O2','/MT','/EHsc','/W3','/GS','/Zi','/DUNICODE','/D_UNICODE','/D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1','/D_CRT_SECURE_NO_WARNINGS','/Fdobj/','/Foobj/'
$localSources = @(Get-ChildItem src\*.cpp | ForEach-Object { $_.FullName })
& cl.exe @cl $localSources 2>&1 | Select-Object -Last 40
$compileExit = $LASTEXITCODE
if ($compileExit -ne 0) { "COMPILE EXIT $compileExit"; exit 1 }
$objs = @(Get-ChildItem obj\*.obj | ForEach-Object { $_.FullName })
$libs = 'libcmt.lib','libcpmt.lib','libvcruntime.lib','libucrt.lib','libs\kernelbase.lib','libs\kernel32legacy.lib','libs\sechost.lib','libs\ws2_32.lib','libs\ntdll.lib','libs\cfgmgr32.lib'
& link.exe /nologo /SUBSYSTEM:CONSOLE /MACHINE:ARM /NODEFAULTLIB /DEBUG /OPT:REF /OUT:out\sonda.exe $objs $libs 2>&1 | Select-Object -Last 40
$linkExit = $LASTEXITCODE
if ($linkExit -ne 0) { "LINK EXIT $linkExit"; exit 1 }
# Authenticode signature with our own DumbPhone certificate (the phone's loader may reject a file with no signature at all)
$signtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\signtool.exe'
if (-not (Test-Path out\sonda.exe)) { "LINK OUTPUT MISSING"; exit 1 }
$ErrorActionPreference = 'Continue'
& $signtool sign /q /fd sha256 /f ..\DumbPhone.pfx out\sonda.exe 2>&1 | Select-Object -Last 5
# Over SSH without a console logon the pfx key is locked; the rooted phone runs the unsigned exe too.
if ($LASTEXITCODE -ne 0) { "UNSIGNED (sign exit $LASTEXITCODE)" }
"EXE " + (Get-Item out\sonda.exe).Length
