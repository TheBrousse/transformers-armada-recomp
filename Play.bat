@echo off
rem Plays the game. In the game: F1 = the menu (settings, saves), F11 = full screen.
rem First start: settings.ini is made from settings.example.ini (overlay\settings.example.ini when there is one).
cd /d "%~dp0"
if not exist settings.ini if exist overlay\settings.example.ini copy /y overlay\settings.example.ini settings.ini >nul
if not exist settings.ini if exist settings.example.ini copy /y settings.example.ini settings.ini >nul
start "" "out\build\transformers\RelWithDebInfo\TransformersRecomp.exe"
