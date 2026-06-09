@echo off
REM Launch the always-on "Leo" Claude Code session with the voice-buddy channel.
REM Opens a PowerShell window (the invocation that reliably loads the channel).
start "LEO - voice buddy" powershell -NoExit -Command "Set-Location $HOME; Write-Host 'LEO: voice buddy session' -ForegroundColor Cyan; claude --channels plugin:leo-channel@leo-channels"
