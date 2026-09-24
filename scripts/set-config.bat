@echo off
chcp 65001 >nul
REM 开机自启三件套 — 交互式设置面板
REM 1. 配置 ExePath / ConfigPath（写注册表 HKCU\Software\happlayer）
REM 2. 可选安装/卸载开机自启

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ---------- 默认值：当前目录的 happlayer.exe 和 layers.txt ----------
set "DEFAULT_EXE=%SCRIPT_DIR%\..\build\Release\happlayer.exe"
set "DEFAULT_CONFIG=%SCRIPT_DIR%\..\layers.txt"

REM ---------- 读已存配置（显示在提示里） ----------
set "CUR_EXE="
set "CUR_CONFIG="
for /f "tokens=2*" %%a in ('reg query "HKCU\Software\happlayer" /v ExePath 2^>nul') do set "CUR_EXE=%%b"
for /f "tokens=2*" %%a in ('reg query "HKCU\Software\happlayer" /v ConfigPath 2^>nul') do set "CUR_CONFIG=%%b"

if "%CUR_EXE%"=="" set "CUR_EXE=%DEFAULT_EXE%"
if "%CUR_CONFIG%"=="" set "CUR_CONFIG=%DEFAULT_CONFIG%"

echo.
echo ================================
echo   happlayer 设置面板
echo ================================
echo.
echo 当前配置：
echo   ExePath:    %CUR_EXE%
echo   ConfigPath: %CUR_CONFIG%
echo.

REM ---------- 1) 配置 ExePath ----------
echo [1] 修改 happlayer.exe 路径
echo [2] 修改 layers.txt 路径
echo [3] 安装开机自启
echo [4] 卸载开机自启
echo [5] 打开 layers.txt 编辑音频源（[audio] source=…）
echo [6] 退出
echo.
set /p "CHOICE=请选择 (1-6): "

if "%CHOICE%"=="1" goto set_exe
if "%CHOICE%"=="2" goto set_cfg
if "%CHOICE%"=="3" goto do_install
if "%CHOICE%"=="4" goto do_uninstall
if "%CHOICE%"=="5" goto edit_audio
if "%CHOICE%"=="6" goto end

echo [错误] 无效选择
goto end

:set_exe
set /p "NEW_EXE=新 ExePath（当前: %CUR_EXE%）："
if "%NEW_EXE%"=="" set "NEW_EXE=%CUR_EXE%"
reg add "HKCU\Software\happlayer" /v ExePath /t REG_SZ /d "%NEW_EXE%" /f >nul
if errorlevel 1 (
    echo [错误] 写注册表失败
    pause
    exit /b 1
)
echo [OK] ExePath 已更新为：%NEW_EXE%
goto end

:set_cfg
set /p "NEW_CFG=新 ConfigPath（当前: %CUR_CONFIG%）："
if "%NEW_CFG%"=="" set "NEW_CFG=%CUR_CONFIG%"
reg add "HKCU\Software\happlayer" /v ConfigPath /t REG_SZ /d "%NEW_CFG%" /f >nul
if errorlevel 1 (
    echo [错误] 写注册表失败
    pause
    exit /b 1
)
echo [OK] ConfigPath 已更新为：%NEW_CFG%
goto end

:do_install
call "%SCRIPT_DIR%\install-autostart.bat"
goto end

:do_uninstall
call "%SCRIPT_DIR%\uninstall-autostart.bat"
goto end

REM ---------- 5) 编辑音频源：定位到 layers.txt 的 [audio] 段 ----------
:edit_audio
echo.
echo 当前 ConfigPath: %CUR_CONFIG%
if not exist "%CUR_CONFIG%" (
    echo [错误] 找不到 layers.txt：%CUR_CONFIG%
    pause
    exit /b 1
)
echo 在 layers.txt 的 [audio] 段修改 source= 后重启 happlayer 生效。
echo 示例：
echo   [audio]
echo   source=clips/background.wav
echo   volume=0.8
echo   loop=1
echo.
echo 正在打开 layers.txt ...
start "" notepad.exe "%CUR_CONFIG%"
echo.
echo [提示] 编辑保存后，重启 happlayer 即可生效（无需重装）。
goto end

:end
echo.
pause
exit /b 0
