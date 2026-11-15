"""
OTA 上位机：把 APP 固件通过串口发给 BootLoader（零第三方依赖）

用法（在仓库根目录执行）：
    python tools/ota_send.py --port COM14
    python tools/ota_send.py --port COM14 --hex mdk/Objects/Freertos的嵌入式智能天气时钟.hex
    python tools/ota_send.py --port COM14 --corrupt 100     # 第④步：故意传坏包
    python tools/ota_send.py --port COM14 --truncate 2      # 第④步：故意少发 2 帧
    python tools/ota_send.py --port COM14 --brick --yes     # 第⑤步：刷一份跑不起来的固件
    python tools/ota_send.py --port COM14 --monitor         # 只听串口日志，按 RESET 观察

--corrupt / --truncate 是「负面测试」开关：让 Boot 收到一包 CRC32 对不上
或根本没收全的固件，用来验证「APP 区一个字节都不擦、原固件照常启动」。
这两种模式不会等 DONE，只把 Boot 打印的日志完整显示出来。

--brick 是「破坏性测试」：保留向量表（MSP/PC 合法），只把 Reset_Handler
入口的 32 B 改成 0xFF，并按坏数据重算 CRC32 —— 于是所有校验都会通过，
Boot 真的会把这份一上电就 HardFault 的固件写进 APP 区。用来验证试跑计数
三次失败后自动停 Boot 等重升。**Boot 区永远不会被擦，跑一次正常升级即可救回。**

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


def dump_plain(uart, tail, timeout=0.02):
    """负面测试专用：只打印文本日志，控制码（ACK/NAK/ABORT）一律忽略。
    返回 (tail, lines)，lines 是本轮收到的完整文本行，供上层判定回退是否成功。"""
    data = uart.read(256)
    lines = []
    if not data:
        time.sleep(timeout)
        return tail, lines

    for b in data:
        if b in CONTROL:
            continue
        tail += bytes([b])
        if b == 0x0A:
            text = tail.decode("ascii", "replace").strip()
            if text:
                print("   | " + text)
                lines.append(text)
            tail = bytearray()
    return tail, lines


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
    ap.add_argument("--corrupt", type=int, default=None, metavar="N",
                    help="负面测试：把第 N 帧 DATA 的首字节翻转 1 bit（帧 CRC16 仍合法，整包 CRC32 会挂）")
    ap.add_argument("--truncate", type=int, default=0, metavar="N",
                    help="负面测试：故意少发最后 N 帧 DATA，触发 premature END")
    ap.add_argument("--brick", action="store_true",
                    help="破坏性测试：刷一份向量表合法但一上电就 HardFault 的固件")
    ap.add_argument("--yes", action="store_true", help="确认执行 --brick（不加会拒绝运行）")
    ap.add_argument("--monitor", action="store_true",
                    help="只打开串口打印日志，不发任何命令；按 RESET 观察，Ctrl+C 退出")
    args = ap.parse_args()

    if args.monitor and (args.brick or args.corrupt is not None or args.truncate):
        print("--monitor 不能和任何测试开关一起用")
        return 1
    if args.brick and (args.corrupt is not None or args.truncate):
        print("--brick 不能和 --corrupt / --truncate 一起用")
        return 1

    if args.monitor:
        print("=" * 62)
        print(" 串口监听（不发任何命令）")
        print("   按板子 RESET 键观察 Boot 日志，Ctrl+C 退出后 COM 口才会释放")
        print("=" * 62)
        with UART(args.port, args.baud) as uart:
            tail = bytearray()
            try:
                while True:
                    tail, _ = dump_plain(uart, tail, 0.02)
            except KeyboardInterrupt:
                print("\n监听结束，串口已释放。")
        return 0

    if args.brick and not args.yes:
        print("=" * 62)
        print(" --brick 会真的把一份跑不起来的固件写进 APP 区")
        print("   · 天气时钟会黑屏 / 卡死，boot_try 没人清")
        print("   · 需要手动按 RESET 三次，Boot 才会停住等重升")
        print("   · Boot 区永不擦写，跑一次正常升级即可救回，不会永久变砖")
        print(" 确认要继续，请在命令末尾再加 --yes")
        print("=" * 62)
        return 1

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

    if args.brick:
        # 目标：让所有校验都通过、Boot 真的把固件写进 APP 区，但跳过去立刻死。
        #   ① 前 8 字节（MSP / 复位向量）必须原样保留 —— boot_app_check()
        #      只查这两项，改了会在 ota_commit() 第⑤步被拒，测不到跳转失败。
        #   ② 复位向量指向 Reset_Handler，把入口 32 B 换成 0xFF（Thumb
        #      未定义指令）→ 一上电就 HardFault，屏幕不亮、没人调
        #      app_boot_confirm()，boot_try 就会一直累加。
        #   ③ 按改坏后的数据重算整包 CRC32 写进镜像头 —— 于是 CRC32 /
        #      读回校验 / 向量表检查全部通过，效果等同于「刷了个坏固件」。
        pc = int.from_bytes(fw[4:8], "little") & ~1
        off = pc - APP_START
        if not (0x100 <= off <= len(fw) - 32):
            raise ValueError("复位向量 0x%08X 超出固件范围，拒绝构造" % pc)
        bad = bytearray(fw)
        bad[off:off + 32] = b"\xFF" * 32
        fw = bytes(bad)
        print("! 破坏性模式：Reset_Handler @0x%08X 入口 32 B 已改成 0xFF" % pc)
        print("! CRC32 按坏数据重算，Boot 会正常收下并写入 APP 区")

    head, crc = make_image(fw, args.version)
    chunks = [fw[i:i + 128] for i in range(0, len(fw), 128)]

    fault_mode = (args.corrupt is not None) or (args.truncate > 0)
    if args.corrupt is not None and not (0 <= args.corrupt < len(chunks)):
        print("--corrupt 必须在 0 ~ %d 之间" % (len(chunks) - 1))
        return 1
    if args.truncate and not (1 <= args.truncate < len(chunks)):
        print("--truncate 必须在 1 ~ %d 之间" % (len(chunks) - 1))
        return 1

    mode = ("（负面测试模式，故意失败）" if fault_mode else
            "（破坏性模式，会刷进跑不起来的固件）" if args.brick else "")
    print("=" * 62)
    print(" OTA 升级包%s" % mode)
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
        sent = 0
        total = len(chunks) - args.truncate
        for seq, chunk in enumerate(chunks):
            if args.truncate and seq >= total:
                print("   ! 故意少发最后 %d 帧（--truncate）" % args.truncate)
                break
            if args.corrupt is not None and seq == args.corrupt:
                bad = bytearray(chunk)
                bad[0] ^= 0x01                  # 只翻 1 bit：帧 CRC16 重算后仍合法
                chunk = bytes(bad)
                print("   ! 故意翻转第 %d 帧的第 1 个 bit（--corrupt）" % seq)

            tail = send_frame(uart, tail, TYPE_DATA, seq, chunk, ACK, 3.0)
            sent += 1
            if sent % 32 == 0 or sent == total:
                print("   %4d/%4d 帧  %6d/%d B" %
                      (sent, len(chunks), min(sent * 128, len(fw)), len(fw)))
        print("   发送耗时 %.1f s" % (time.time() - t0))

        if fault_mode:
            print("[4/5] 负面测试：发 END 后只收日志，不等 DONE ...")
            uart.write(build_frame(TYPE_END, sent, b""))

            # --truncate 的 END 被判成 premature 后，Boot 不是立刻退出，而是
            # 继续等下一帧：要连吃 4 次 3 s 帧超时（约 12 s）才 abort 回主流程，
            # 那时候才会打印 [SAFE] 和 jumping to app。早期版本只等 8 s，
            # 把最关键的「回退证据」掐掉了，所以这里等够并盯着关键行。
            deadline = time.time() + 25.0
            seen = {"reject": False, "safe": False, "jump": False}
            while time.time() < deadline:
                tail, lines = dump_plain(uart, tail, 0.01)
                for text in lines:
                    if "CRC MISMATCH" in text or "premature END" in text:
                        seen["reject"] = True
                    if "[SAFE]" in text:
                        seen["safe"] = True
                    if "jumping to app" in text:
                        seen["jump"] = True
                if seen["jump"]:
                    break

            print("\n" + "=" * 62)
            print(" 负面测试结束（这次失败是故意的）")
            print("   坏包被拒 : %s" % ("是" if seen["reject"] else "未捕获"))
            print("   APP 未动 : %s" % ("是" if seen["safe"] else "未捕获"))
            print("   回退启动 : %s" % ("是" if seen["jump"] else "未捕获"))
            print("   结论     : %s" %
                  ("PASS - 原固件完好，板子已回到天气时钟" if all(seen.values())
                   else "日志未收全，请确认板子是否仍停在升级窗口（30 s 超时后自动退出）"))
            print("=" * 62)
            return 0

        print("[4/5] 发 END，Boot 开始 CRC32 校验 + 擦写 APP（约 2~5 s）...")
        tail = send_frame(uart, tail, TYPE_END, len(chunks), b"", DONE, 30.0, retries=0)

        print("[5/5] %s读取复位后的启动日志 ...\n" %
              ("坏固件已写入！" if args.brick else "DONE！"))
        deadline = time.time() + 5.0
        while time.time() < deadline:
            _, tail = dump_log(uart, tail, 0.01)

    if args.brick:
        print("\n" + "=" * 62)
        print(" 破坏性固件已写入（这是故意的）")
        print("   屏幕应该黑着 / 卡死，APP 不会打印 boot confirmed")
        print("")
        print(" 下一步：另开窗口跑 python tools/ota_send.py --port %s --monitor" % args.port)
        print("   然后按板子 RESET 键，观察：")
        print("     第 1 次 -> param: boot_try=1  /  trial boot #2")
        print("     第 2 次 -> param: boot_try=2  /  trial boot #3")
        print("     第 3 次 -> boot_try >= 3 -> trial FAILED, stay in boot")
        print("             -> waiting for recovery firmware...")
        print("")
        print(" 救砖（Ctrl+C 退出监听，释放 COM 口后）：")
        print("   python tools/ota_send.py --port %s" % args.port)
        print("   Boot 会自动开升级窗口，正常升级一次即可回到天气时钟")
        print("=" * 62)
        return 0

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
