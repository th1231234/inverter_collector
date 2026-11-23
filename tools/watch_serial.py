"""串口日志实时查看工具（Windows 控制台中文不乱码）。

用法：
    python watch_serial.py                 # 默认 COM8, 115200
    python watch_serial.py COM8 115200

按 Ctrl+C 退出。
日志同时写入 serial_YYYYmmdd_HHMMSS.log，方便回看。
"""

import datetime
import os
import sys
import time

import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "serial_%s.log" % stamp)

    print("listening on %s @ %d, log -> %s" % (port, baud, log_path))
    print("press Ctrl+C to stop")
    print("-" * 60)

    out = open(log_path, "w", encoding="utf-8", errors="replace")
    total = 0

    while True:
        ser = None
        try:
            ser = serial.Serial(port, baud, timeout=0.2)
            while True:
                data = ser.read(4096)
                if not data:
                    continue
                txt = data.decode("utf-8", errors="replace")
                out.write(txt)
                out.flush()
                total += len(data)
                # Windows 控制台默认 GBK，直接写 utf-8 会乱码
                try:
                    sys.stdout.buffer.write(txt.encode("gbk", "replace"))
                except Exception:
                    sys.stdout.write(txt)
                sys.stdout.flush()
        except KeyboardInterrupt:
            break
        except Exception as exc:
            print("\n[port hiccup: %s]" % exc)
            time.sleep(0.5)
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass

    out.close()
    print("\n" + "-" * 60)
    print("stopped, %d bytes captured -> %s" % (total, log_path))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
