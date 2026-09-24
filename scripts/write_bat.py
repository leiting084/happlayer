"""
写入 .bat 文件的辅助脚本。
- 强制 UTF-8 无 BOM
- 强制 CRLF 行尾
- 包含 chcp 65001 >nul（如有中文 echo）
- 防路径结尾反斜杠 + 引号转义：用 pushd/popd 或去尾 \
- 验证：写完 readback 确认编码 + 行尾
"""
import sys
import os

# install-autostart.bat 内容
INSTALL_BAT = r'''@echo off
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
'''

# uninstall-autostart.bat 内容
UNINSTALL_BAT = r'''@echo off
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
'''

# set-config.bat 内容（交互式配置）
SET_CONFIG_BAT = r'''@echo off
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
echo [5] 退出
echo.
set /p "CHOICE=请选择 (1-5): "

if "%CHOICE%"=="1" goto set_exe
if "%CHOICE%"=="2" goto set_cfg
if "%CHOICE%"=="3" goto do_install
if "%CHOICE%"=="4" goto do_uninstall
if "%CHOICE%"=="5" goto end

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

:end
echo.
pause
exit /b 0
'''


def write_bat(path, content):
    """写 .bat：UTF-8 无 BOM + CRLF"""
    # 先转 CRLF（如果输入是 LF）
    content = content.replace('\r\n', '\n').replace('\n', '\r\n')
    with open(path, 'wb') as f:
        f.write(content.encode('utf-8'))

    # 验证
    data = open(path, 'rb').read()
    assert data[:3] != b'\xef\xbb\xbf', f'BOM detected: {path}'
    assert data.count(b'\n') == data.count(b'\r\n'), f'LF 行尾: {path}'
    assert b'chcp 65001' in data, f'缺 chcp 65001: {path}'
    print(f'OK: {path} (UTF-8 无 BOM, CRLF, chcp 65001)')


if __name__ == '__main__':
    base = os.path.dirname(os.path.abspath(__file__))
    write_bat(os.path.join(base, 'install-autostart.bat'), INSTALL_BAT)
    write_bat(os.path.join(base, 'uninstall-autostart.bat'), UNINSTALL_BAT)
    write_bat(os.path.join(base, 'set-config.bat'), SET_CONFIG_BAT)
    print('Done.')
