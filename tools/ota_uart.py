"""
零依赖 Windows 串口层（ctypes 直接调 kernel32）

为什么不 import pyserial：这台机器的 pip 连不上外网，装不了第三方库。
而 CH340 在 Windows 上就是一个普通的 COM 口，用系统自带的 CreateFile /
SetCommState / ReadFile / WriteFile 就能读写，全部用 Python 标准库完成。

对外只暴露一个 UART 类，接口故意做得像 pyserial：
    uart = UART("COM14", 115200)
    uart.write(b"U")
    data = uart.read(1)      # 非阻塞，没数据返回 b""
    uart.close()
"""

import ctypes
from ctypes import wintypes

# ---------------------------------------------------------------- Win32 常量
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
PURGE_RXCLEAR = 0x0008
PURGE_TXCLEAR = 0x0004
MAXDWORD = 0xFFFFFFFF

NOPARITY = 0
ONESTOPBIT = 0

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class _DCB(ctypes.Structure):
    """Win32 DCB 结构。位域部分合并成一个 DWORD，只开 fBinary(bit0)。"""
    _fields_ = [
        ("DCBlength", wintypes.DWORD),
        ("BaudRate", wintypes.DWORD),
        ("_bits", wintypes.DWORD),
        ("wReserved", wintypes.WORD),
        ("XonLim", wintypes.WORD),
        ("XoffLim", wintypes.WORD),
        ("ByteSize", ctypes.c_byte),
        ("Parity", ctypes.c_byte),
        ("StopBits", ctypes.c_byte),
        ("XonChar", ctypes.c_char),
        ("XoffChar", ctypes.c_char),
        ("ErrorChar", ctypes.c_char),
        ("EofChar", ctypes.c_char),
        ("EvtChar", ctypes.c_char),
        ("wReserved1", wintypes.WORD),
    ]


class _COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [
        ("ReadIntervalTimeout", wintypes.DWORD),
        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
        ("ReadTotalTimeoutConstant", wintypes.DWORD),
        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
        ("WriteTotalTimeoutConstant", wintypes.DWORD),
    ]


_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                             wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                             wintypes.HANDLE]
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.SetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(_DCB)]
_k32.GetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(_DCB)]
_k32.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.POINTER(_COMMTIMEOUTS)]
_k32.PurgeComm.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                           ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
_k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                          ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]


def _err(what):
    raise OSError("%s 失败，Win32 error = %d" %
                  (what, ctypes.get_last_error()))


class UART:
    def __init__(self, port, baud=115200):
        # COM10 以上必须写成 \\.\COMxx，否则 CreateFile 打不开
        path = "\\\\.\\" + port.upper()
        self.handle = _k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                       0, None, OPEN_EXISTING, 0, None)
        if not self.handle or self.handle == INVALID_HANDLE_VALUE:
            _err("打开 " + path)

        dcb = _DCB()
        dcb.DCBlength = ctypes.sizeof(_DCB)
        if not _k32.GetCommState(self.handle, ctypes.byref(dcb)):
            _err("GetCommState")

        dcb.BaudRate = baud
        dcb._bits = 1                 # fBinary=1；硬件流控、XON/XOFF 全关
        dcb.ByteSize = 8
        dcb.Parity = NOPARITY
        dcb.StopBits = ONESTOPBIT
        if not _k32.SetCommState(self.handle, ctypes.byref(dcb)):
            _err("SetCommState")

        # 这个超时组合的含义：ReadFile 立刻返回「当前已经收到的字节」，不等待。
        # 超时由 ota_send.py 自己用 time.time() 管理，才能一边等 ACK 一边打印日志。
        to = _COMMTIMEOUTS()
        to.ReadIntervalTimeout = MAXDWORD
        to.ReadTotalTimeoutMultiplier = 0
        to.ReadTotalTimeoutConstant = 0
        to.WriteTotalTimeoutMultiplier = 0
        to.WriteTotalTimeoutConstant = 0
        if not _k32.SetCommTimeouts(self.handle, ctypes.byref(to)):
            _err("SetCommTimeouts")

        _k32.PurgeComm(self.handle, PURGE_RXCLEAR | PURGE_TXCLEAR)

    def write(self, data):
        if isinstance(data, str):
            data = data.encode("ascii")
        # 用 create_string_buffer 明确拿到指针，避免不同 Python 版本对
        # "bytes 传给 c_void_p" 的隐式转换行为不一致。
        buf = ctypes.create_string_buffer(data)
        written = wintypes.DWORD(0)
        ok = _k32.WriteFile(self.handle, buf, len(data),
                            ctypes.byref(written), None)
        if not ok or written.value != len(data):
            _err("WriteFile")
        return written.value

    def read(self, n=1):
        buf = ctypes.create_string_buffer(n)
        got = wintypes.DWORD(0)
        _k32.ReadFile(self.handle, buf, n, ctypes.byref(got), None)
        return buf.raw[:got.value]

    def close(self):
        if self.handle:
            _k32.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
