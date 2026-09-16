@echo off
rem Build the shadps4 emulator (x64-Clang-Debug).
rem Usage: build-debug.bat [--reconfigure]
rem   --reconfigure  Re-run the CMake configure step before building.
setlocal

set "PRESET=x64-Clang-Debug"
set "LLVM_BIN=E:/research/tools/LLVM-19.1.1/bin"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VCVARS%" >nul || goto fail
rem vcvars64.bat may change the working directory, so return to the repository root afterwards.
cd /d "%~dp0" || goto fail

if /i "%~1"=="--reconfigure" goto configure
if not exist "Build\%PRESET%\CMakeCache.txt" goto configure
goto build

:configure
rem ENABLE_TESTS must stay OFF: this branch excludes the emulator target when tests are enabled.
cmake --preset %PRESET% -DENABLE_TESTS=OFF ^
    -DCMAKE_C_COMPILER=%LLVM_BIN%/clang-cl.exe ^
    -DCMAKE_CXX_COMPILER=%LLVM_BIN%/clang-cl.exe ^
    -DCMAKE_RC_COMPILER=%LLVM_BIN%/llvm-rc.exe ^
    -DCMAKE_MT=%LLVM_BIN%/llvm-mt.exe || goto fail

:build
cmake --build "Build\%PRESET%" --target shadps4 --parallel 8 || goto fail
echo.
echo Build succeeded: %CD%\Build\%PRESET%\shadps4.exe
set "EXIT_CODE=0"
goto end

:fail
set "EXIT_CODE=%errorlevel%"
echo.
echo Build failed with exit code %EXIT_CODE%.

:end
rem Keep the window open when the script was started by double-clicking it.
echo %cmdcmdline% | findstr /i /c:"%~nx0" >nul && pause
exit /b %EXIT_CODE%
