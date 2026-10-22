@echo off
rem Builds TrueDrawFPS.asi with MSVC (x64).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
for %%s in (inst wheel char prop) do (
    ml64 /nologo /c /Fobuild\%%s_stub.obj src\blend\%%s_stub.asm || exit /b 1
)
set SRC=src\dllmain.cpp src\config.cpp src\log.cpp src\patch.cpp src\pacer.cpp src\fpsoption.cpp src\gates.cpp ^
 src\framelatency.cpp src\crashguard.cpp src\replay.cpp src\probe.cpp src\ui.cpp ^
 src\blend\frame.cpp src\blend\objects.cpp src\blend\character.cpp src\blend\props.cpp src\blend\wheels.cpp ^
 src\blend\view.cpp src\blend\effects.cpp src\blend\readouts.cpp
set OBJ=build\inst_stub.obj build\wheel_stub.obj build\char_stub.obj build\prop_stub.obj
cl /nologo /LD /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS %SRC% %OBJ% /Fobuild\ /Febuild\TrueDrawFPS.asi /link /NOLOGO /MAP:build\TrueDrawFPS.map || exit /b 1
