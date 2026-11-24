@echo off
rem ====================================================================
rem Decode a protobuf hex payload offline (no broker, no MQTTX needed).
rem
rem Usage: paste the payload copied from MQTTX after the command.
rem   Spaces are fine, quotes are not required:
rem     decode 0a08 696e 7465 726e 3031 10 e2 07 ...
rem     decode 0a013110011880602005
rem
rem To use it from any directory, add this folder to PATH:
rem   setx PATH "%PATH%;D:\work\espidf_projects\inverter_collector\tools"
rem (setx only takes effect in a newly opened cmd window)
rem
rem NOTE: keep this file pure ASCII. cmd.exe reads .bat as ANSI/GBK,
rem so UTF-8 Chinese comments get garbled and break the script.
rem ====================================================================
setlocal
set "PY=D:\work\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
set "SCRIPT=%~dp0proto_decode.py"
"%PY%" "%SCRIPT%" hex %*
endlocal
