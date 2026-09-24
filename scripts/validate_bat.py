"""
验证 .bat 编码 + 实际跑 install/uninstall 测试注册表读写
"""
import os
import sys
import subprocess
import json
import re
import time

# Windows 终端默认 GBK，.bat 输出含中文可能触发编码错误
# 强制 stdout/stderr 编码 utf-8 + errors='replace' 避免崩溃
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)

# ---------- 1. 编码 + 行尾 + 关键命令检查 ----------
def check_bat_file(path, name):
    print(f'\n=== [{name}] {path} ===')
    with open(path, 'rb') as f:
        data = f.read()

    issues = []
    # 无 BOM
    if data[:3] == b'\xef\xbb\xbf':
        issues.append('FAIL: 检测到 UTF-8 BOM')
    else:
        print('  [OK] 无 BOM')

    # CRLF
    lf_count = data.count(b'\n')
    crlf_count = data.count(b'\r\n')
    if lf_count != crlf_count:
        issues.append(f'FAIL: 发现 LF 行尾 ({lf_count} LF, {crlf_count} CRLF)')
    else:
        print(f'  [OK] 全 CRLF ({crlf_count} 行)')

    # chcp 65001
    if b'chcp 65001' in data:
        print('  [OK] 含 chcp 65001')
    else:
        issues.append('FAIL: 缺 chcp 65001 >nul')

    # 没有 taskkill //im 类全量杀
    bad_kill = re.search(rb'taskkill\s+.*?//im\s+\w+\.exe', data, re.IGNORECASE)
    if bad_kill:
        issues.append(f'FAIL: 检测到全量杀: {bad_kill.group(0)!r}')
    else:
        print('  [OK] 无全量杀进程')

    # 路径结尾反斜杠 + 引号（红线）：只匹配已知会带 \ 结尾的变量
    # %~dp0 / %SCRIPT_DIR% / %BASEDIR% 等 — 这些变量值必然以 \ 结尾
    # 模式：%(~dp0|SCRIPT_DIR|BASEDIR|X)%\"   （变量结尾是 \，紧跟 "）
    # 普通 %EXE_PATH% 之类不以 \ 结尾，不算 bug
    bad_var_patterns = [rb'%~dp0\\"', rb'%SCRIPT_DIR%\\"', rb'%BASEDIR%\\"']
    really_bad = None
    for p in bad_var_patterns:
        m = re.search(p, data)
        if m:
            really_bad = m
            break
    if really_bad:
        issues.append(f'FAIL: 路径结尾反斜杠+引号转义 bug: {really_bad.group(0)!r}')
    else:
        print('  [OK] 无路径结尾反斜杠+引号转义问题')

    if issues:
        print('  ---- ISSUES ----')
        for i in issues:
            print(f'  {i}')
        return False
    return True


# ---------- 2. 实际跑 install / uninstall 测试 ----------
def get_reg_value(name):
    """读注册表值"""
    r = subprocess.run(
        ['reg', 'query', r'HKCU\Software\Microsoft\Windows\CurrentVersion\Run', '/v', name],
        capture_output=True, text=True, encoding='gbk', errors='replace'
    )
    if r.returncode != 0:
        return None
    # 输出格式: "    happlayer    REG_SZ    <value>"
    m = re.search(rf'{name}\s+REG_SZ\s+(.+)', r.stdout)
    return m.group(1).strip() if m else None


def set_config_for_test():
    """在测试前写好 set-config 等价的注册表项"""
    exe_path = os.path.join(PROJECT_DIR, 'build', 'Release', 'happlayer.exe')
    cfg_path = os.path.join(PROJECT_DIR, 'layers.txt')
    exe_path = os.path.abspath(exe_path).replace('/', '\\')
    cfg_path = os.path.abspath(cfg_path).replace('/', '\\')
    for k, v in [('ExePath', exe_path), ('ConfigPath', cfg_path)]:
        subprocess.run(
            ['reg', 'add', r'HKCU\Software\happlayer', '/v', k, '/t', 'REG_SZ', '/d', v, '/f'],
            capture_output=True
        )
    print(f'  [setup] ExePath={exe_path}')
    print(f'  [setup] ConfigPath={cfg_path}')


def run_bat(name, *args):
    """通过 cmd.exe 跑 .bat 文件"""
    bat = os.path.join(SCRIPT_DIR, name)
    cmd = ['cmd.exe', '/c', bat] + list(args)
    print(f'  [run] {" ".join(cmd)}')
    # 一些 .bat 会 pause 等用户输入，用 echo y 喂进去
    p = subprocess.run(
        cmd, capture_output=True, text=True, input='\n',
        encoding='gbk', errors='replace', timeout=15
    )
    print(f'  [returncode] {p.returncode}')
    if p.stdout:
        print(f'  [stdout] {p.stdout.strip()[:500]}')
    if p.stderr:
        print(f'  [stderr] {p.stderr.strip()[:500]}')
    return p.returncode


def main():
    print('========== 第一部分：编码 + 命令检查 ==========')
    ok1 = check_bat_file(os.path.join(SCRIPT_DIR, 'install-autostart.bat'), 'install')
    ok2 = check_bat_file(os.path.join(SCRIPT_DIR, 'uninstall-autostart.bat'), 'uninstall')
    ok3 = check_bat_file(os.path.join(SCRIPT_DIR, 'set-config.bat'), 'set-config')

    print('\n========== 第二部分：实际测试注册表读写 ==========')
    # 0. 清理：先确保没有残留
    subprocess.run(
        ['reg', 'delete', r'HKCU\Software\Microsoft\Windows\CurrentVersion\Run', '/v', 'happlayer', '/f'],
        capture_output=True
    )
    print('  [cleanup] 清理残留 Run 键')

    # 1. 写 set-config 用的 ExePath / ConfigPath
    set_config_for_test()

    # 2. 跑 install-autostart.bat
    print('\n--- 跑 install-autostart.bat ---')
    run_bat('install-autostart.bat')

    # 3. 验证注册表有值
    val = get_reg_value('happlayer')
    print(f'  [verify] Run\\happlayer = {val!r}')
    install_ok = val is not None and 'happlayer.exe' in val

    # 4. 跑 uninstall-autostart.bat
    print('\n--- 跑 uninstall-autostart.bat ---')
    run_bat('uninstall-autostart.bat')

    # 5. 验证注册表无值
    val2 = get_reg_value('happlayer')
    print(f'  [verify] Run\\happlayer = {val2!r}')
    uninstall_ok = val2 is None

    # 6. 清理测试配置
    subprocess.run(
        ['reg', 'delete', r'HKCU\Software\happlayer', '/f'],
        capture_output=True
    )
    print('  [cleanup] 清理测试配置项')

    print('\n========== 总结 ==========')
    print(f'  install-autostart.bat 编码: {"PASS" if ok1 else "FAIL"}')
    print(f'  uninstall-autostart.bat 编码: {"PASS" if ok2 else "FAIL"}')
    print(f'  set-config.bat 编码: {"PASS" if ok3 else "FAIL"}')
    print(f'  install 写注册表: {"PASS" if install_ok else "FAIL"}')
    print(f'  uninstall 删注册表: {"PASS" if uninstall_ok else "FAIL"}')

    all_ok = ok1 and ok2 and ok3 and install_ok and uninstall_ok
    return 0 if all_ok else 1


if __name__ == '__main__':
    sys.exit(main())
