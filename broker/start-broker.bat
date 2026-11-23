@echo off
rem Lance Mosquitto en avant-plan avec la configuration du projet.
rem Variable MOSQUITTO_DIR pour un chemin d'installation différent.
if "%MOSQUITTO_DIR%"=="" set "MOSQUITTO_DIR=C:\Program Files\mosquitto"
"%MOSQUITTO_DIR%\mosquitto.exe" -c "%~dp0mosquitto-hla.conf" -v
