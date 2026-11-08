# build.ps1 - builds tools\crashcatch\crashcatch.exe (crashcatch.cpp: a program run as its debugger, its fatal end's
# stacks and a minidump written - TODO 163) with Visual Studio 2022's compiler. Output beside this script.
$here = $PSScriptRoot
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmd = "`"$vcvars`" >nul && cl /nologo /O2 /EHsc /std:c++17 /Fe:`"$here\crashcatch.exe`" /Fo:`"$here\\`" `"$here\crashcatch.cpp`" dbghelp.lib"
cmd /c $cmd
"crashcatch build exit $LASTEXITCODE"
