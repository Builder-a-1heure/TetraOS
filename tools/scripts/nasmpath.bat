@echo off
:: Chemin du dossier projet
set "project_path=C:\Users\roro4\OneDrive\Bureau\Project - TetraOS"

:: Vérifie si on est admin (via la variable %errorlevel% de NET FILE)
net file >nul 2>&1
if %errorlevel% NEQ 0 (
    echo [*] Elevation requise. Relance en administrateur...
    powershell -Command "Start-Process '%~f0' -Verb runAs"
    exit /b
)

:: On est admin ici

cd /d "%project_path%"
echo [*] Invite admin ouverte dans : %cd%
cmd
