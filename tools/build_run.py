"""跑 idf.py build，把编译输出直接重定向到文件。

为什么要这个脚本：

在 PowerShell 里用 `*>` 或管道接 idf.py 的输出，PowerShell 会把每一行
先转成 .NET 对象、再按 UTF-16 写文件。999 个编译任务时这个转换会严重拖慢，
日志看起来「卡在 777/999 不动」，其实是父进程在慢慢搬数据（第一次踩坑时
还误判成 ccache 死锁）。

subprocess 把子进程的 stdout 文件句柄直接交给文件，中间不做任何字符转换，
日志能实时增长，也能正确反映真实进度。

用法：
    python tools/build_run.py              # 输出到 build_log.txt
    python tools/build_run.py other.log    # 自定义日志名
"""

import os
import subprocess
import sys

log_name = sys.argv[1] if len(sys.argv) > 1 else "build_log.txt"
log_path = os.path.join(os.getcwd(), log_name)

# idf.py 是 python 脚本而不是可执行文件，Windows 直接 spawn 会报 WinError 193。
# 用当前解释器（ESP-IDF 自带的 python）去执行它，依赖才齐全。
idf_path = os.environ.get("IDF_PATH", "")
idf_py = os.path.join(idf_path, "tools", "idf.py")
if not os.path.isfile(idf_py):
    print("IDF_PATH not set or idf.py not found:", idf_py)
    sys.exit(2)

cmd = [sys.executable, idf_py, "--no-ccache", "build"]

with open(log_path, "w", encoding="utf-8", errors="replace", buffering=1) as log:
    proc = subprocess.run(
        cmd,
        stdout=log,
        stderr=subprocess.STDOUT,
        cwd=os.getcwd(),
    )

print("EXIT", proc.returncode)
sys.exit(proc.returncode)
