"""软复位并抓取串口日志 —— 专治「四线烧录线没有 RTS，烧完不启动」。

背景：
    idf.py flash 结束时那行 "Hard resetting via RTS pin..." 靠拉低 RTS 复位芯片。
    我们的烧录线只有 VCC/GND/TX/RX 四根，RTS 悬空，这句等于喊空口号。
    芯片可能一直停在下载模式，程序根本没执行。
    而且 idf.py monitor 在非交互终端里会立刻 EOF 退出（只打印一个 Done），
    看不到任何日志，很容易误判成板子坏了。

本脚本做两件事：
    1. 用 esptool 的 stub 在芯片内部做软件复位（不依赖 RTS 线）
    2. 立刻打开串口把启动日志抓下来，按 GBK 输出避免中文乱码

用法（在工程根目录）：
    D:\\work\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe tools\\boot_run.py
    ... tools\\boot_run.py COM8 115200 60      # 指定串口/波特率/抓取 60 秒
    ... tools\\boot_run.py COM8 115200 0       # 一直抓到 Ctrl+C

日志同时写入 tools/boot_YYYYmmdd_HHMMSS.log。
"""

import datetime
import os
import subprocess
import sys
import time

import serial

IDF_PY = r"D:\work\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
ESPTOOL = r"D:\work\Espressif\frameworks\esp-idf-v5.5.5\components\esptool_py\esptool\esptool.py"


def soft_reset(port):
    """让芯片从下载模式跳出去执行 flash 里的程序。"""
    python = IDF_PY if os.path.exists(IDF_PY) else sys.executable
    if not os.path.exists(ESPTOOL):
        print("[warn] esptool.py not found: %s" % ESPTOOL)
        print("[warn] skip soft reset, just listen on serial")
        return False

    cmd = [python, ESPTOOL, "--chip", "esp32c3", "-p", port,
           "--before", "no_reset", "--after", "soft_reset", "run"]
    print("[1/2] soft reset via esptool stub ...")
    try:
        ret = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except Exception as exc:
        print("[warn] soft reset failed: %s" % exc)
        return False

    tail = (ret.stdout or "") + (ret.stderr or "")
    for line in tail.strip().splitlines():
        print("      " + line)
    if ret.returncode != 0:
        print("[warn] esptool exit code %d (port busy? monitor still open?)"
              % ret.returncode)
        return False
    print("      -> chip should be running now")
    return True


def listen(port, baud, seconds):
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "boot_%s.log" % stamp)
    out = open(log_path, "w", encoding="utf-8", errors="replace")

    print("[2/2] listening on %s @ %d%s"
          % (port, baud, "" if seconds <= 0 else ", %ds" % seconds))
    print("      log -> %s" % log_path)
    print("-" * 64)

    total = 0
    deadline = 0 if seconds <= 0 else time.time() + seconds

    while True:
        if deadline and time.time() > deadline:
            print("\n[time up]")
            break
        ser = None
        try:
            ser = serial.Serial(port, baud, timeout=0.2)
            while True:
                if deadline and time.time() > deadline:
                    break
                data = ser.read(4096)
                if not data:
                    continue
                txt = data.decode("utf-8", errors="replace")
                out.write(txt)
                out.flush()
                total += len(data)
                try:
                    sys.stdout.buffer.write(txt.encode("gbk", "replace"))
                except Exception:
                    sys.stdout.write(txt)
                sys.stdout.flush()
        except KeyboardInterrupt:
            raise
        except Exception as exc:
            print("\n[port hiccup: %s]" % exc)
            time.sleep(0.5)
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass
        if deadline and time.time() > deadline:
            break

    out.close()
    print("-" * 64)
    print("captured %d bytes -> %s" % (total, log_path))
    if total == 0:
        print()
        print("一个字节都没收到，按顺序排查：")
        print("  1. 板子 TX/RX 是否接反（模块 TX -> 板子 RX=GPIO39，模块 RX -> 板子 TX=GPIO23）")
        print("  2. BOOT 键(IO9)是否被按住/卡住，日志里会出现 waiting for download")
        print("  3. 串口是否被别的窗口占用（monitor / 串口助手 先关掉）")
        print("  4. 供电是否足够（MQTT 版本启动瞬间电流较大，USB 口不够会反复重启）")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    seconds = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    soft_reset(port)
    time.sleep(0.05)
    listen(port, baud, seconds)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[stopped by user]")
