@echo off
rem Called by Inno Setup (SignTool=signtool, see build.ps1) with the file to sign
rem as %1. The signtool arguments (certificate, timestamp...) come from the
rem environment so that no secret appears on a command line shown in a log.
signtool sign %AL_SIGN_ARGS% %1
exit /b %errorlevel%
