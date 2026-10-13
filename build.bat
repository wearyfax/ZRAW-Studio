@echo off
rem Builds bin\ZRAW Studio.exe, a single portable exe that carries the zrawdng engine, zrawcore.dll
rem (previews and thumbnails), zraw.dll (the mpv plugin) and ZrawIcon.dll (the Explorer add-on).
rem Needs the Visual Studio Build Tools with the C++ workload. The C# side is compiled with the
rem .NET Framework 4.x compiler that ships with Windows, so it is written in C# 5.
setlocal
cd /d "%~dp0"
if not exist obj mkdir obj
if not exist bin mkdir bin

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS echo Visual Studio Build Tools with the C++ workload not found & exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

rem Engine: converter, preview library and mpv plugin
set FLAGS=/nologo /O2 /EHsc /MT /std:c++17 /arch:AVX2 /W3 /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4018 /wd4996 /Ithird_party /Ithird_party\mpv\include /Foobj\
cl %FLAGS% engine\main.cpp /Fe:bin\zrawdng.exe || exit /b 1
cl %FLAGS% /LD engine\zrawcore.cpp /Fe:bin\zrawcore.dll /link /IMPLIB:obj\zrawcore.lib || exit /b 1
cl %FLAGS% /LD engine\mpv_plugin.cpp /Fe:bin\zraw.dll /link /IMPLIB:obj\zraw.lib || exit /b 1
rem zrawdng play needs libmpv-2.dll next to it; ZRAW Studio itself does not
if exist third_party\mpv\libmpv-2.dll copy /y third_party\mpv\libmpv-2.dll bin\ >nul

rem Explorer add-on (icons, thumbnails and the preview pane for ZRAW clips)
rc /nologo /fo obj\ZrawIcon.res explorer\ZrawIcon.rc || exit /b 1
cl /nologo /O2 /MT /LD /W3 /EHsc /std:c++17 /utf-8 /Foobj\ explorer\ZrawIcon.cpp obj\ZrawIcon.res /link /DEF:explorer\ZrawIcon.def /OUT:obj\ZrawIcon.dll /IMPLIB:obj\ZrawIcon.lib || exit /b 1

rem ReaLTaiizor 3.8.2.1 (MIT), the UI controls, from NuGet
if not exist obj\ReaLTaiizor.dll (
  powershell -NoProfile -Command "Invoke-WebRequest https://www.nuget.org/api/v2/package/ReaLTaiizor/3.8.2.1 -OutFile obj\ReaLTaiizor.zip; Expand-Archive obj\ReaLTaiizor.zip obj\ReaLTaiizor -Force; Copy-Item obj\ReaLTaiizor\lib\net48\ReaLTaiizor.dll obj\ReaLTaiizor.dll" || exit /b 1
)

rem Gzip the payloads that ride inside the exe
powershell -NoProfile -Command "foreach ($f in 'obj\ReaLTaiizor.dll','bin\zrawdng.exe','bin\zrawcore.dll','bin\zraw.dll') { $i = [IO.File]::ReadAllBytes($f); $o = [IO.File]::Create('obj\' + [IO.Path]::GetFileName($f) + '.gz'); $g = New-Object IO.Compression.GZipStream($o, [IO.Compression.CompressionLevel]::Optimal); $g.Write($i, 0, $i.Length); $g.Close() }" || exit /b 1

set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
"%CSC%" /nologo /target:winexe /optimize /win32icon:app\icon.ico /out:"bin\ZRAW Studio.exe" /r:obj\ReaLTaiizor.dll ^
  /resource:obj\ReaLTaiizor.dll.gz,ReaLTaiizor.dll.gz /resource:obj\zrawdng.exe.gz,zrawdng.exe.gz ^
  /resource:obj\zrawcore.dll.gz,zrawcore.dll.gz /resource:obj\zraw.dll.gz,zraw.dll.gz /resource:obj\ZrawIcon.dll,ZrawIcon.dll ^
  /resource:app\icon.ico,icon.ico /resource:explorer\icon.ico,zraw-badge.ico app\*.cs || exit /b 1
echo Built bin\ZRAW Studio.exe
