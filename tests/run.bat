@echo off
rem Rebuilds the plugin, then runs the self-check (tests\test_blend.cpp compiles every plugin source into one exe).
pushd "%~dp0.."
call "%cd%\build.bat" || exit /b 1
cl /nologo /O2 /EHsc /D_CRT_SECURE_NO_WARNINGS tests\test_blend.cpp %OBJ% /Fobuild\ /Febuild\test_blend.exe && build\test_blend.exe
popd
