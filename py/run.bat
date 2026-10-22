@echo off
rem 啟動 AnyVideo Download。用 pyw 執行就不會多開一個黑色主控台視窗；
rem 沒有 pyw 時退回 py，這時候主控台會顯示錯誤訊息，方便排查。
cd /d "%~dp0"
where pyw >nul 2>nul
if %errorlevel%==0 (
    start "" pyw -3 "%~dp0anyvideo_download.py"
) else (
    py -3 "%~dp0anyvideo_download.py"
    if errorlevel 1 pause
)
