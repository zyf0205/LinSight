@echo off
rem Launch the LinSight bench GUI with the Python that has python-can.
cd /d "%~dp0..\.."
py -3.10 host\gui\linsight_gui.py
if errorlevel 1 pause
