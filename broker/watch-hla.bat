@echo off
rem Affiche tout le trafic hla/# (présence des fédérés, etc.)
if "%MOSQUITTO_DIR%"=="" set "MOSQUITTO_DIR=C:\Program Files\mosquitto"
"%MOSQUITTO_DIR%\mosquitto_sub.exe" -h localhost -t "hla/#" -v
