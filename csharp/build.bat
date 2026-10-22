@echo off
rem Build the C# (WPF, .NET 8) version.
rem Output: bin\Release\net8.0-windows\AnyVideoDownload.exe
setlocal
cd /d "%~dp0"
dotnet build AnyVideoDownload.csproj -c Release -nologo || exit /b 1
echo.
echo Built: %~dp0bin\Release\net8.0-windows\AnyVideoDownload.exe
