@echo off
rem dev.cmd — run any command inside the VS 2022 x64 dev environment.
rem Ninja + cl need MSVC/SDK paths in the environment; this is the one place
rem that knowledge lives. Examples:
rem   dev cmake --preset dev
rem   dev cmake --build --preset dev
rem   dev ctest --preset dev -L lex
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
%*
