@echo off
rem Compiles one generated function the way the game build does (its flags and precompiled
rem header), in a few seconds instead of a batch of hundreds: for a compiler error or crash
rem (C1001) in one function, or to try a change to the generated code. The object goes to
rem out\compile_one\.
rem
rem   tools\compile_one.cmd work\generated\sub_003711A0_0x3711a0.cpp
rem
rem Needs a finished build (out\build) for the precompiled header.
setlocal
set ROOT=%~dp0..
for %%I in ("%ROOT%") do set ROOT=%%~fI
set OBJ=%ROOT%\out\build\transformers\TransformersRecomp.dir\RelWithDebInfo
set PCHDIR=%ROOT%\out\build\transformers\CMakeFiles\TransformersRecomp.dir\RelWithDebInfo
if not exist "%OBJ%\cmake_pch.pch" (
    echo No precompiled header in %OBJ%: build the game first.
    exit /b 1
)
for %%I in ("%~1") do set SOURCE=%%~fI
if not exist "%SOURCE%" (
    echo usage: tools\compile_one.cmd ^<generated .cpp^>
    exit /b 1
)
if not exist "%ROOT%\out\compile_one" mkdir "%ROOT%\out\compile_one"
rem The generated file is included from a wrapper, as the batches include it.
> "%ROOT%\out\compile_one\one.cpp" echo #include "%SOURCE:\=/%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cl.exe /c /I"%ROOT%\work\generated" /I"%ROOT%\transformers\src" /I"%ROOT%\transformers\decomp" ^
    /I"%ROOT%\PS2Recomp\ps2xRuntime\include" /I"%ROOT%\PS2Recomp\ps2xRuntime\src\lib\Kernel" ^
    /I"%ROOT%\out\build\_deps\raylib-src\src" /I"%ROOT%\out\build\_deps\raylib-src\src\external\glfw\include" ^
    /I"%ROOT%\PS2Recomp\ps2xIOP\include" /I"%ROOT%\out\build\_deps\rlimgui-src" /I"%ROOT%\out\build\_deps\imgui-src" ^
    /Z7 /nologo /W1 /WX- /diagnostics:column /O2 /Ob1 /D _MBCS /D WIN32 /D _WINDOWS /D NDEBUG ^
    /D PS2X_ENABLE_DEBUG_UI=1 /D PS2_RUNTIME_LOGS=1 /D PS2X_ENABLE_IOP_RPC_TRACE=1 /D PLATFORM_DESKTOP ^
    /D GRAPHICS_API_OPENGL_33 /D "CMAKE_INTDIR=\"RelWithDebInfo\"" /EHsc /MD /GS- /std:c++20 ^
    /Yu"%PCHDIR%\cmake_pch.hxx" /Fp"%OBJ%\cmake_pch.pch" /Fo"%ROOT%\out\compile_one\\" /external:W0 /TP ^
    /FI"%PCHDIR%\cmake_pch.hxx" /external:I "%ROOT%\out\build\ThirdParty\ffmpeg-prefix\src\ffmpeg_external\include" ^
    /bigobj "%ROOT%\out\compile_one\one.cpp"
exit /b %ERRORLEVEL%
