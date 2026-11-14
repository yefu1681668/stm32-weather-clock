"""
OTA 上位机：把 APP 固件通过串口发给 BootLoader（零第三方依赖）

用法（在仓库根目录执行）：
    python tools/ota_send.py --port COM14
    python tools/ota_send.py --port COM14 --hex mdk/Objects/Freertos的嵌入式智能天气时钟.hex

接线：CH340 TXD -> PA10、RXD -> PA9、GND 共地，115200 8N1。
运行前请关闭串口助手（COM 口只能被一个程序占用），脚本自己会实时打印 Boot 日志。

流程：
    1. 读 Keil 生成的 Intel HEX，还原出从 0x08004000 开始的连续固件 bin
    2. 前面加 32 B 镜像头（magic / length / crc32 / version）
    3. 串口发一个 'U' 让板子回到 Boot（APP 运行时会写参数页再复位）
    4. 等 Boot 打印 "waiting for firmware"，再按 HEAD / DATA x N / END 逐帧发送
    5. 每帧等 ACK，失败重发；全部发完 Boot 自己擦写 APP 并复位

帧格式（与 boot/boot_cfg.h、boot_ota.c 完全一致）：
    SOF(0xA5) TYPE(1) SEQ(2,小端) LEN(2,小端) PAYLOAD CRC16(2,小端)
    CRC16-CCITT-FALSE 覆盖 TYPE..PAYLOAD，不含 SOF
    应答：0x06 ACK / 0x15 NAK / 0x07 DONE / 0x1B ABORT
"""

import argparse
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ota_uart import UART                                    # noqa: E402

# ------------------------------------------------------------------ 协议常量
APP_START = 0x08004000
APP_MAX_SIZE = 0xBC00                 # 47 KB
IMAGE_MAGIC = 0x3241544F              # "OTA2" 按小端读成 uint32
HDR_SIZE = 32

SOF = 0xA5
TYPE_HEAD, TYPE_DATA, TYPE_END = 0x01, 0x02, 0x03
ACK, NAK, DONE, ABORT = 0x06, 0x15, 0x07, 0x1B
CONTROL = {ACK: "ACK", NAK: "NAK", DONE: "DONE", ABORT: "ABORT"}

DEFAULT_HEX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                           "mdk", "Objects", "Freertos的嵌入式智能天气时钟.hex")


