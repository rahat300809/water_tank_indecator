@echo off
title MQTT TCP Proxy - Smart Water Tank
echo Starting MQTT TCP Proxy (0.0.0.0:1883 -^> 192.168.110.133:1883)...
python "%~dp0mqtt_proxy.py"
pause
