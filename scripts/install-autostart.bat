@echo off
chcp 65001 >nul
REM 开机自启三件套 — 安装开机自启
REM 写注册表 HKCU\Software\Microsoft\Windows\CurrentVersion\Run\happlayer
REM
REM 用法：双击运行，或命令行 install-autostart.bat
REM 前置：先运行 set-config.bat 配置 exe 和 config 路径

REM ---------- 路径常量 ----------
set "SCRIPT_DIR=%~dp0"
REM 路径结尾反斜杠 → 去尾（避免 pushd 嵌套和后续引号转义问题）
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ---------- 读配置（注册表） ----------
set "EXE_PATH="
set "CONFIG_PATH="
for /f "tokens=2*" %%a in ('reg query "HKCU\Software\happlayer" /v ExePath 2^>nul') do set "EXE_PATH=%%b"
for /f "tokens=2*" %%a in ('reg query "HKCU\Software\happlayer" /v ConfigPath 2^>nul') do set "CONFIG_PATH=%%b"

REM ---------- 配置缺失则提示 ----------
if "%EXE_PATH%"=="" (
    echo [错误] 未配置 ExePath，请先运行 set-config.bat
    pause
    exit /b 1
)
if not exist "%EXE_PATH%" (
    echo [错误] ExePath 不存在：%EXE_PATH%
    pause
    exit /b 1
)

REM ---------- 拼装自启命令 ----------
REM 不用 set 嵌入引号（\" 会被 cmd 当转义），直接在 reg add 时拼
REM 注册表值格式："<exe绝对路径>" "<config绝对路径>"（引号包路径，空格分隔）
if "%CONFIG_PATH%"=="" (
    reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v happlayer /t REG_SZ /d "\"%EXE_PATH%\"" /f >nul
) else (
    reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v happlayer /t REG_SZ /d "\"%EXE_PATH%\" \"%CONFIG_PATH%\"" /f >nul
)
if errorlevel 1 (
    echo [错误] 写注册表失败
    pause
    exit /b 1
)

echo.
echo [OK] 开机自启已安装
echo   ExePath:    %EXE_PATH%
echo   ConfigPath: %CONFIG_PATH%
echo   注册表项:   HKCU\Software\Microsoft\Windows\CurrentVersion\Run\happlayer
echo.
echo 卸载请运行 uninstall-autostart.bat
pause
exit /b 0