# ------------------------------------------------------------------ 小工具
def crc16(data):
    """CRC16-CCITT-FALSE：poly 0x1021 / init 0xFFFF / 不反射 / 末尾不取反。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) \
                else (crc << 1) & 0xFFFF
    return crc


def build_frame(ftype, seq, payload):
    """组一帧：6 B 头 + PAYLOAD + 2 B CRC16（CRC 覆盖头里除 SOF 外的 5 B）。"""
    head = struct.pack("<BBHH", SOF, ftype, seq, len(payload))
    return head + payload + struct.pack("<H", crc16(head[1:] + payload))


def parse_intel_hex(path):
    """还原出 [0x08004000, 0x08004000+len) 的连续固件字节，空洞用 0xFF 填。

    Keil 的 hex 里混着扩展线性地址(type 4)、起始地址(type 5)、EOF(type 1)，
    只取落在 APP 区内的 type 0 数据记录。长度取「最后一个写过的地址」，
    **绝不靠扫 0xFF 猜** —— 那是 11-14 日志里 size=47352 那个坑。
    """
    mem = bytearray([0xFF]) * APP_MAX_SIZE
    upper = 0
    highest = APP_START

    with open(path, "r", encoding="ascii", errors="ignore") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line.startswith(":"):
                continue
            try:
                rec = bytes.fromhex(line[1:])
            except ValueError:
                raise ValueError("第 %d 行不是合法 hex: %s" % (lineno, line))
            if len(rec) < 5 or (sum(rec) & 0xFF) != 0:
                raise ValueError("第 %d 行校验和不对" % lineno)

            count, rtype = rec[0], rec[3]
            payload = rec[4:4 + count]
            addr = (rec[1] << 8) | rec[2]

            if rtype == 0x04:                      # 扩展线性地址 -> 高 16 位
                upper = (payload[0] << 8) | payload[1]
            elif rtype == 0x00:                    # 数据
                full = (upper << 16) | addr
                if full < APP_START:
                    continue                       # Boot 区的记录不属于 APP
                off = full - APP_START
                if off + count > APP_MAX_SIZE:
                    raise ValueError("固件超出 APP 分区 47 KB")
                mem[off:off + count] = payload
                highest = max(highest, full + count)
            elif rtype in (0x01, 0x05, 0x02):     # EOF / 起始地址 / 段地址
                continue
            else:
                raise ValueError("不支持的 hex 记录类型 0x%02X" % rtype)

    length = highest - APP_START
    if length < 8 or length > APP_MAX_SIZE:
        raise ValueError("解析出来的固件长度异常：%d B" % length)

    # 内部 Flash 按半字编程，长度补齐到偶数；补的那字节是 0xFF，无害
    if length % 2:
        length += 1
    return bytes(mem[:length])


def make_image(fw, version):
    """32 B 镜像头 + 原始固件。crc32 用 zlib，和 Boot 的位运算版逐位一致。"""
    crc = zlib.crc32(fw) & 0xFFFFFFFF
    head = struct.pack("<8I", IMAGE_MAGIC, len(fw), crc, version, 0, 0, 0, 0)
    assert len(head) == HDR_SIZE
    return head, crc


# ------------------------------------------------------------------ 串口交互
def dump_log(uart, tail, timeout=0.02):
    """读一批字节，把非控制字符当日志按行打印，遇到控制码就返回它。"""
    data = uart.read(256)
    if not data:
        time.sleep(timeout)
        return None, tail

    for b in data:
        if b in CONTROL:
            return b, tail
        tail += bytes([b])
        if b == 0x0A:                              # '\n'
            text = tail.decode("ascii", "replace").strip()
            if text:
                print("   | " + text)
            tail = bytearray()
    return None, tail


def wait_reply(uart, uart_tail, expect, timeout):
    """等一个控制码，期间把 Boot 打的文本日志实时显示出来。"""
    deadline = time.time() + timeout
    tail = bytearray(uart_tail)
    while time.time() < deadline:
        code, tail = dump_log(uart, tail)
        if code is not None:
            if code == ABORT:
                raise RuntimeError("Boot 主动 ABORT（看上面的日志）")
            return code, tail
    return None, tail


def wait_prompt(uart, timeout=6.0):
    """发完 'U' 后等 Boot 打印 'waiting for firmware ...'。
    若板子本来就停在升级窗口（日志已经过去了），也允许继续，后面靠 ACK 判断。
    """
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = uart.read(256)
        if data:
            buf += data
            for b in data:
                if b == 0x0A:
                    text = buf.decode("ascii", "replace").strip()
                    if text:
                        print("   | " + text)
                    if "waiting for firmware" in text:
                        return True                  # 整行匹配，不怕跨包切断
                    buf = bytearray()
            if b"waiting for firmware" in data:
                return True
        else:
            time.sleep(0.01)
    return False


def send_frame(uart, tail, ftype, seq, payload, expect, timeout, retries=3):
    frame = build_frame(ftype, seq, payload)
    for attempt in range(retries + 1):
        uart.write(frame)
        code, tail = wait_reply(uart, tail, expect, timeout)
        if code == expect:
            return tail
        label = CONTROL.get(code, "超时/无应答")
        if attempt < retries:
            print("   ! 帧 %d 第 %d 次收到 %s，重发" % (seq, attempt + 1, label))
    raise RuntimeError("帧 %d（type=0x%02X）重发 %d 次仍没等到 %s" %
                       (seq, ftype, retries, CONTROL.get(expect)))


# ------------------------------------------------------------------ 主流程
def main():
    ap = argparse.ArgumentParser(description="STM32 天气时钟 OTA 串口上位机")
    ap.add_argument("--port", required=True, help="CH340 的 COM 口，例如 COM14")
    ap.add_argument("--hex", default=DEFAULT_HEX, help="Keil 生成的 APP hex")
    ap.add_argument("--bin", default=None, help="直接给一个已还原的 bin（不给则用 --hex）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--version", type=int, default=2, help="写进镜像头的版本号")
    ap.add_argument("--no-trigger", action="store_true",
                    help="板子已经停在 Boot 升级窗口时加这个，不再发 'U'")
    args = ap.parse_args()

    if args.bin:
        fw = open(args.bin, "rb").read()
    else:
        if not os.path.isfile(args.hex):
            print("找不到 hex：%s" % args.hex)
            print("请先用 Keil F7 编译 APP，或用 --hex / --bin 明确指定。")
            return 1
        fw = parse_intel_hex(args.hex)

    if len(fw) > APP_MAX_SIZE:
        print("固件 %d B 超过 APP 分区 %d B" % (len(fw), APP_MAX_SIZE))
        return 1

    head, crc = make_image(fw, args.version)
    chunks = [fw[i:i + 128] for i in range(0, len(fw), 128)]

    print("=" * 62)
    print(" OTA 升级包")
    print("   源      : %s" % (args.bin or args.hex))
    print("   固件长度: %d B (0x%X)，分 %d 帧" % (len(fw), len(fw), len(chunks)))
    print("   CRC32   : 0x%08X" % crc)
    print("   版本    : %d" % args.version)
    print("   串口    : %s @ %d" % (args.port, args.baud))
    print("=" * 62)

    with UART(args.port, args.baud) as uart:
        if not args.no_trigger:
            print("\n[1/5] 发 'U' 让板子回到 Boot ...")
            uart.write(b"U")
            if not wait_prompt(uart):
                print("   ! 没等到 'waiting for firmware'，可能是板子已在升级窗口，继续尝试")
        else:
            print("\n[1/5] 跳过触发（--no-trigger）")

        # Boot 打印完 "waiting for firmware" 后还要走完 TC 等待才进入收帧循环。
        # 串口没有 FIFO，稍微让一点时间，避免 HEAD 整帧在 Boot 还没开始
        # 紧轮询时就被溢出丢掉（真丢了也能靠重发救回来，只是要白等 8 s）。
        time.sleep(0.15)

        print("[2/5] 发 HEAD（镜像头 32 B）...")
        tail = send_frame(uart, bytearray(), TYPE_HEAD, 0, head, ACK, 8.0)

        print("[3/5] 发 DATA（每帧 128 B，逐帧等 ACK）...")
        t0 = time.time()
        for seq, chunk in enumerate(chunks):
            tail = send_frame(uart, tail, TYPE_DATA, seq, chunk, ACK, 3.0)
            if (seq + 1) % 32 == 0 or seq + 1 == len(chunks):
                print("   %4d/%4d 帧  %6d/%d B" %
                      (seq + 1, len(chunks), min((seq + 1) * 128, len(fw)), len(fw)))
        print("   发送耗时 %.1f s" % (time.time() - t0))

        print("[4/5] 发 END，Boot 开始 CRC32 校验 + 擦写 APP（约 2~5 s）...")
        tail = send_frame(uart, tail, TYPE_END, len(chunks), b"", DONE, 30.0, retries=0)

        print("[5/5] DONE！读取复位后的启动日志 ...\n")
        deadline = time.time() + 5.0
        while time.time() < deadline:
            _, tail = dump_log(uart, tail, 0.01)

    print("\n" + "=" * 62)
    print(" 升级完成。核对上面复位后的日志：")
    print("   app @0x08004000 valid, scan_end=<真实长度>   <- 不再是 47352")
    print("   jumping to app... + 天气时钟屏幕亮起")
    print("=" * 62)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as e:
        print("\n[失败] %s" % e)
        sys.exit(1)
