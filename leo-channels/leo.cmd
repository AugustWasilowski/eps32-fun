@echo off
REM leo - launch Claude Code with the ESP32 voice-buddy channel from any directory.
"%USERPROFILE%\.local\bin\claude.exe" --channels plugin:leo-channel@leo-channels %*
