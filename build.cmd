@echo off
rem Builds "out\3D FlowerBox RT.scr" with Visual Studio (C++ x64 tools), the
rem Windows SDK and the NVIDIA DLSS SDK files in third_party\dlss.
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio with the C++ x64 tools was not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "DLSS=third_party\dlss"
if not exist "%DLSS%\lib\nvngx_dlss.dll" (
    powershell -NoProfile -ExecutionPolicy Bypass -File fetch_dlss.ps1 || (
        echo Could not download the DLSS SDK files; see README.md.
        exit /b 1
    )
)

if not exist build mkdir build
if not exist out mkdir out

rem DXC may point at a specific dxc.exe; otherwise the SDK's dxc on PATH is used.
if not defined DXC (
    where dxc >nul 2>nul || (
        echo dxc.exe was not found; install a Windows 10/11 SDK.
        exit /b 1
    )
    set "DXC=dxc"
)

set "HLSL=shaders\flowerbox.hlsl"
"%DXC%" -nologo -T vs_6_0 -E VSMain -O3 -Fh build\shader_vs.h -Vn g_VS %HLSL% || exit /b 1
"%DXC%" -nologo -T ps_6_0 -E PSMain -O3 -Fh build\shader_ps.h -Vn g_PS %HLSL% || exit /b 1
"%DXC%" -nologo -T ps_6_0 -E PSMain -O3 -D MOTION_VECTORS=1 -Fh build\shader_psmv.h -Vn g_PSMV %HLSL% || exit /b 1
"%DXC%" -nologo -T ps_6_5 -E PSMain -O3 -D RAYTRACING=1 -Fh build\shader_psrt.h -Vn g_PSRT %HLSL% || exit /b 1
"%DXC%" -nologo -T ps_6_5 -E PSMain -O3 -D RAYTRACING=1 -D MOTION_VECTORS=1 -Fh build\shader_psrtmv.h -Vn g_PSRTMV %HLSL% || exit /b 1

rc /nologo /fo build\flowerbox.res res\flowerbox.rc || exit /b 1

cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Ibuild /I"%DLSS%\include" /Fobuild\ ^
   src\main.cpp src\settings.cpp src\flowerbox.cpp src\renderer.cpp src\config.cpp src\dlss.cpp build\flowerbox.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:"out\3D FlowerBox RT.scr" ^
   "%DLSS%\lib\nvsdk_ngx_s.lib" d3d12.lib dxgi.lib user32.lib gdi32.lib advapi32.lib comctl32.lib ole32.lib ^
   windowscodecs.lib shell32.lib version.lib || exit /b 1

echo Built out\3D FlowerBox RT.scr
