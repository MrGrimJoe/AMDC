@echo off
rem Build with MSVC (run from "x64 Native Tools Command Prompt for VS"). Not tested with MSVC by the author:
rem the MinGW build in dist\windows IS tested (under Wine). Run test_fast.exe first.
if not exist build mkdir build
cl /nologo /O2 /std:c++17 /EHsc /W3 src\fast.cpp src\amdcf_main.cpp /Fe:build\amdcf.exe /Fo:build\ || exit /b 1
cl /nologo /O2 /std:c++17 /EHsc /W3 src\fast.cpp tests\test_fast.cpp /Fe:build\test_fast.exe /Fo:build\ || exit /b 1
cl /nologo /O2 /std:c++17 /EHsc /W3 src\cm.cpp src\transforms.cpp src\container.cpp src\main.cpp /Fe:build\amdc1.exe /Fo:build\ || exit /b 1
build\test_fast.exe
