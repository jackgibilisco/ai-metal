@echo off
setlocal enabledelayedexpansion

rem Windows build driver, the counterpart to the macOS Makefile. One cl.exe
rem invocation per target, no project file.
rem
rem   build.bat         build build\Renderer_gl.exe
rem   build.bat run     build and launch
rem   build.bat test    unit tests, then parity against tests\golden
rem   build.bat clean   remove build\

cd /d "%~dp0"

if "%1"=="clean" (
    if exist build rmdir /s /q build
    echo cleaned
    exit /b 0
)

if not defined VCToolsInstallDir call :find_vcvars || exit /b 1

set BUILD=build
set OBJ=%BUILD%\obj
if not exist %OBJ% mkdir %OBJ%

set CXXFLAGS=/nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /D_USE_MATH_DEFINES /Isrc /Isrc\third_party /Fo%OBJ%\
set LIBS=opengl32.lib gdi32.lib user32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib dwmapi.lib advapi32.lib

set CORE=src\arena.cpp src\undo_stack.cpp src\scene.cpp src\audio.cpp src\timeline.cpp src\gizmo.cpp src\game.cpp src\scene_import.cpp src\blend_file.cpp src\menu.cpp src\ui.cpp src\app.cpp src\renderer_common.cpp src\font_atlas.cpp
set GL_BACKEND=src\renderer_gl.cpp src\ui_render_gl.cpp src\gl_shader.cpp src\windows\gl_loader.cpp
set PLATFORM=src\windows\platform_windows.cpp src\windows\platform_windows_gl.cpp

rem zstd's amalgamated decompressor is C99, so it gets its own object.
set ZSTD_OBJ=%OBJ%\zstddeclib.obj
if not exist %ZSTD_OBJ% (
    cl /nologo /TC /O2 /MT /W0 /D_CRT_SECURE_NO_WARNINGS /c src\third_party\zstd\zstddeclib.c /Fo%ZSTD_OBJ% || exit /b 1
)

if "%1"=="test" goto :test

cl %CXXFLAGS% %CORE% %GL_BACKEND% %PLATFORM% %ZSTD_OBJ% %LIBS% /link /SUBSYSTEM:WINDOWS /OUT:%BUILD%\Renderer_gl.exe || exit /b 1
echo built %BUILD%\Renderer_gl.exe

if "%1"=="run" %BUILD%\Renderer_gl.exe
exit /b 0

:test
if not exist %OBJ%\test mkdir %OBJ%\test
set TESTFLAGS=/nologo /std:c++17 /EHsc /W4 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /D_USE_MATH_DEFINES /Isrc /Isrc\third_party /Fo%OBJ%\test\

cl %TESTFLAGS% tests\math3d_test.cpp /link /OUT:%BUILD%\math3d_test.exe || exit /b 1
cl %TESTFLAGS% tests\font_atlas_test.cpp src\font_atlas.cpp /link /OUT:%BUILD%\font_atlas_test.exe || exit /b 1
cl %TESTFLAGS% tests\gizmo_test.cpp src\gizmo.cpp /link /OUT:%BUILD%\gizmo_test.exe || exit /b 1
cl %TESTFLAGS% tests\scene_undo_test.cpp src\scene.cpp src\arena.cpp src\undo_stack.cpp /link /OUT:%BUILD%\scene_undo_test.exe || exit /b 1
cl %TESTFLAGS% tests\timeline_undo_test.cpp src\scene.cpp src\timeline.cpp src\arena.cpp src\undo_stack.cpp /link /OUT:%BUILD%\timeline_undo_test.exe || exit /b 1

%BUILD%\math3d_test.exe || exit /b 1
%BUILD%\font_atlas_test.exe || exit /b 1
%BUILD%\gizmo_test.exe || exit /b 1
%BUILD%\scene_undo_test.exe || exit /b 1
%BUILD%\timeline_undo_test.exe || exit /b 1

cl %CXXFLAGS% tests\render_parity.cpp tests\offscreen_wgl.cpp %CORE% %GL_BACKEND% src\windows\platform_windows_gl.cpp %ZSTD_OBJ% %LIBS% /link /OUT:%BUILD%\parity_gl.exe || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /Fo%OBJ%\diff_ tests\image_diff.cpp /link /OUT:%BUILD%\image_diff.exe || exit /b 1

if exist %BUILD%\parity\gl rmdir /s /q %BUILD%\parity\gl
mkdir %BUILD%\parity\gl
rem The goldens are Metal renders with the AO pass disabled: SSAO is not
rem bit-reproducible across GPU vendors, everything else is. `make parity-test`
rem on a Mac still compares the full images, AO included.
%BUILD%\parity_gl.exe %BUILD%\parity\gl --ao-off || exit /b 1
%BUILD%\image_diff.exe tests\golden %BUILD%\parity\gl --no-gpu-timings --cross-gpu || exit /b 1
exit /b 0

:find_vcvars
rem vswhere.exe lives under a path with parentheses, which cmd cannot quote
rem inside a for/f command, so its answer comes back through a file instead.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo vswhere.exe not found; install the Visual Studio C++ build tools
    exit /b 1
)
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\ai-metal-vspath.txt"
set /p VSPATH=<"%TEMP%\ai-metal-vspath.txt"
if not defined VSPATH (
    echo no Visual Studio install with the C++ tools
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
