@echo off
chcp 65001 >nul
REM 开机自启三件套 — 卸载开机自启
REM 删除注册表 HKCU\Software\Microsoft\Windows\CurrentVersion\Run\happlayer

reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v happlayer /f >nul 2>&1
if errorlevel 1 (
    echo [信息] 注册表项不存在，无需卸载
) else (
    echo [OK] 开机自启已卸载
)
pause
exit /b 0
