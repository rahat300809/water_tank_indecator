@echo off
title Dual MQTT Proxy (1883 + 9001) - Smart Water Tank
echo Starting Dual MQTT Proxy (Ports 1883 and 9001 -^> 192.168.110.133)...
python -u "%~dp0mqtt_proxy.py"
pause
