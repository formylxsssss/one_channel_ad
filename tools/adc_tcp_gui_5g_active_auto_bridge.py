#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STM32F427 + LAN8720A + ADS127L11 5G主动连接版上位机 - 0/20/40dB增益 + 自动归零 + 本地分包保存版

本版重点：
1. 每个 TCP DATA 帧都以“下位机帧头里的 bits / bytes_per_sample”为准解析，
   不再依赖界面下拉框，避免 16位/24位不一致导致错位。
2. 支持 0dB / 20dB / 40dB 三档增益选择，并把 ADC 电压按增益折算为输入端电压 V。
3. 保留 raw code 统计，方便判断短接是否接近 0。
4. 开始采集后默认打印前 N 个 DATA 帧调试信息：
   - fs / bits / bps / sample_count / data_bytes
   - payload 前 16 字节 hex
   - raw 前 10 个码值
   - voltage 前 10 个电压值
5. 采集中默认不自动 GET_PARAM，避免下位机持续打印 GET_PARAM。
6. 支持本地保存 raw / wav / bin 三种格式：
   - raw：只保存下位机 DATA payload 里的 ADC 原始字节，完全不加头。
   - wav：保存为单声道 PCM WAV，16bit/24bit 自动匹配，便于 Audacity/Matlab/Python 直接打开。
   - bin：保存自定义带时间戳和采样序号的二进制帧，便于后期精确排查丢样。
7. 保存文件按电脑本地时间每 1 小时自动分包：
   - 文件名格式：基础名_YYYYMMDD_HH.MM.扩展名
   - 例如 17:20 开始采集，第一包为 *_17.20.raw，第二包为 *_18.20.raw。

依赖：
    pip install numpy matplotlib

运行：
    python adc_tcp_gui_voltage_zero_time_fft_adaptive_fft_axis.py
"""

import os
import asyncio
import queue
import socket
import struct
import threading
import time
import wave
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from collections import deque
from typing import Optional, Set

import numpy as np
import matplotlib
matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure


# ==============================
# 协议定义，必须与 STM32 下位机 adc_tcp_server.c 保持一致
# ==============================
MAGIC = 0xA55A
MAGIC_BYTES = b"\x5A\xA5"
VER = 1

DEFAULT_IP = "127.0.0.1"        # 这里填写“公网服务器IP”，不是采集器IP
DEFAULT_PORT = 9001             # 上位机连接服务器GUI端口；设备连接服务器9000端口

DEFAULT_DEVICE_SERVER_IP = "127.0.0.1"
DEFAULT_DEVICE_SERVER_PORT = 9000
DEFAULT_DEVICE_ID = 1

TYPE_SET_PARAM = 0x01
TYPE_GET_PARAM = 0x02
TYPE_START = 0x03
TYPE_STOP = 0x04
TYPE_ACK = 0x10
TYPE_DATA = 0x81
TYPE_SET_NET = 0x20
TYPE_GET_NET = 0x21
TYPE_NET_ACK = 0x22
TYPE_REGISTER = 0x30

NET_SET_FLAG_SAVE = 0x01
NET_SET_FLAG_RECONNECT = 0x02

VALID_FS = [400000, 200000, 100000, 50000, 25000]
VALID_BITS = [24, 16]
VALID_RANGES = [0, 1, 2]
VALID_SAVE_FORMATS = ["raw", "wav", "bin"]
SAVE_SPLIT_SECONDS = 3600.0  # 每 1 小时自动分包，按电脑本地时间命名

FRAME_HDR_LEN = 10
FRAME_CRC_LEN = 2
FRAME_MAX_PAYLOAD = 16384

# 协议字段仍然叫 range，为了兼容下位机旧协议；
# 现在的软件含义改为“增益档位”：
#   0 -> CH1_D0 -> 0dB  -> x1
#   1 -> CH1_D1 -> 20dB -> x10
#   2 -> CH1_D2 -> 40dB -> x100
VALID_GAIN_DB = {0: 0, 1: 20, 2: 40}
GAIN_LINEAR = {0: 1.0, 1: 10.0, 2: 100.0}
RANGE_TEXT = {
    0: "0 - 0dB / x1 / CH1_D0",
    1: "1 - 20dB / x10 / CH1_D1",
    2: "2 - 40dB / x100 / CH1_D2",
}


def range_to_gain_db(rng: int) -> int:
    return int(VALID_GAIN_DB.get(int(rng), 0))


def range_to_gain_linear(rng: int) -> float:
    return float(GAIN_LINEAR.get(int(rng), 1.0))


def range_to_text(rng: int) -> str:
    return RANGE_TEXT.get(int(rng), f"{rng} - 未知增益")


# ==============================
# 协议/数据工具函数
# ==============================
def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = ((crc >> 1) ^ 0xA001) & 0xFFFF
            else:
                crc >>= 1
    return crc & 0xFFFF


def build_frame(ftype: int, seq: int, payload: bytes = b"") -> bytes:
    hdr = struct.pack("<HBBHI", MAGIC, VER, ftype, len(payload), seq & 0xFFFFFFFF)
    body = hdr + payload
    return body + struct.pack("<H", crc16_modbus(body))


def parse_ack(payload: bytes) -> dict:
    """
    ACK 基础字段：
      int32  status
      uint32 fs
      uint8  bits
      uint8  range
      uint8  running
      uint8  reserved
      uint32 overrun
      uint32 sample_seq

    扩展字段：
      uint32 flags
      uint32 ready
      uint32 queued
      uint32 free
    """
    if len(payload) < 20:
        raise ValueError(f"ACK payload too short: {len(payload)}")

    status, fs = struct.unpack_from("<iI", payload, 0)
    bits = payload[8]
    rng = payload[9]
    running = payload[10]
    overrun = struct.unpack_from("<I", payload, 12)[0]
    sample_seq = struct.unpack_from("<I", payload, 16)[0]

    ack = {
        "status": status,
        "fs": fs,
        "bits": bits,
        "range": rng,
        "running": running,
        "overrun": overrun,
        "sample_seq": sample_seq,
        "flags": 0,
        "ready": 0,
        "queued": 0,
        "free": 0,
    }

    if len(payload) >= 24:
        ack["flags"] = struct.unpack_from("<I", payload, 20)[0]
    if len(payload) >= 32:
        ack["ready"], ack["queued"], ack["free"], ack["sndbuf"] = struct.unpack_from("<HHHH", payload, 24)
    if len(payload) >= 44:
        ack["unacked_total_bytes"], ack["tcp_err_mem"], ack["acked_data_frames"] = struct.unpack_from("<III", payload, 32)

    return ack


def parse_ipv4_text(ip_text: str) -> bytes:
    parts = ip_text.strip().split(".")
    if len(parts) != 4:
        raise ValueError("IP必须是a.b.c.d格式")
    vals = []
    for p in parts:
        v = int(p)
        if v < 0 or v > 255:
            raise ValueError("IP每段必须在0~255之间")
        vals.append(v)
    if vals[0] == 0 or vals[3] in (0, 255):
        raise ValueError("IP地址明显无效")
    return bytes(vals)


def format_ipv4_bytes(b: bytes | bytearray) -> str:
    if len(b) < 4:
        return "0.0.0.0"
    return f"{int(b[0])}.{int(b[1])}.{int(b[2])}.{int(b[3])}"


def build_set_net_payload(server_ip: str, server_port: int, device_id: int, save: bool = True, reconnect: bool = True) -> bytes:
    ipb = parse_ipv4_text(server_ip)
    if server_port <= 0 or server_port > 65535:
        raise ValueError("服务器端口必须在1~65535之间")
    if device_id <= 0 or device_id > 255:
        raise ValueError("设备ID必须在1~255之间")
    flags = 0
    if save:
        flags |= NET_SET_FLAG_SAVE
    if reconnect:
        flags |= NET_SET_FLAG_RECONNECT
    return ipb + struct.pack("<HBB", int(server_port), int(device_id), int(flags))


def parse_net_ack(payload: bytes) -> dict:
    if len(payload) < 16:
        raise ValueError(f"NET_ACK payload too short: {len(payload)}")
    status = struct.unpack_from("<i", payload, 0)[0]
    ip = format_ipv4_bytes(payload[4:8])
    port = struct.unpack_from("<H", payload, 8)[0]
    dev_id = payload[10]
    connected = payload[11]
    reconnect_pending = payload[12]
    info = {
        "status": status,
        "server_ip": ip,
        "server_port": port,
        "device_id": dev_id,
        "connected": connected,
        "reconnect_pending": reconnect_pending,
        "local_ip": "",
        "netmask": "",
        "gateway": "",
        "reconnect_ms": 0,
    }
    if len(payload) >= 36:
        info["local_ip"] = format_ipv4_bytes(payload[16:20])
        info["netmask"] = format_ipv4_bytes(payload[20:24])
        info["gateway"] = format_ipv4_bytes(payload[24:28])
        info["reconnect_ms"] = struct.unpack_from("<I", payload, 28)[0]
    return info


def raw_adc_to_int(raw: bytes, bytes_per_sample: int) -> np.ndarray:
    """
    解析下位机 TCP DATA 帧里的 ADC 原始字节。
    注意：这里完全按 DATA 帧头里的 bps 解析，不看界面位数选择框。

    bps=3：
        24bit，MSB first，二补码，范围 [-8388608, 8388607]

    bps=2：
        16bit，MSB first，二补码，范围 [-32768, 32767]
    """
    if bytes_per_sample == 3:
        n = len(raw) // 3
        if n <= 0:
            return np.empty(0, dtype=np.int32)
        u8 = np.frombuffer(raw[:n * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        val = (u8[:, 0] << 16) | (u8[:, 1] << 8) | u8[:, 2]
        val = np.where((val & 0x800000) != 0, val - 0x1000000, val)
        return val.astype(np.int32)

    if bytes_per_sample == 2:
        n = len(raw) // 2
        if n <= 0:
            return np.empty(0, dtype=np.int32)
        return np.frombuffer(raw[:n * 2], dtype=">i2").astype(np.int32)

    return np.empty(0, dtype=np.int32)


def full_scale_code_from_bps(bits: int, bps: int) -> int:
    if bps == 3 or bits == 24:
        return 8388607
    if bps == 2 or bits == 16:
        return 32767
    return 1


def raw_to_voltage(raw_code: np.ndarray, bits: int, bps: int, fs_voltage: float) -> np.ndarray:
    fs_code = full_scale_code_from_bps(bits, bps)
    if fs_code <= 0:
        return np.zeros_like(raw_code, dtype=np.float64)
    return raw_code.astype(np.float64) / float(fs_code) * float(fs_voltage)


def voltage_to_raw_codes(voltage: np.ndarray, bits: int, bps: int, fs_voltage: float) -> np.ndarray:
    """
    把已经换算成电压的波形重新换算成 ADC 二补码码值。
    主要用于 WAV 保存“归零后”的波形；raw/bin 仍然保存板子发来的原始字节。
    """
    if voltage is None or voltage.size == 0 or fs_voltage <= 0.0:
        return np.empty(0, dtype=np.int32)

    fs_code = full_scale_code_from_bps(bits, bps)
    y = np.rint(voltage.astype(np.float64) / float(fs_voltage) * float(fs_code)).astype(np.int64)

    if bps == 3 or bits == 24:
        y = np.clip(y, -8388608, 8388607)
    elif bps == 2 or bits == 16:
        y = np.clip(y, -32768, 32767)
    else:
        y = np.zeros_like(y)

    return y.astype(np.int32)


def raw_stats(y: np.ndarray, bits: int, bps: int, near_fs_percent: float = 95.0) -> dict:
    if y is None or y.size == 0:
        return {
            "valid": False,
            "count": 0,
            "min": 0,
            "max": 0,
            "mean": 0.0,
            "std": 0.0,
            "ptp": 0,
            "max_abs": 0,
            "fs_code": full_scale_code_from_bps(bits, bps),
            "max_abs_pct": 0.0,
            "near_fs_count": 0,
            "near_fs_pct": 0.0,
        }

    yi = y.astype(np.int64)
    fs_code = full_scale_code_from_bps(bits, bps)
    max_abs = int(np.max(np.abs(yi)))
    near_th = int(fs_code * near_fs_percent / 100.0)
    near_cnt = int(np.sum(np.abs(yi) >= near_th))

    return {
        "valid": True,
        "count": int(yi.size),
        "min": int(np.min(yi)),
        "max": int(np.max(yi)),
        "mean": float(np.mean(yi)),
        "std": float(np.std(yi)),
        "ptp": int(np.max(yi) - np.min(yi)),
        "max_abs": max_abs,
        "fs_code": fs_code,
        "max_abs_pct": (max_abs / fs_code * 100.0) if fs_code else 0.0,
        "near_fs_count": near_cnt,
        "near_fs_pct": near_cnt / yi.size * 100.0,
    }


def volt_stats(v: np.ndarray) -> dict:
    if v is None or v.size == 0:
        return {
            "valid": False,
            "count": 0,
            "min": 0.0,
            "max": 0.0,
            "mean": 0.0,
            "std": 0.0,
            "ptp": 0.0,
            "max_abs": 0.0,
        }

    return {
        "valid": True,
        "count": int(v.size),
        "min": float(np.min(v)),
        "max": float(np.max(v)),
        "mean": float(np.mean(v)),
        "std": float(np.std(v)),
        "ptp": float(np.max(v) - np.min(v)),
        "max_abs": float(np.max(np.abs(v))),
    }


def judge_short_input(stats: dict, bits: int, bps: int, zero_threshold_lsb: int, noise_threshold_lsb: int) -> tuple[str, str]:
    if not stats.get("valid"):
        return "WAIT", "等待数据"

    max_abs = stats["max_abs"]
    std = stats["std"]
    near_pct = stats["near_fs_pct"]

    if near_pct > 1.0 or stats["max_abs_pct"] > 80.0:
        return "FAIL", "接近满量程，优先怀疑帧错位、位数不匹配、STATUS字节混入或模拟前端饱和"

    if max_abs > zero_threshold_lsb:
        return "WARN", f"短接后最大绝对码值偏大，max_abs={max_abs} > {zero_threshold_lsb}"

    if std > noise_threshold_lsb:
        return "WARN", f"短接后噪声偏大，std={std:.1f} > {noise_threshold_lsb}"

    return "OK", "短接原始码值接近0"


def hex_preview(data: bytes, max_len: int = 16) -> str:
    d = data[:max_len]
    return " ".join(f"{b:02X}" for b in d)



def adc_codes_to_wav_pcm_bytes(raw_codes: np.ndarray, bps: int) -> bytes:
    """
    把 ADC 二补码整数转成 WAV PCM 字节。

    bps=2：16bit PCM，小端 int16。
    bps=3：24bit PCM，小端 3字节二补码。
    """
    if raw_codes is None or raw_codes.size == 0:
        return b""

    if bps == 2:
        y = np.clip(raw_codes.astype(np.int64), -32768, 32767).astype("<i2")
        return y.tobytes()

    if bps == 3:
        y = np.clip(raw_codes.astype(np.int64), -8388608, 8388607).astype(np.int32)
        u = (y & 0x00FFFFFF).astype(np.uint32)
        out = np.empty(y.size * 3, dtype=np.uint8)
        out[0::3] = (u & 0xFF).astype(np.uint8)
        out[1::3] = ((u >> 8) & 0xFF).astype(np.uint8)
        out[2::3] = ((u >> 16) & 0xFF).astype(np.uint8)
        return out.tobytes()

    return b""


def default_save_path(fmt: str) -> str:
    """
    这里返回的是“基础路径”，真正保存时会自动变成：
        基础名_YYYYMMDD_HH.MM.ext
    """
    fmt = (fmt or "raw").lower()
    if fmt not in VALID_SAVE_FORMATS:
        fmt = "raw"
    return os.path.abspath(f"adc_capture.{fmt}")


def normalize_save_path(path: str, fmt: str) -> str:
    """
    归一化用户选择的基础路径。
    注意：这不是最终分包文件名。最终分包文件名由 make_split_save_path() 根据电脑时间生成。
    """
    fmt = (fmt or "raw").lower()
    if fmt not in VALID_SAVE_FORMATS:
        fmt = "raw"
    if not path:
        return default_save_path(fmt)

    path = os.path.abspath(path)
    root, ext = os.path.splitext(path)
    need_ext = "." + fmt
    if ext.lower() != need_ext:
        path = root + need_ext
    return path


def make_split_save_path(base_path: str, fmt: str, part_start_ts: float, part_index: int) -> str:
    """
    生成当前分包文件名。

    例：
        base_path = D:/data/adc_capture.raw
        part_start_ts 对应 2026-05-19 17:20:xx
        输出：D:/data/adc_capture_20260519_17.20.raw

    如果同名文件已存在，会自动追加 _02 / _03，避免覆盖。
    """
    fmt = (fmt or "raw").lower()
    if fmt not in VALID_SAVE_FORMATS:
        fmt = "raw"

    base_path = normalize_save_path(base_path, fmt)
    folder = os.path.dirname(base_path) or os.getcwd()
    stem = os.path.splitext(os.path.basename(base_path))[0]
    if not stem:
        stem = "adc_capture"

    tm = time.localtime(part_start_ts)
    time_tag = time.strftime("%Y%m%d_%H.%M", tm)
    name = f"{stem}_{time_tag}.{fmt}"
    path = os.path.join(folder, name)

    if not os.path.exists(path):
        return path

    # 同一分钟内重复开始采集，避免覆盖旧文件。
    for n in range(2, 1000):
        alt = os.path.join(folder, f"{stem}_{time_tag}_{n:02d}.{fmt}")
        if not os.path.exists(alt):
            return alt

    # 极端情况兜底。
    return os.path.join(folder, f"{stem}_{time_tag}_{part_index + 1}_{int(time.time())}.{fmt}")


def format_hhmm(ts: float) -> str:
    return time.strftime("%H.%M", time.localtime(ts))



# ==============================
# 内置 TCP Bridge 服务线程
# ==============================
class BridgeServerThread(threading.Thread):
    """
    GUI 启动时自动运行的中转服务。

    端口关系：
      STM32 下位机 -> 本机 device_port，默认 9000
      GUI 上位机   -> 本机 gui_port，默认 9001

    这样用户不需要再手动运行 adc_5g_bridge_server.py。
    """

    def __init__(self,
                 event_queue: queue.Queue,
                 host: str = "0.0.0.0",
                 device_port: int = 9000,
                 gui_port: int = 9001):
        super().__init__(daemon=True)
        self.event_queue = event_queue
        self.host = host
        self.device_port = int(device_port)
        self.gui_port = int(gui_port)
        self.stop_event = threading.Event()

        self.device_writer: Optional[asyncio.StreamWriter] = None
        self.gui_writers: Set[asyncio.StreamWriter] = set()
        self.dev_server = None
        self.gui_server = None

        # 用于 GUI 关闭时从 Tk 主线程通知 asyncio 线程立即释放端口。
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.started_event = threading.Event()
        self.stopped_event = threading.Event()
        self.failed_event = threading.Event()
        self.failed_reason = ""

    def _post_log(self, msg: str):
        now = time.strftime("%H:%M:%S")
        self.event_queue.put(("log", f"[{now}] [BRIDGE] {msg}"))

    @staticmethod
    def _peer(writer: asyncio.StreamWriter) -> str:
        try:
            info = writer.get_extra_info("peername")
            return str(info) if info else "unknown"
        except Exception:
            return "unknown"

    @staticmethod
    def _close_writer(writer: Optional[asyncio.StreamWriter]):
        if writer is None:
            return
        try:
            writer.close()
        except Exception:
            pass

    def _close_all_in_loop(self):
        """必须在 Bridge 的 asyncio 线程内调用。用于立即关闭监听端口和连接。"""
        try:
            if self.dev_server is not None:
                self.dev_server.close()
        except Exception:
            pass

        try:
            if self.gui_server is not None:
                self.gui_server.close()
        except Exception:
            pass

        try:
            self._close_writer(self.device_writer)
            self.device_writer = None
        except Exception:
            pass

        try:
            for w in list(self.gui_writers):
                self._close_writer(w)
            self.gui_writers.clear()
        except Exception:
            pass

    async def _safe_drain(self, writer: asyncio.StreamWriter) -> bool:
        try:
            await writer.drain()
            return True
        except Exception:
            return False

    async def _forward_to_guis(self, data: bytes):
        if not self.gui_writers:
            return

        dead = []
        for w in list(self.gui_writers):
            try:
                w.write(data)
                if not await self._safe_drain(w):
                    dead.append(w)
            except Exception:
                dead.append(w)

        for w in dead:
            self.gui_writers.discard(w)
            self._close_writer(w)

    async def _forward_to_device(self, data: bytes):
        if self.device_writer is None:
            return

        try:
            self.device_writer.write(data)
            if not await self._safe_drain(self.device_writer):
                self._close_writer(self.device_writer)
                self.device_writer = None
        except Exception:
            self._close_writer(self.device_writer)
            self.device_writer = None

    async def _handle_device(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        if self.device_writer is not None:
            self._post_log(f"新设备接入，关闭旧设备: old={self._peer(self.device_writer)}, new={self._peer(writer)}")
            self._close_writer(self.device_writer)

        self.device_writer = writer
        total = 0
        last_log = time.time()
        self._post_log(f"设备已连接: {self._peer(writer)}")

        try:
            while not self.stop_event.is_set():
                data = await reader.read(65536)
                if not data:
                    break

                total += len(data)
                await self._forward_to_guis(data)

                now = time.time()
                if now - last_log >= 5.0:
                    rate = total / max(now - last_log, 0.001)
                    self._post_log(f"设备->上位机 当前5秒平均约 {rate/1024/1024:.3f} MB/s")
                    total = 0
                    last_log = now

        except asyncio.CancelledError:
            raise
        except Exception as e:
            self._post_log(f"设备连接异常: {e}")
        finally:
            if self.device_writer is writer:
                self.device_writer = None
            self._close_writer(writer)
            self._post_log(f"设备已断开: {self._peer(writer)}")

    async def _handle_gui(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.gui_writers.add(writer)
        self._post_log(f"上位机已连接: {self._peer(writer)}, 当前上位机数量={len(self.gui_writers)}")

        try:
            while not self.stop_event.is_set():
                data = await reader.read(65536)
                if not data:
                    break

                if self.device_writer is None:
                    self._post_log(f"收到上位机数据但设备未连接，丢弃 {len(data)} 字节")
                    continue

                await self._forward_to_device(data)

        except asyncio.CancelledError:
            raise
        except Exception as e:
            self._post_log(f"上位机连接异常: {e}")
        finally:
            self.gui_writers.discard(writer)
            self._close_writer(writer)
            self._post_log(f"上位机已断开: {self._peer(writer)}, 当前上位机数量={len(self.gui_writers)}")

    async def _run_async(self):
        self.loop = asyncio.get_running_loop()
        try:
            self.dev_server = await asyncio.start_server(
                self._handle_device,
                self.host,
                self.device_port,
                limit=1024 * 1024,
            )
            self.gui_server = await asyncio.start_server(
                self._handle_gui,
                self.host,
                self.gui_port,
                limit=1024 * 1024,
            )

            self.started_event.set()
            self._post_log("Bridge started")
            self._post_log(f"Device port: {self.host}:{self.device_port}")
            self._post_log(f"GUI port   : {self.host}:{self.gui_port}")

            async with self.dev_server, self.gui_server:
                while not self.stop_event.is_set():
                    await asyncio.sleep(0.2)

        except OSError as e:
            self.failed_reason = str(e)
            self.failed_event.set()
            self._post_log(f"启动失败：端口可能被占用或被权限/防火墙限制：{e}")
        except Exception as e:
            self.failed_reason = str(e)
            self.failed_event.set()
            self._post_log(f"服务异常退出：{e}")
        finally:
            self._close_writer(self.device_writer)
            self.device_writer = None
            for w in list(self.gui_writers):
                self._close_writer(w)
            self.gui_writers.clear()

            if self.dev_server is not None:
                self.dev_server.close()
                try:
                    await self.dev_server.wait_closed()
                except Exception:
                    pass
            if self.gui_server is not None:
                self.gui_server.close()
                try:
                    await self.gui_server.wait_closed()
                except Exception:
                    pass

            self.stopped_event.set()
            self._post_log("Bridge stopped")

    def run(self):
        asyncio.run(self._run_async())

    def stop(self):
        """从 GUI/Tk 主线程调用，要求 Bridge 尽快释放 9000/9001。"""
        self.stop_event.set()
        loop = self.loop
        if loop is not None:
            try:
                loop.call_soon_threadsafe(self._close_all_in_loop)
            except Exception:
                pass

# ==============================
# TCP 后台线程
# ==============================
class TcpAdcWorker(threading.Thread):
    def __init__(self, event_queue: queue.Queue):
        super().__init__(daemon=True)

        self.cmd_queue = queue.Queue()
        self.event_queue = event_queue
        self.stop_event = threading.Event()

        self.sock = None
        self.rxbuf = bytearray()
        self.tx_seq = 0
        self.expected_board_frame_seq = None

        self.connected = False
        self.streaming = False

        self.save_file = None          # raw/bin 文件句柄
        self.save_wav = None           # wav.Wave_write 句柄
        self.saving_enabled = False
        self.save_format = "raw"
        self.save_base_path = ""       # 用户选择的基础路径
        self.save_path = ""            # 当前正在写入的分包路径
        self.save_fs = 0
        self.save_bits = 0
        self.save_bps = 0
        self.save_range = 0
        self.save_samples = 0          # 本次采集总保存样点
        self.save_bytes = 0            # 本次采集总保存字节
        self.save_frames = 0           # 本次采集总保存DATA帧
        self.save_part_index = 0
        self.save_session_start_ts = 0.0
        self.save_part_start_ts = 0.0
        self.save_part_samples = 0
        self.save_part_bytes = 0
        self.save_part_frames = 0
        self.save_split_seconds = SAVE_SPLIT_SECONDS
        self.close_file_after_stop = False
        self.last_data_time = 0.0

        self.total_samples = 0
        self.total_bytes = 0
        self.last_rate_time = time.time()
        self.last_rate_samples = 0
        self.instant_rate = 0.0

        self.expected_sample_seq = None
        self.sample_gap_events = 0
        self.missing_samples = 0
        self.frame_seq_gap_events = 0
        self.crc_errors = 0
        self.bad_frames = 0
        self.board_overrun = 0
        self.last_ack = None
        self.last_error = ""

        self.current_fs = 0
        self.current_bits = 0
        self.current_range = 0
        self.current_bps = 0

        self.plot_max_points = 12000
        self.plot_raw = deque(maxlen=self.plot_max_points)
        self.plot_volt = deque(maxlen=self.plot_max_points)
        self.plot_volt_zero = deque(maxlen=self.plot_max_points)

        # 时域显示为了不卡顿可以抽点，但 FFT 必须使用未抽点数据。
        # 否则 400kS/s 抽点到约 30kS/s 后，频谱只能显示到约 15kHz。
        self.fft_max_points = 32768
        self.fft_volt_zero = deque(maxlen=self.fft_max_points)
        self.fft_effective_fs = 0.0

        self.plot_decim = 1
        self.plot_effective_fs = 0.0

        # 自动归零：用慢速滑动平均估计直流偏置，再从显示波形中扣除。
        # 注意：这会去掉慢变直流分量；如果要测真正 DC 电压，应关闭自动归零。
        self.auto_zero_enabled = True
        self.zero_tau_s = 2.0
        self.zero_offset_v = 0.0
        self.zero_offset_valid = False
        self.save_zeroed_wav = True

        self.last_ui_emit = time.time()
        self.last_periodic_get = time.time()

        # 默认关闭采集中自动 GET_PARAM
        self.periodic_get_enabled = False
        self.periodic_get_interval_s = 1.0

        self.last_gap_log_time = 0.0
        self.last_ack_log_time = 0.0

        # 电压换算满量程，单位 V，表示 ADC 满量程 ±fs_voltage
        self.fs_voltage = 4.096

        # DATA帧调试
        self.data_debug_enable = True
        self.data_debug_max_frames = 8
        self.data_debug_count = 0

    def command(self, name: str, **kwargs):
        self.cmd_queue.put((name, kwargs))

    def run(self):
        while not self.stop_event.is_set():
            self._process_commands()
            self._recv_drain()
            self._periodic_get_if_needed()
            self._close_file_if_stop_idle()
            self._emit_ui_periodic()

            if self.sock is None:
                time.sleep(0.01)
            else:
                time.sleep(0.0003)

        self._close_socket()
        self._close_save_file()

    def _post(self, etype: str, data=None):
        self.event_queue.put((etype, data))

    def _log(self, msg: str):
        now = time.strftime("%H:%M:%S")
        self._post("log", f"[{now}] {msg}")

    def _process_commands(self):
        while True:
            try:
                name, kwargs = self.cmd_queue.get_nowait()
            except queue.Empty:
                return

            try:
                if name == "connect":
                    self._cmd_connect(kwargs["ip"], int(kwargs["port"]))
                elif name == "disconnect":
                    self._cmd_disconnect()
                elif name == "get":
                    self._send_cmd(TYPE_GET_PARAM)
                    self._log("发送 GET_PARAM")
                elif name == "get_net":
                    self._send_cmd(TYPE_GET_NET)
                    self._log("发送 GET_NET")
                elif name == "set_net":
                    payload = build_set_net_payload(
                        kwargs["server_ip"],
                        int(kwargs["server_port"]),
                        int(kwargs["device_id"]),
                        bool(kwargs.get("save", True)),
                        bool(kwargs.get("reconnect", True)),
                    )
                    self._send_cmd(TYPE_SET_NET, payload)
                    self._log(
                        f"发送 SET_NET: server={kwargs['server_ip']}:{int(kwargs['server_port'])}, "
                        f"device_id={int(kwargs['device_id'])}, "
                        f"save={bool(kwargs.get('save', True))}, reconnect={bool(kwargs.get('reconnect', True))}"
                    )
                elif name == "set":
                    fs = int(kwargs["fs"])
                    bits = int(kwargs["bits"])
                    rng = int(kwargs["range"])
                    payload = struct.pack("<IBB2x", fs, bits, rng)
                    self._send_cmd(TYPE_SET_PARAM, payload)
                    self._log(f"发送 SET_PARAM: fs={fs}, bits={bits}, gain={rng}({range_to_gain_db(rng)}dB, x{range_to_gain_linear(rng):g})")
                elif name == "start":
                    self.periodic_get_enabled = bool(kwargs.get("periodic_get", False))
                    self.current_range = int(kwargs.get("range", self.current_range))
                    self.fs_voltage = float(kwargs.get("fs_voltage", 4.096))
                    self.auto_zero_enabled = bool(kwargs.get("auto_zero_enabled", True))
                    self.zero_tau_s = float(kwargs.get("zero_tau_s", 2.0))
                    if self.zero_tau_s < 0.02:
                        self.zero_tau_s = 0.02
                    if self.zero_tau_s > 300.0:
                        self.zero_tau_s = 300.0
                    self.save_zeroed_wav = bool(kwargs.get("save_zeroed_wav", True))
                    self.zero_offset_v = 0.0
                    self.zero_offset_valid = False

                    self.data_debug_enable = bool(kwargs.get("data_debug_enable", True))
                    self.data_debug_max_frames = int(kwargs.get("data_debug_max_frames", 8))
                    self.data_debug_count = 0

                    self.plot_max_points = int(kwargs.get("plot_points", 12000))
                    self.plot_raw = deque(maxlen=self.plot_max_points)
                    self.plot_volt = deque(maxlen=self.plot_max_points)
                    self.plot_volt_zero = deque(maxlen=self.plot_max_points)

                    # FFT 固定保留最近一段未抽点的归零数据，频谱横轴使用真实采样率 fs。
                    self.fft_max_points = int(kwargs.get("fft_points", 32768))
                    if self.fft_max_points < 4096:
                        self.fft_max_points = 4096
                    if self.fft_max_points > 131072:
                        self.fft_max_points = 131072
                    self.fft_volt_zero = deque(maxlen=self.fft_max_points)
                    self.fft_effective_fs = 0.0

                    self.plot_decim = 1
                    self.plot_effective_fs = 0.0
                    self._reset_stream_counters(clear_plot=True)

                    save_enabled = bool(kwargs.get("save_enabled", False))
                    save_path = kwargs.get("save_path", "")
                    save_format = kwargs.get("save_format", "raw")
                    if save_enabled:
                        self._open_save_file(save_path, save_format)

                    self._send_cmd(TYPE_START)
                    self._log(
                        f"发送 START，采集中自动GET={'打开' if self.periodic_get_enabled else '关闭'}，"
                        f"ADC满量程=±{self.fs_voltage:g}V，"
                        f"增益={self.current_range}({range_to_gain_db(self.current_range)}dB,x{range_to_gain_linear(self.current_range):g})，"
                        f"输入端满量程≈±{self.fs_voltage / range_to_gain_linear(self.current_range):g}V，"
                        f"自动归零={'打开' if self.auto_zero_enabled else '关闭'}，"
                        f"归零时间常数={self.zero_tau_s:g}s，"
                        f"WAV保存={'归零后' if self.save_zeroed_wav else '原始'}，"
                        f"保存={'打开/' + self.save_format if self.saving_enabled else '关闭'}"
                    )
                elif name == "stop":
                    self._send_cmd(TYPE_STOP)
                    self.close_file_after_stop = True
                    self._log("发送 STOP")
                elif name == "clear_stats":
                    self._reset_stream_counters(clear_plot=True)
                elif name == "set_plot_points":
                    self.plot_max_points = int(kwargs.get("plot_points", 12000))
                    old_raw = list(self.plot_raw)[-self.plot_max_points:]
                    old_volt = list(self.plot_volt)[-self.plot_max_points:]
                    old_volt_zero = list(self.plot_volt_zero)[-self.plot_max_points:]
                    self.plot_raw = deque(old_raw, maxlen=self.plot_max_points)
                    self.plot_volt = deque(old_volt, maxlen=self.plot_max_points)
                    self.plot_volt_zero = deque(old_volt_zero, maxlen=self.plot_max_points)
                elif name == "shutdown":
                    self.stop_event.set()
            except Exception as e:
                self.last_error = str(e)
                self._log(f"命令 {name} 失败: {e}")
                self._post("error", str(e))

    def _cmd_connect(self, ip: str, port: int):
        self._close_socket()
        self.rxbuf.clear()
        self.expected_board_frame_seq = None

        sock = socket.create_connection((ip, port), timeout=5.0)
        sock.settimeout(0.0)

        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        except Exception:
            pass
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)
        except Exception:
            pass
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except Exception:
            pass

        self.sock = sock
        self.connected = True
        self.streaming = False
        self._log(f"已连接 {ip}:{port}")
        self._post("connected", True)

    def _cmd_disconnect(self):
        self._close_socket()
        self._close_save_file()
        self._log("已断开连接")
        self._post("connected", False)

    def _close_socket(self):
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            try:
                self.sock.close()
            except Exception:
                pass
        self.sock = None
        self.connected = False
        self.streaming = False
        self.rxbuf.clear()

    def _open_save_file(self, path: str, fmt: str = "raw"):
        """
        开启本次保存会话。
        实际文件不会直接使用用户输入的文件名，而是自动加上电脑本地时间：
            基础名_YYYYMMDD_HH.MM.ext
        并且每 1 小时自动切到下一个分包。
        """
        fmt = (fmt or "raw").lower()
        if fmt not in VALID_SAVE_FORMATS:
            fmt = "raw"

        self._close_save_file()

        self.save_format = fmt
        self.save_base_path = normalize_save_path(path, fmt)
        self.save_path = ""
        self.save_fs = 0
        self.save_bits = 0
        self.save_bps = 0
        self.save_range = 0
        self.save_samples = 0
        self.save_bytes = 0
        self.save_frames = 0
        self.save_part_index = 0
        self.save_session_start_ts = time.time()
        self.save_part_start_ts = 0.0
        self.save_part_samples = 0
        self.save_part_bytes = 0
        self.save_part_frames = 0
        self.saving_enabled = True
        self.close_file_after_stop = False

        # raw/bin 可以立即创建第一包；wav 需要等第一帧 DATA 到来后知道 fs/bps 再创建。
        self._open_new_save_part(part_start_ts=self.save_session_start_ts)
        self._log(
            f"保存会话已打开: fmt={self.save_format}, 每{int(self.save_split_seconds)}秒分包, "
            f"基础路径={self.save_base_path}"
        )

    def _save_active(self) -> bool:
        return self.saving_enabled

    def _open_new_save_part(self,
                            part_start_ts=None,
                            fs: int = 0,
                            bits: int = 0,
                            bps: int = 0,
                            rng: int = 0):
        """
        打开一个新的 1 小时分包。
        raw/bin 立即打开文件；wav 先生成路径，等第一帧 DATA 到来后再 wave.open。
        """
        if not self.saving_enabled:
            return

        if part_start_ts is None:
            part_start_ts = time.time()

        # 确保上一包句柄已关闭，但不要清空总统计。
        self._close_current_save_part(log_close=True)

        self.save_part_start_ts = float(part_start_ts)
        self.save_part_samples = 0
        self.save_part_bytes = 0
        self.save_part_frames = 0
        self.save_path = make_split_save_path(self.save_base_path, self.save_format, self.save_part_start_ts, self.save_part_index)

        folder = os.path.dirname(self.save_path)
        if folder:
            os.makedirs(folder, exist_ok=True)

        if self.save_format == "raw":
            self.save_file = open(self.save_path, "wb", buffering=1024 * 1024)
            self._log(f"保存分包#{self.save_part_index + 1} 已打开: RAW -> {self.save_path}")

        elif self.save_format == "bin":
            self.save_file = open(self.save_path, "wb", buffering=1024 * 1024)
            ts_us = int(self.save_part_start_ts * 1000000)
            # 文件头 64 字节：magic/version/header_len/start_ts_us/part_index/split_seconds
            header = struct.pack(
                "<8sIIQII",
                b"ADCBIN1\0",
                1,
                64,
                ts_us,
                int(self.save_part_index),
                int(self.save_split_seconds),
            )
            header += bytes(64 - len(header))
            self.save_file.write(header)
            self.save_bytes += len(header)
            self.save_part_bytes += len(header)
            self._log(f"保存分包#{self.save_part_index + 1} 已打开: BIN -> {self.save_path}")

        elif self.save_format == "wav":
            self.save_file = None
            self.save_wav = None
            self.save_fs = int(fs) if fs else 0
            self.save_bits = int(bits) if bits else 0
            self.save_bps = int(bps) if bps else 0
            self.save_range = int(rng) if rng else 0
            self._log(f"保存分包#{self.save_part_index + 1} 已准备: WAV -> {self.save_path}")

    def _rotate_save_part_if_needed(self, fs: int, bits: int, bps: int, rng: int):
        if not self.saving_enabled:
            return

        now = time.time()

        if self.save_part_start_ts <= 0.0 or not self.save_path:
            self._open_new_save_part(part_start_ts=now, fs=fs, bits=bits, bps=bps, rng=rng)
            return

        if now - self.save_part_start_ts >= self.save_split_seconds:
            old_start = format_hhmm(self.save_part_start_ts)
            self.save_part_index += 1
            self._log(f"保存达到1小时，切换分包: 上一包起始={old_start}, 新包起始={format_hhmm(now)}")
            self._open_new_save_part(part_start_ts=now, fs=fs, bits=bits, bps=bps, rng=rng)

    def _ensure_wav_open(self, fs: int, bits: int, bps: int, rng: int):
        if self.save_wav is not None:
            return

        if not self.save_path:
            self._open_new_save_part(part_start_ts=time.time(), fs=fs, bits=bits, bps=bps, rng=rng)

        sampwidth = 3 if bps == 3 else 2
        self.save_wav = wave.open(self.save_path, "wb")
        self.save_wav.setnchannels(1)
        self.save_wav.setsampwidth(sampwidth)
        self.save_wav.setframerate(int(fs))
        self.save_fs = int(fs)
        self.save_bits = int(bits)
        self.save_bps = int(bps)
        self.save_range = int(rng)
        self._log(
            f"WAV 分包#{self.save_part_index + 1} 文件已创建: "
            f"fs={fs}, bits={bits}, bps={bps}, ch=1 -> {self.save_path}"
        )

    def _write_save_frame(self,
                          tcp_frame_seq: int,
                          fs: int,
                          bits: int,
                          rng: int,
                          bps: int,
                          first_sample_seq: int,
                          sample_count: int,
                          raw: bytes,
                          raw_codes: np.ndarray,
                          wav_codes=None):
        if not self.saving_enabled:
            return

        try:
            self._rotate_save_part_if_needed(fs=fs, bits=bits, bps=bps, rng=rng)

            if self.save_format == "raw":
                if self.save_file is None:
                    self._open_new_save_part(part_start_ts=time.time(), fs=fs, bits=bits, bps=bps, rng=rng)
                self.save_file.write(raw)
                written = len(raw)
                self.save_bytes += written
                self.save_part_bytes += written

            elif self.save_format == "bin":
                if self.save_file is None:
                    self._open_new_save_part(part_start_ts=time.time(), fs=fs, bits=bits, bps=bps, rng=rng)
                ts_us = int(time.time() * 1000000)
                record_header_len = struct.calcsize("<4sIIBBBBIIHHQ")
                record_len = record_header_len + len(raw)
                rec = struct.pack(
                    "<4sIIBBBBIIHHQ",
                    b"FRM1",
                    record_len,
                    int(fs),
                    int(bits) & 0xFF,
                    int(rng) & 0xFF,
                    int(bps) & 0xFF,
                    0,
                    int(tcp_frame_seq) & 0xFFFFFFFF,
                    int(first_sample_seq) & 0xFFFFFFFF,
                    int(sample_count) & 0xFFFF,
                    len(raw) & 0xFFFF,
                    ts_us,
                )
                self.save_file.write(rec)
                self.save_file.write(raw)
                written = len(rec) + len(raw)
                self.save_bytes += written
                self.save_part_bytes += written

            elif self.save_format == "wav":
                if bps not in (2, 3):
                    raise RuntimeError(f"invalid bps for wav: {bps}")
                if self.save_wav is None:
                    self._ensure_wav_open(fs, bits, bps, rng)
                else:
                    # WAV 单个文件要求采样率/采样宽度固定。
                    if int(fs) != self.save_fs or int(bps) != self.save_bps:
                        raise RuntimeError(
                            f"WAV save parameter changed: old fs={self.save_fs}, bps={self.save_bps}; "
                            f"new fs={fs}, bps={bps}"
                        )
                codes_for_wav = wav_codes if wav_codes is not None and wav_codes.size else raw_codes
                wav_bytes = adc_codes_to_wav_pcm_bytes(codes_for_wav, bps)
                self.save_wav.writeframesraw(wav_bytes)
                written = len(wav_bytes)
                self.save_bytes += written
                self.save_part_bytes += written

            self.save_samples += int(sample_count)
            self.save_frames += 1
            self.save_part_samples += int(sample_count)
            self.save_part_frames += 1

        except Exception as e:
            self._log(f"保存文件失败，已停止保存: {e}")
            self._close_save_file()

    def _close_current_save_part(self, log_close: bool = False):
        """
        只关闭当前分包，不结束整个保存会话。
        """
        had_file = self.save_file is not None or self.save_wav is not None
        path = self.save_path
        try:
            if self.save_file is not None:
                self.save_file.flush()
                self.save_file.close()
            if self.save_wav is not None:
                self.save_wav.close()
            if log_close and had_file and path:
                self._log(
                    f"保存分包已关闭: part={self.save_part_index + 1}, "
                    f"start={format_hhmm(self.save_part_start_ts)}, "
                    f"samples={self.save_part_samples}, frames={self.save_part_frames}, "
                    f"bytes={self.save_part_bytes}, path={path}"
                )
        except Exception as e:
            if log_close:
                self._log(f"关闭保存分包异常: {e}")

        self.save_file = None
        self.save_wav = None
        self.save_fs = 0
        self.save_bits = 0
        self.save_bps = 0
        self.save_range = 0

    def _close_save_file(self):
        was_saving = self.saving_enabled or self.save_file is not None or self.save_wav is not None
        total_samples = self.save_samples
        total_frames = self.save_frames
        total_bytes = self.save_bytes
        base_path = self.save_base_path

        self._close_current_save_part(log_close=was_saving)

        if was_saving:
            self._log(
                f"保存会话已关闭: fmt={self.save_format}, total_samples={total_samples}, "
                f"total_frames={total_frames}, total_bytes={total_bytes}, base={base_path}"
            )

        self.save_file = None
        self.save_wav = None
        self.saving_enabled = False
        self.save_base_path = ""
        self.save_path = ""
        self.save_fs = 0
        self.save_bits = 0
        self.save_bps = 0
        self.save_range = 0
        self.save_samples = 0
        self.save_bytes = 0
        self.save_frames = 0
        self.save_part_index = 0
        self.save_session_start_ts = 0.0
        self.save_part_start_ts = 0.0
        self.save_part_samples = 0
        self.save_part_bytes = 0
        self.save_part_frames = 0
        self.close_file_after_stop = False

    def _close_file_if_stop_idle(self):
        if self.close_file_after_stop and self._save_active():
            if time.time() - self.last_data_time > 0.8:
                self._close_save_file()

    def _send_cmd(self, ftype: int, payload: bytes = b""):
        if self.sock is None:
            raise ConnectionError("尚未连接板子")
        frame = build_frame(ftype, self.tx_seq, payload)
        self.tx_seq = (self.tx_seq + 1) & 0xFFFFFFFF
        self.sock.sendall(frame)

    def _recv_drain(self):
        if self.sock is None:
            return

        got_any = False
        for _ in range(96):
            try:
                data = self.sock.recv(65536)
                if not data:
                    self._log("连接已被板子关闭")
                    self._close_socket()
                    self._post("connected", False)
                    return
                self.rxbuf.extend(data)
                got_any = True
            except BlockingIOError:
                break
            except socket.timeout:
                break
            except Exception as e:
                self.last_error = str(e)
                self._log(f"接收错误: {e}")
                self._close_socket()
                self._post("connected", False)
                return

        if got_any:
            self._parse_rxbuf()

    def _parse_rxbuf(self):
        while True:
            idx = self.rxbuf.find(MAGIC_BYTES)
            if idx < 0:
                if len(self.rxbuf) > 1:
                    del self.rxbuf[:-1]
                return
            if idx > 0:
                del self.rxbuf[:idx]

            if len(self.rxbuf) < FRAME_HDR_LEN:
                return

            try:
                magic, ver, ftype, length, seq = struct.unpack_from("<HBBHI", self.rxbuf, 0)
            except struct.error:
                return

            if magic != MAGIC or ver != VER or length > FRAME_MAX_PAYLOAD:
                self.bad_frames += 1
                del self.rxbuf[0]
                continue

            total = FRAME_HDR_LEN + length + FRAME_CRC_LEN
            if len(self.rxbuf) < total:
                return

            frame_without_crc = bytes(self.rxbuf[:FRAME_HDR_LEN + length])
            rx_crc = struct.unpack_from("<H", self.rxbuf, FRAME_HDR_LEN + length)[0]
            calc_crc = crc16_modbus(frame_without_crc)
            if rx_crc != calc_crc:
                self.crc_errors += 1
                self.bad_frames += 1
                del self.rxbuf[0]
                continue

            payload = bytes(self.rxbuf[FRAME_HDR_LEN:FRAME_HDR_LEN + length])
            del self.rxbuf[:total]

            self._check_board_frame_seq(seq)
            self._handle_frame(ftype, seq, payload)

    def _check_board_frame_seq(self, seq: int):
        if self.expected_board_frame_seq is None:
            self.expected_board_frame_seq = (seq + 1) & 0xFFFFFFFF
            return

        if seq != self.expected_board_frame_seq:
            self.frame_seq_gap_events += 1
            now = time.time()
            if now - self.last_gap_log_time > 1.0:
                self._log(f"警告：板子TCP应用帧序号不连续，expect={self.expected_board_frame_seq}, got={seq}")
                self.last_gap_log_time = now

        self.expected_board_frame_seq = (seq + 1) & 0xFFFFFFFF

    def _handle_frame(self, ftype: int, seq: int, payload: bytes):
        if ftype == TYPE_ACK:
            try:
                ack = parse_ack(payload)
            except Exception as e:
                self.bad_frames += 1
                self._log(f"ACK 解析失败: {e}")
                return

            self.last_ack = ack
            self.board_overrun = ack["overrun"]
            self.current_fs = ack["fs"]
            self.current_bits = ack["bits"]
            self.current_range = ack["range"]
            self.streaming = bool(ack["running"])

            now = time.time()
            force_log = (ack["status"] != 0) or (now - self.last_ack_log_time > 0.8)
            if force_log:
                if ack["status"] != 0:
                    self._log(
                        f"ACK ERROR: status={ack['status']}, fs={ack['fs']}, bits={ack['bits']}, "
                        f"gain={ack['range']}, running={ack['running']}, overrun={ack['overrun']}, "
                        f"sample_seq={ack['sample_seq']}, flags=0x{ack['flags']:08X}"
                    )
                else:
                    self._log(
                        f"ACK OK: fs={ack['fs']}, bits={ack['bits']}, gain={ack['range']}, "
                        f"running={ack['running']}, overrun={ack['overrun']}, "
                        f"sample_seq={ack['sample_seq']}, flags=0x{ack['flags']:08X}"
                    )
                self.last_ack_log_time = now

            self._post("ack", ack)
            return

        if ftype == TYPE_NET_ACK:
            try:
                net_ack = parse_net_ack(payload)
            except Exception as e:
                self.bad_frames += 1
                self._log(f"NET_ACK 解析失败: {e}")
                return
            self._log(
                f"NET_ACK: status={net_ack['status']}, "
                f"server={net_ack['server_ip']}:{net_ack['server_port']}, "
                f"device_id={net_ack['device_id']}, connected={net_ack['connected']}, "
                f"reconnect_pending={net_ack['reconnect_pending']}, "
                f"local={net_ack.get('local_ip') or '-'}, gw={net_ack.get('gateway') or '-'}"
            )
            self._post("net_ack", net_ack)
            return

        if ftype == TYPE_REGISTER:
            # 服务器透明转发时可能看到设备注册帧，这不是错误。
            dev = payload[0] if len(payload) >= 1 else 0
            fs = struct.unpack_from("<I", payload, 4)[0] if len(payload) >= 8 else 0
            bits = payload[8] if len(payload) >= 9 else 0
            rng = payload[9] if len(payload) >= 10 else 0
            self._log(f"收到设备REGISTER: device_id={dev}, fs={fs}, bits={bits}, gain={rng}")
            return

        if ftype == TYPE_DATA:
            self._handle_data(seq, payload)
            return

        self.bad_frames += 1
        self._log(f"未知帧类型: 0x{ftype:02X}, seq={seq}, len={len(payload)}")

    def _handle_data(self, tcp_frame_seq: int, payload: bytes):
        """
        DATA payload 格式按此前下位机程序：
          uint32 fs
          uint8  bits
          uint8  range
          uint8  bps
          uint8  reserved
          uint32 first_sample_seq
          uint16 sample_count
          uint16 data_bytes
          uint8  raw[data_bytes]
        """
        if len(payload) < 16:
            self.bad_frames += 1
            self._log(f"DATA payload too short: len={len(payload)}")
            return

        fs = struct.unpack_from("<I", payload, 0)[0]
        bits = payload[4]
        rng = payload[5]
        bps = payload[6]
        first_sample_seq = struct.unpack_from("<I", payload, 8)[0]
        sample_count, data_bytes = struct.unpack_from("<HH", payload, 12)

        raw = payload[16:16 + data_bytes]

        # 严格按帧头校验，避免静默错位
        if bps not in (2, 3):
            self.bad_frames += 1
            self._log(f"DATA bps invalid: bps={bps}, bits={bits}, payload_len={len(payload)}")
            return

        if len(raw) != data_bytes:
            self.bad_frames += 1
            self._log(f"DATA len mismatch: len(raw)={len(raw)}, data_bytes={data_bytes}")
            return

        if sample_count * bps != data_bytes:
            self.bad_frames += 1
            self._log(f"DATA count mismatch: samples={sample_count}, bps={bps}, data_bytes={data_bytes}")
            return

        # 以 DATA 帧头为准更新状态
        self.last_data_time = time.time()
        self.streaming = True
        self.current_fs = fs
        self.current_bits = bits
        self.current_range = rng
        self.current_bps = bps

        # 解析 raw code 和 voltage
        # 注意：协议字段仍叫 range，但本版含义为增益档位。
        # 上位机显示的是“输入端折算电压”：V_input = V_adc_diff / gain。
        raw_codes = raw_adc_to_int(raw, bps)
        gain_linear = range_to_gain_linear(rng)
        input_fs_voltage = self.fs_voltage / gain_linear
        voltages = raw_to_voltage(raw_codes, bits, bps, input_fs_voltage)

        # 自动归零：得到扣除直流偏置后的显示/分析波形，使其围绕 0V 上下浮动。
        voltages_zero = self._apply_auto_zero_voltage(voltages, fs, sample_count)
        wav_codes = None
        if self.auto_zero_enabled and self.save_zeroed_wav:
            wav_codes = voltage_to_raw_codes(voltages_zero, bits, bps, input_fs_voltage)

        # 开始采集后打印前 N 个 DATA 帧，专门排查 16/24位、STATUS混入、字节序问题
        if self.data_debug_enable and self.data_debug_count < self.data_debug_max_frames:
            self.data_debug_count += 1
            raw10 = raw_codes[:10].tolist()
            v10 = [round(float(x), 6) for x in voltages[:10]]
            self._log(
                f"DATA DEBUG #{self.data_debug_count}: tcp_seq={tcp_frame_seq}, fs={fs}, bits={bits}, "
                f"bps={bps}, first_seq={first_sample_seq}, samples={sample_count}, "
                f"data_bytes={data_bytes}, payload_raw16={hex_preview(raw, 16)}"
            )
            z10 = [round(float(x), 6) for x in voltages_zero[:10]]
            self._log(f"DATA DEBUG raw[0:10]={raw10}")
            self._log(f"DATA DEBUG input_volt[0:10]={v10}")
            self._log(f"DATA DEBUG zeroed_input_volt[0:10]={z10}, offset={self.zero_offset_v:.9f}V")

        # 采样序号连续性检查
        if self.expected_sample_seq is None:
            self.expected_sample_seq = (first_sample_seq + sample_count) & 0xFFFFFFFF
        else:
            if first_sample_seq != self.expected_sample_seq:
                gap = (first_sample_seq - self.expected_sample_seq) & 0xFFFFFFFF
                if gap < 0x7FFFFFFF:
                    self.missing_samples += gap
                self.sample_gap_events += 1

                now = time.time()
                if now - self.last_gap_log_time > 1.0:
                    self._log(f"警告：采样序号不连续，expect={self.expected_sample_seq}, got={first_sample_seq}, gap={gap}")
                    self.last_gap_log_time = now

                self.expected_sample_seq = first_sample_seq
            self.expected_sample_seq = (self.expected_sample_seq + sample_count) & 0xFFFFFFFF

        # 本地保存：raw/wav/bin
        # raw 保存下位机原始ADC字节；wav 保存标准PCM；bin 保存带帧头和采样序号的记录。
        self._write_save_frame(
            tcp_frame_seq=tcp_frame_seq,
            fs=fs,
            bits=bits,
            rng=rng,
            bps=bps,
            first_sample_seq=first_sample_seq,
            sample_count=sample_count,
            raw=raw,
            raw_codes=raw_codes,
            wav_codes=wav_codes,
        )

        self.total_samples += sample_count
        self.total_bytes += data_bytes

        # 图形缓存限速，避免 GUI 太卡：时域图可以抽点显示。
        # 但 FFT 不能使用抽点后的显示数据，否则 400kS/s 会因为抽点只显示到约 15kHz。
        # 所以这里分成两个缓存：
        #   plot_volt_zero：抽点后的归零时域显示缓存；
        #   fft_volt_zero ：未抽点的归零频域计算缓存，横轴上限=真实fs/2。
        decim = max(1, int(fs // 30000)) if fs > 0 else 1
        self.plot_decim = decim
        self.plot_effective_fs = (float(fs) / float(decim)) if fs > 0 else 0.0
        self.fft_effective_fs = float(fs) if fs > 0 else 0.0
        if raw_codes.size:
            self.plot_raw.extend(raw_codes[::decim].tolist())
            self.plot_volt.extend(voltages[::decim].tolist())
            self.plot_volt_zero.extend(voltages_zero[::decim].tolist())
            self.fft_volt_zero.extend(voltages_zero.tolist())

    def _apply_auto_zero_voltage(self, voltages: np.ndarray, fs: int, sample_count: int) -> np.ndarray:
        """
        自动归零核心：
        1. 对每个 DATA 帧求平均值，认为它主要代表当前直流偏置；
        2. 用一阶低通/滑动平均更新 offset；
        3. 返回 voltage - offset，用于下图显示、统计，以及可选 WAV 保存。

        zero_tau_s 越小，归零越快，但越容易把低频真实信号也当作漂移扣掉；
        zero_tau_s 越大，归零越慢，但更不容易削弱低频信号。
        """
        if voltages is None or voltages.size == 0:
            return np.empty(0, dtype=np.float64)

        if not self.auto_zero_enabled:
            self.zero_offset_v = 0.0
            self.zero_offset_valid = False
            return voltages.astype(np.float64, copy=True)

        frame_mean = float(np.mean(voltages))

        if not self.zero_offset_valid:
            self.zero_offset_v = frame_mean
            self.zero_offset_valid = True
        else:
            frame_dt = (float(sample_count) / float(fs)) if fs and sample_count else 0.02
            alpha = 1.0 - float(np.exp(-frame_dt / max(self.zero_tau_s, 0.001)))
            if alpha < 0.0001:
                alpha = 0.0001
            elif alpha > 1.0:
                alpha = 1.0
            self.zero_offset_v += alpha * (frame_mean - self.zero_offset_v)

        return voltages.astype(np.float64, copy=False) - self.zero_offset_v

    def _periodic_get_if_needed(self):
        if not self.periodic_get_enabled:
            return
        if self.sock is None:
            return
        if not self.streaming:
            return

        now = time.time()
        if now - self.last_periodic_get >= self.periodic_get_interval_s:
            self.last_periodic_get = now
            try:
                self._send_cmd(TYPE_GET_PARAM)
            except Exception:
                pass

    def _emit_ui_periodic(self):
        now = time.time()
        if now - self.last_ui_emit < 0.1:
            return

        dt = now - self.last_rate_time
        if dt >= 1.0:
            ds = self.total_samples - self.last_rate_samples
            self.instant_rate = ds / dt
            self.last_rate_time = now
            self.last_rate_samples = self.total_samples

        self.last_ui_emit = now

        raw_arr = np.fromiter(self.plot_raw, dtype=np.int32) if self.plot_raw else np.empty(0, dtype=np.int32)
        volt_arr = np.fromiter(self.plot_volt, dtype=np.float64) if self.plot_volt else np.empty(0, dtype=np.float64)
        volt_zero_arr = np.fromiter(self.plot_volt_zero, dtype=np.float64) if self.plot_volt_zero else np.empty(0, dtype=np.float64)
        fft_zero_arr = np.fromiter(self.fft_volt_zero, dtype=np.float64) if self.fft_volt_zero else np.empty(0, dtype=np.float64)

        st_raw = raw_stats(raw_arr, self.current_bits, self.current_bps)
        st_volt = volt_stats(volt_arr)
        st_volt_zero = volt_stats(volt_zero_arr)

        status = {
            "connected": self.connected,
            "streaming": self.streaming,
            "fs": self.current_fs,
            "bits": self.current_bits,
            "range": self.current_range,
            "bps": self.current_bps,
            "fs_voltage": self.fs_voltage,
            "gain_db": range_to_gain_db(self.current_range),
            "gain_linear": range_to_gain_linear(self.current_range),
            "input_fs_voltage": self.fs_voltage / range_to_gain_linear(self.current_range),
            "auto_zero_enabled": self.auto_zero_enabled,
            "zero_tau_s": self.zero_tau_s,
            "zero_offset_v": self.zero_offset_v if self.zero_offset_valid else 0.0,
            "zero_offset_valid": self.zero_offset_valid,
            "save_zeroed_wav": self.save_zeroed_wav,
            "plot_decim": self.plot_decim,
            "plot_effective_fs": self.plot_effective_fs,
            "fft_effective_fs": self.fft_effective_fs,
            "fft_points": int(fft_zero_arr.size),
            "fft_max_points": int(self.fft_max_points),
            "total_samples": self.total_samples,
            "total_bytes": self.total_bytes,
            "instant_rate": self.instant_rate,
            "sample_gap_events": self.sample_gap_events,
            "missing_samples": self.missing_samples,
            "frame_seq_gap_events": self.frame_seq_gap_events,
            "crc_errors": self.crc_errors,
            "bad_frames": self.bad_frames,
            "board_overrun": self.board_overrun,
            "last_error": self.last_error,
            "saving": self.saving_enabled,
            "save_format": self.save_format,
            "save_path": self.save_path,
            "save_samples": self.save_samples,
            "save_bytes": self.save_bytes,
            "save_frames": self.save_frames,
            "save_part_index": self.save_part_index,
            "save_part_samples": self.save_part_samples,
            "save_part_bytes": self.save_part_bytes,
            "save_part_frames": self.save_part_frames,
            "save_part_start": format_hhmm(self.save_part_start_ts) if self.save_part_start_ts else "-",
            "save_split_seconds": self.save_split_seconds,
            "periodic_get": self.periodic_get_enabled,
            "raw_stats": st_raw,
            "volt_stats": st_volt,
            "volt_zero_stats": st_volt_zero,
        }
        self._post("status", status)

        if volt_zero_arr.size:
            self._post(
                "plot",
                {
                    "zeroed_time": volt_zero_arr,
                    "time_fs": self.plot_effective_fs,
                    "zeroed_fft": fft_zero_arr,
                    "fft_fs": self.fft_effective_fs,
                    "decim": self.plot_decim,
                },
            )

    def _reset_stream_counters(self, clear_plot: bool = True):
        self.total_samples = 0
        self.total_bytes = 0
        self.last_rate_time = time.time()
        self.last_rate_samples = 0
        self.instant_rate = 0.0
        self.expected_sample_seq = None
        self.sample_gap_events = 0
        self.missing_samples = 0
        self.frame_seq_gap_events = 0
        self.crc_errors = 0
        self.bad_frames = 0
        self.board_overrun = 0
        self.last_error = ""
        self.expected_board_frame_seq = None
        self.last_data_time = time.time()
        self.last_gap_log_time = 0.0
        self.data_debug_count = 0
        self.zero_offset_v = 0.0
        self.zero_offset_valid = False
        self.plot_decim = 1
        self.plot_effective_fs = 0.0
        self.fft_effective_fs = 0.0
        if clear_plot:
            self.plot_raw.clear()
            self.plot_volt.clear()
            self.plot_volt_zero.clear()
            self.fft_volt_zero.clear()
        self._post("status", {})


# ==============================
# Tkinter GUI
# ==============================
class AdcGuiApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("STM32 ADS127L11 5G主动连接版上位机 - 内置Bridge服务 + 0/20/40dB增益")
        self.root.geometry("1360x960")

        self.event_queue = queue.Queue()

        # 内置 TCP Bridge 服务稍后在 UI 创建完成后启动。
        # 这样即使 Bridge 启动失败，错误信息也一定能显示到日志区。
        self.bridge = None

        self.worker = TcpAdcWorker(self.event_queue)
        self.worker.start()

        self.ip_var = tk.StringVar(value=DEFAULT_IP)
        self.port_var = tk.StringVar(value=str(DEFAULT_PORT))

        # 这些是写入“下位机”的主动连接目标：服务器公网IP、设备接入端口、设备ID。
        self.device_server_ip_var = tk.StringVar(value=DEFAULT_DEVICE_SERVER_IP)
        self.device_server_port_var = tk.StringVar(value=str(DEFAULT_DEVICE_SERVER_PORT))
        self.device_id_var = tk.StringVar(value=str(DEFAULT_DEVICE_ID))
        self.net_status_var = tk.StringVar(value="设备服务器参数: 未读取")

        self.fs_var = tk.StringVar(value="50000")
        self.bits_var = tk.StringVar(value="24")
        self.range_var = tk.StringVar(value=RANGE_TEXT[0])
        self.plot_points_var = tk.StringVar(value="16000")
        self.save_enable_var = tk.BooleanVar(value=False)
        self.save_format_var = tk.StringVar(value="raw")
        self.save_path_var = tk.StringVar(value=os.path.abspath("adc_capture.raw"))  # 基础路径，实际文件名会自动加 YYYYMMDD_HH.MM

        self.periodic_get_var = tk.BooleanVar(value=False)

        # 电压换算参数：满量程 ±V
        self.fs_voltage_var = tk.StringVar(value="4.096")

        # 自动归零参数
        self.auto_zero_var = tk.BooleanVar(value=True)
        self.zero_tau_var = tk.StringVar(value="2.0")
        self.save_zeroed_wav_var = tk.BooleanVar(value=True)

        # DATA帧调试参数
        self.data_debug_var = tk.BooleanVar(value=True)
        self.data_debug_count_var = tk.StringVar(value="8")

        # 短接判断阈值
        self.zero_threshold_24_var = tk.StringVar(value="50000")
        self.zero_threshold_16_var = tk.StringVar(value="200")
        self.noise_threshold_24_var = tk.StringVar(value="10000")
        self.noise_threshold_16_var = tk.StringVar(value="50")

        self.connected_var = tk.StringVar(value="未连接")
        self.running_var = tk.StringVar(value="未采集")
        self.board_cfg_var = tk.StringVar(value="板子参数: fs=-, bits=-, gain=-")
        self.rate_var = tk.StringVar(value="实时速率: 0 S/s")
        self.samples_var = tk.StringVar(value="样本数: 0")
        self.bytes_var = tk.StringVar(value="字节数: 0")
        self.loss_var = tk.StringVar(value="丢包/丢样: 未检测")
        self.overrun_var = tk.StringVar(value="板端 overrun: 0")
        self.crc_var = tk.StringVar(value="CRC错误: 0 / 坏帧: 0")
        self.save_state_var = tk.StringVar(value="未保存")
        self.periodic_get_state_var = tk.StringVar(value="采集中自动GET: 关闭")
        self.zero_state_var = tk.StringVar(value="自动归零: 打开，等待数据")

        self.raw_stat_var = tk.StringVar(value="raw统计: 等待数据")
        self.volt_stat_var = tk.StringVar(value="原始电压统计: 等待数据")
        self.volt_zero_stat_var = tk.StringVar(value="输入端归零后电压统计: 等待数据")
        self.fft_stat_var = tk.StringVar(value="频域统计: 等待数据")
        self.raw_judge_var = tk.StringVar(value="短接判断: 等待数据")
        self.parse_info_var = tk.StringVar(value="解析依据: 等待 DATA 帧；本版以 DATA 帧头 bits/bps 为准解析。")

        self.latest_status = {}
        self.last_plot_time = 0.0

        self._build_ui()
        self._start_bridge_service()
        self._schedule_poll()
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def _start_bridge_service(self):
        """UI 创建完成后启动内置 Bridge，并把启动状态写入 GUI 日志区。"""
        self.append_log("[SYS] 正在启动内置 Bridge 服务：设备端口 9000，上位机端口 9001")

        self.bridge = BridgeServerThread(
            self.event_queue,
            host="0.0.0.0",
            device_port=DEFAULT_DEVICE_SERVER_PORT,
            gui_port=DEFAULT_PORT,
        )
        self.bridge.start()

        # 1 秒后主动检测本机端口是否已经监听，避免用户看不到 Bridge 日志时无法判断。
        self.root.after(1000, self._check_bridge_ports_once)

    def _check_bridge_ports_once(self):
        def port_open(port: int) -> bool:
            try:
                with socket.create_connection(("127.0.0.1", int(port)), timeout=0.3):
                    return True
            except Exception:
                return False

        dev_ok = port_open(DEFAULT_DEVICE_SERVER_PORT)
        gui_ok = port_open(DEFAULT_PORT)

        if getattr(self.bridge, "started_event", None) is not None and self.bridge.started_event.is_set():
            self.append_log(f"[SYS] 内置 Bridge 启动成功：127.0.0.1:{DEFAULT_DEVICE_SERVER_PORT} 和 127.0.0.1:{DEFAULT_PORT} 已可连接")
        elif getattr(self.bridge, "failed_event", None) is not None and self.bridge.failed_event.is_set():
            self.append_log("[SYS] 内置 Bridge 未启动成功。端口检测结果仅表示有程序占用该端口，不代表是当前GUI内置Bridge。")
            self.append_log(f"[SYS] 端口状态：9000={'OPEN' if dev_ok else 'CLOSED'}, 9001={'OPEN' if gui_ok else 'CLOSED'}；失败原因：{self.bridge.failed_reason}")
        else:
            self.append_log(f"[SYS] Bridge 正在启动或等待系统释放端口：9000={'OPEN' if dev_ok else 'CLOSED'}, 9001={'OPEN' if gui_ok else 'CLOSED'}")

    def _build_ui(self):
        top = ttk.Frame(self.root, padding=8)
        top.pack(side=tk.TOP, fill=tk.X)

        conn = ttk.LabelFrame(top, text="连接")
        conn.pack(side=tk.LEFT, padx=4, fill=tk.Y)
        ttk.Label(conn, text="IP").grid(row=0, column=0, padx=3, pady=3)
        ttk.Entry(conn, textvariable=self.ip_var, width=15).grid(row=0, column=1, padx=3, pady=3)
        ttk.Label(conn, text="端口").grid(row=0, column=2, padx=3, pady=3)
        ttk.Entry(conn, textvariable=self.port_var, width=6).grid(row=0, column=3, padx=3, pady=3)
        ttk.Button(conn, text="连接", command=self.connect).grid(row=1, column=0, columnspan=2, sticky="ew", padx=3, pady=3)
        ttk.Button(conn, text="断开", command=self.disconnect).grid(row=1, column=2, columnspan=2, sticky="ew", padx=3, pady=3)
        ttk.Label(conn, textvariable=self.connected_var).grid(row=2, column=0, columnspan=4, sticky="w", padx=3, pady=3)

        param = ttk.LabelFrame(top, text="参数设置")
        param.pack(side=tk.LEFT, padx=4, fill=tk.Y)
        ttk.Label(param, text="采样率").grid(row=0, column=0, padx=3, pady=3)
        ttk.Combobox(param, textvariable=self.fs_var, width=9, values=[str(x) for x in VALID_FS], state="readonly").grid(row=0, column=1, padx=3, pady=3)
        ttk.Label(param, text="位数").grid(row=0, column=2, padx=3, pady=3)
        ttk.Combobox(param, textvariable=self.bits_var, width=5, values=[str(x) for x in VALID_BITS], state="readonly").grid(row=0, column=3, padx=3, pady=3)
        ttk.Label(param, text="增益档位").grid(row=1, column=0, padx=3, pady=3)
        ttk.Combobox(param, textvariable=self.range_var, width=22, values=[RANGE_TEXT[i] for i in VALID_RANGES], state="readonly").grid(row=1, column=1, columnspan=3, padx=3, pady=3, sticky="ew")
        ttk.Button(param, text="设置并保存到板子Flash", command=self.set_param).grid(row=2, column=0, columnspan=2, sticky="ew", padx=3, pady=3)
        ttk.Button(param, text="读取板子参数", command=self.get_param).grid(row=2, column=2, columnspan=2, sticky="ew", padx=3, pady=3)
        ttk.Label(param, textvariable=self.board_cfg_var).grid(row=3, column=0, columnspan=4, sticky="w", padx=3, pady=3)

        net = ttk.LabelFrame(top, text="下位机主动连接参数")
        net.pack(side=tk.LEFT, padx=4, fill=tk.Y)
        ttk.Label(net, text="服务器公网IP").grid(row=0, column=0, padx=3, pady=3)
        ttk.Entry(net, textvariable=self.device_server_ip_var, width=15).grid(row=0, column=1, padx=3, pady=3)
        ttk.Label(net, text="设备端口").grid(row=0, column=2, padx=3, pady=3)
        ttk.Entry(net, textvariable=self.device_server_port_var, width=6).grid(row=0, column=3, padx=3, pady=3)
        ttk.Label(net, text="设备ID").grid(row=1, column=0, padx=3, pady=3)
        ttk.Entry(net, textvariable=self.device_id_var, width=6).grid(row=1, column=1, padx=3, pady=3)
        ttk.Button(net, text="读取连接参数", command=self.get_net_param).grid(row=1, column=2, sticky="ew", padx=3, pady=3)
        ttk.Button(net, text="写入Flash并重连", command=self.set_net_param).grid(row=1, column=3, sticky="ew", padx=3, pady=3)
        ttk.Label(net, textvariable=self.net_status_var).grid(row=2, column=0, columnspan=4, sticky="w", padx=3, pady=3)

        cap = ttk.LabelFrame(top, text="采集控制")
        cap.pack(side=tk.LEFT, padx=4, fill=tk.Y)
        ttk.Button(cap, text="开始采集", command=self.start_capture).grid(row=0, column=0, padx=3, pady=3, sticky="ew")
        ttk.Button(cap, text="停止采集", command=self.stop_capture).grid(row=0, column=1, padx=3, pady=3, sticky="ew")
        ttk.Button(cap, text="清零统计", command=self.clear_stats).grid(row=0, column=2, padx=3, pady=3, sticky="ew")
        ttk.Checkbutton(cap, text="采集中每秒读取状态（默认关闭）", variable=self.periodic_get_var).grid(row=1, column=0, columnspan=3, sticky="w", padx=3, pady=3)

        ttk.Label(cap, text="显示点数").grid(row=2, column=0, padx=3, pady=3)
        ttk.Entry(cap, textvariable=self.plot_points_var, width=8).grid(row=2, column=1, padx=3, pady=3)
        ttk.Label(cap, text="ADC满量程±V").grid(row=2, column=2, padx=3, pady=3)
        ttk.Entry(cap, textvariable=self.fs_voltage_var, width=8).grid(row=2, column=3, padx=3, pady=3)

        ttk.Checkbutton(cap, text="打印DATA调试", variable=self.data_debug_var).grid(row=3, column=0, sticky="w", padx=3, pady=3)
        ttk.Label(cap, text="帧数").grid(row=3, column=1, padx=3, pady=3)
        ttk.Entry(cap, textvariable=self.data_debug_count_var, width=5).grid(row=3, column=2, padx=3, pady=3)

        ttk.Checkbutton(cap, text="自动归零", variable=self.auto_zero_var).grid(row=4, column=0, sticky="w", padx=3, pady=3)
        ttk.Label(cap, text="归零时间常数(s)").grid(row=4, column=1, padx=3, pady=3)
        ttk.Entry(cap, textvariable=self.zero_tau_var, width=6).grid(row=4, column=2, padx=3, pady=3)
        ttk.Checkbutton(cap, text="WAV保存归零后", variable=self.save_zeroed_wav_var).grid(row=4, column=3, sticky="w", padx=3, pady=3)

        ttk.Label(cap, textvariable=self.zero_state_var).grid(row=5, column=0, columnspan=4, sticky="w", padx=3, pady=3)
        ttk.Label(cap, textvariable=self.running_var).grid(row=6, column=0, columnspan=4, sticky="w", padx=3, pady=3)
        ttk.Label(cap, textvariable=self.periodic_get_state_var).grid(row=7, column=0, columnspan=4, sticky="w", padx=3, pady=3)

        save = ttk.LabelFrame(top, text="保存到本地")
        save.pack(side=tk.LEFT, padx=4, fill=tk.BOTH, expand=True)
        ttk.Checkbutton(save, text="启用保存", variable=self.save_enable_var).grid(row=0, column=0, padx=3, pady=3, sticky="w")
        ttk.Label(save, text="格式").grid(row=0, column=1, padx=3, pady=3, sticky="e")
        ttk.Combobox(save, textvariable=self.save_format_var, width=6, values=VALID_SAVE_FORMATS, state="readonly").grid(row=0, column=2, padx=3, pady=3, sticky="w")
        ttk.Entry(save, textvariable=self.save_path_var, width=34).grid(row=1, column=0, columnspan=3, padx=3, pady=3, sticky="ew")
        ttk.Button(save, text="选择", command=self.choose_save_file).grid(row=1, column=3, padx=3, pady=3)
        ttk.Label(save, text="每1小时自动分包；文件名=基础名_YYYYMMDD_HH.MM.ext；raw=纯ADC字节，wav=PCM，bin=带序号/时间戳").grid(row=2, column=0, columnspan=4, sticky="w", padx=3, pady=3)
        ttk.Label(save, textvariable=self.save_state_var).grid(row=3, column=0, columnspan=4, sticky="w", padx=3, pady=3)
        save.columnconfigure(0, weight=1)

        mid = ttk.Frame(self.root, padding=(8, 0, 8, 4))
        mid.pack(side=tk.TOP, fill=tk.X)
        for i, var in enumerate([
            self.rate_var,
            self.samples_var,
            self.bytes_var,
            self.loss_var,
            self.overrun_var,
            self.crc_var,
        ]):
            lab = ttk.Label(mid, textvariable=var, relief=tk.GROOVE, padding=4)
            lab.grid(row=0, column=i, padx=3, pady=3, sticky="ew")
            mid.columnconfigure(i, weight=1)

        judge = ttk.LabelFrame(self.root, text="电压显示 / 原始码值判断 / 输入短接检查")
        judge.pack(side=tk.TOP, fill=tk.X, padx=8, pady=4)

        ttk.Label(judge, text="24位短接最大|code|阈值").grid(row=0, column=0, padx=3, pady=3, sticky="e")
        ttk.Entry(judge, textvariable=self.zero_threshold_24_var, width=10).grid(row=0, column=1, padx=3, pady=3)
        ttk.Label(judge, text="16位短接最大|code|阈值").grid(row=0, column=2, padx=3, pady=3, sticky="e")
        ttk.Entry(judge, textvariable=self.zero_threshold_16_var, width=10).grid(row=0, column=3, padx=3, pady=3)
        ttk.Label(judge, text="24位std阈值").grid(row=0, column=4, padx=3, pady=3, sticky="e")
        ttk.Entry(judge, textvariable=self.noise_threshold_24_var, width=10).grid(row=0, column=5, padx=3, pady=3)
        ttk.Label(judge, text="16位std阈值").grid(row=0, column=6, padx=3, pady=3, sticky="e")
        ttk.Entry(judge, textvariable=self.noise_threshold_16_var, width=10).grid(row=0, column=7, padx=3, pady=3)

        ttk.Label(judge, textvariable=self.parse_info_var, relief=tk.GROOVE, padding=4).grid(row=1, column=0, columnspan=8, padx=3, pady=3, sticky="ew")
        ttk.Label(judge, textvariable=self.raw_stat_var, relief=tk.GROOVE, padding=4).grid(row=2, column=0, columnspan=8, padx=3, pady=3, sticky="ew")
        ttk.Label(judge, textvariable=self.volt_zero_stat_var, relief=tk.GROOVE, padding=4).grid(row=3, column=0, columnspan=8, padx=3, pady=3, sticky="ew")
        ttk.Label(judge, textvariable=self.fft_stat_var, relief=tk.GROOVE, padding=4).grid(row=4, column=0, columnspan=8, padx=3, pady=3, sticky="ew")
        ttk.Label(judge, textvariable=self.raw_judge_var, relief=tk.GROOVE, padding=4).grid(row=5, column=0, columnspan=8, padx=3, pady=3, sticky="ew")
        judge.columnconfigure(7, weight=1)

        plot_frame = ttk.LabelFrame(self.root, text="实时 ADC：归零后时域图 + 对应频域 FFT 图")
        plot_frame.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=8, pady=4)

        self.fig = Figure(figsize=(8, 5.2), dpi=100)
        self.ax_time = self.fig.add_subplot(211)
        self.ax_time.set_title("Auto-zeroed input voltage waveform")
        self.ax_time.set_ylabel("Voltage (V)")
        self.line_time, = self.ax_time.plot([], [], linewidth=0.8)
        self.ax_time.grid(True)

        self.ax_freq = self.fig.add_subplot(212)
        self.ax_freq.set_title("FFT of auto-zeroed input waveform")
        self.ax_freq.set_xlabel("Frequency (Hz)")  # 后续会根据频率范围自动切换 Hz / kHz / MHz
        self.ax_freq.set_ylabel("Amplitude (Vpk)")
        self.line_freq, = self.ax_freq.plot([], [], linewidth=0.8)
        self.ax_freq.grid(True)
        self.fig.tight_layout()

        self.canvas = FigureCanvasTkAgg(self.fig, master=plot_frame)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        bottom = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        bottom.pack(side=tk.BOTTOM, fill=tk.BOTH, padx=8, pady=4)

        log_frame = ttk.LabelFrame(bottom, text="日志")
        self.log_text = tk.Text(log_frame, height=9, wrap="none")
        yscroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        xscroll = ttk.Scrollbar(log_frame, orient="horizontal", command=self.log_text.xview)
        self.log_text.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        yscroll.pack(side=tk.RIGHT, fill=tk.Y)
        xscroll.pack(side=tk.BOTTOM, fill=tk.X)
        bottom.add(log_frame, weight=1)

        self.append_log("软件启动：5G主动连接版。本版启动GUI时会自动启动本机Bridge服务。连接栏默认填127.0.0.1:9001；下位机连接本机以太网IP:9000。下位机主动连接参数栏填写要写入采集器Flash的服务器公网IP、设备端口和设备ID。")

    def _schedule_poll(self):
        self._poll_worker_events()
        self.root.after(50, self._schedule_poll)

    def _poll_worker_events(self):
        while True:
            try:
                etype, data = self.event_queue.get_nowait()
            except queue.Empty:
                return

            if etype == "log":
                self.append_log(str(data))
            elif etype == "connected":
                self.connected_var.set("已连接" if data else "未连接")
            elif etype == "ack":
                self._on_ack(data)
            elif etype == "net_ack":
                self._on_net_ack(data)
            elif etype == "status":
                self._on_status(data)
            elif etype == "plot":
                self._on_plot(data)
            elif etype == "error":
                self.append_log("ERROR: " + str(data))

    def _on_ack(self, ack: dict):
        gain_db = range_to_gain_db(ack['range'])
        gain_linear = range_to_gain_linear(ack['range'])
        self.board_cfg_var.set(
            f"板子参数: fs={ack['fs']}, bits={ack['bits']}, gain={ack['range']}({gain_db}dB,x{gain_linear:g}), "
            f"status={ack['status']}, flags=0x{ack['flags']:08X}"
        )
        self.overrun_var.set(f"板端 overrun: {ack['overrun']}")
        self.running_var.set("正在采集" if ack["running"] else "未采集")

        if ack["fs"] in VALID_FS:
            self.fs_var.set(str(ack["fs"]))
        if ack["bits"] in VALID_BITS:
            self.bits_var.set(str(ack["bits"]))
        if ack["range"] in VALID_RANGES:
            self.range_var.set(RANGE_TEXT[ack["range"]])

    def _on_net_ack(self, net_ack: dict):
        self.net_status_var.set(
            f"设备服务器参数: status={net_ack['status']}, "
            f"{net_ack['server_ip']}:{net_ack['server_port']}, "
            f"device_id={net_ack['device_id']}, "
            f"connected={net_ack['connected']}, reconnect_pending={net_ack['reconnect_pending']}, "
            f"local={net_ack.get('local_ip') or '-'}, gw={net_ack.get('gateway') or '-'}"
        )
        if net_ack.get("server_ip"):
            self.device_server_ip_var.set(str(net_ack["server_ip"]))
        if int(net_ack.get("server_port") or 0) > 0:
            self.device_server_port_var.set(str(net_ack["server_port"]))
        if int(net_ack.get("device_id") or 0) > 0:
            self.device_id_var.set(str(net_ack["device_id"]))


    def _on_status(self, status: dict):
        if not status:
            return

        self.latest_status = status
        self.connected_var.set("已连接" if status.get("connected") else "未连接")
        self.running_var.set("正在采集" if status.get("streaming") else "未采集")
        self.periodic_get_state_var.set("采集中自动GET: 打开" if status.get("periodic_get") else "采集中自动GET: 关闭")

        fs = int(status.get("fs") or 0)
        bits = int(status.get("bits") or 0)
        bps = int(status.get("bps") or 0)
        rng = int(status.get("range") or 0)
        fs_voltage = float(status.get("fs_voltage") or 0.0)
        gain_db = int(status.get("gain_db") or range_to_gain_db(rng))
        gain_linear = float(status.get("gain_linear") or range_to_gain_linear(rng))
        input_fs_voltage = float(status.get("input_fs_voltage") or (fs_voltage / gain_linear if gain_linear > 0 else fs_voltage))
        auto_zero = bool(status.get("auto_zero_enabled", False))
        zero_tau = float(status.get("zero_tau_s") or 0.0)
        zero_offset = float(status.get("zero_offset_v") or 0.0)
        zero_valid = bool(status.get("zero_offset_valid", False))
        save_zeroed_wav = bool(status.get("save_zeroed_wav", True))
        plot_decim = int(status.get("plot_decim") or 1)
        plot_effective_fs = float(status.get("plot_effective_fs") or 0.0)
        fft_effective_fs = float(status.get("fft_effective_fs") or 0.0)
        fft_points = int(status.get("fft_points") or 0)
        fft_max_points = int(status.get("fft_max_points") or 0)

        self.parse_info_var.set(
            f"解析依据: DATA帧头 fs={fs}, bits={bits}, bps={bps}, gain={rng}({gain_db}dB,x{gain_linear:g})；"
            f"输入端满量程≈±{input_fs_voltage:g}V，显示电压=ADC差分电压/增益；"
            f"归零后电压 = 输入端电压 - 当前offset；"
            f"时域图抽点显示fs≈{plot_effective_fs:.2f}Hz，抽点={plot_decim}；"
            f"FFT使用未抽点归零数据fs={fft_effective_fs:.2f}Hz，点数={fft_points}/{fft_max_points}"
        )
        self.zero_state_var.set(
            f"自动归零: {'打开' if auto_zero else '关闭'}  "
            f"offset={'%.9f V' % zero_offset if zero_valid else '等待估计'}  "
            f"tau={zero_tau:g}s  WAV保存={'归零后' if save_zeroed_wav else '原始'}"
        )

        self.rate_var.set(f"实时速率: {status.get('instant_rate', 0):.0f} S/s")
        self.samples_var.set(f"样本数: {status.get('total_samples', 0)}")
        self.bytes_var.set(f"字节数: {status.get('total_bytes', 0)}")
        self.overrun_var.set(f"板端 overrun: {status.get('board_overrun', 0)}")
        self.crc_var.set(f"CRC错误: {status.get('crc_errors', 0)} / 坏帧: {status.get('bad_frames', 0)}")

        gap_events = status.get("sample_gap_events", 0)
        missing = status.get("missing_samples", 0)
        frame_gaps = status.get("frame_seq_gap_events", 0)
        overrun = status.get("board_overrun", 0)
        crc_errors = status.get("crc_errors", 0)

        if gap_events == 0 and frame_gaps == 0 and overrun == 0 and crc_errors == 0:
            self.loss_var.set("丢包/丢样: 未发现")
        else:
            self.loss_var.set(f"丢样事件:{gap_events} 缺样:{missing} 帧跳:{frame_gaps}")

        if status.get("saving"):
            self.save_state_var.set(
                f"保存中: fmt={status.get('save_format')}  "
                f"总samples={status.get('save_samples', 0)}  "
                f"当前包#{int(status.get('save_part_index', 0)) + 1} "
                f"start={status.get('save_part_start', '-')} "
                f"part_samples={status.get('save_part_samples', 0)}  "
                f"path={status.get('save_path')}"
            )
        else:
            self.save_state_var.set("未保存")

        self._update_stats_and_judge(status)

    def _update_stats_and_judge(self, status: dict):
        st_raw = status.get("raw_stats", {})
        st_zero = status.get("volt_zero_stats", {})

        if not st_raw or not st_raw.get("valid"):
            self.raw_stat_var.set("raw统计: 等待数据")
            self.volt_zero_stat_var.set("输入端归零后电压统计: 等待数据")
            self.fft_stat_var.set("频域统计: 等待数据")
            self.raw_judge_var.set("短接判断: 等待数据")
            return

        self.raw_stat_var.set(
            f"raw统计: count={st_raw['count']}  min={st_raw['min']}  max={st_raw['max']}  "
            f"mean={st_raw['mean']:.2f}  std={st_raw['std']:.2f}  p-p={st_raw['ptp']}  "
            f"max_abs={st_raw['max_abs']}  满量程占比={st_raw['max_abs_pct']:.5f}%  "
            f"接近满量程={st_raw['near_fs_count']}({st_raw['near_fs_pct']:.5f}%)"
        )


        if st_zero and st_zero.get("valid"):
            self.volt_zero_stat_var.set(
                f"输入端归零后电压统计: count={st_zero['count']}  min={st_zero['min']:.9f} V  max={st_zero['max']:.9f} V  "
                f"mean={st_zero['mean']:.9f} V  std={st_zero['std']:.9f} V  p-p={st_zero['ptp']:.9f} V  "
                f"max_abs={st_zero['max_abs']:.9f} V"
            )
        else:
            self.volt_zero_stat_var.set("输入端归零后电压统计: 等待数据")

        bits = int(status.get("bits") or 0)
        bps = int(status.get("bps") or 0)

        try:
            z24 = int(self.zero_threshold_24_var.get())
            z16 = int(self.zero_threshold_16_var.get())
            n24 = int(self.noise_threshold_24_var.get())
            n16 = int(self.noise_threshold_16_var.get())
        except ValueError:
            z24, z16, n24, n16 = 50000, 200, 10000, 50

        if bps == 3 or bits == 24:
            zero_th = z24
            noise_th = n24
        else:
            zero_th = z16
            noise_th = n16

        result, reason = judge_short_input(st_raw, bits, bps, zero_th, noise_th)
        self.raw_judge_var.set(f"短接判断: {result}，{reason}")

    def _autoscale_time_axis(self, ax, y: np.ndarray):
        if y is None or y.size == 0:
            ax.set_xlim(0, 1)
            ax.set_ylim(-0.001, 0.001)
            return

        ax.set_xlim(0, max(1, y.size - 1))
        ymin = float(np.min(y))
        ymax = float(np.max(y))
        if ymin == ymax:
            ymin -= 0.001
            ymax += 0.001
        margin = max(0.000001, (ymax - ymin) * 0.1)
        ax.set_ylim(ymin - margin, ymax + margin)

    def _compute_fft(self, y: np.ndarray, fs_hz: float) -> tuple[np.ndarray, np.ndarray]:
        """
        根据未抽点的归零后采样缓存计算单边幅度谱。
        幅值单位为 Vpk；为减少频谱泄漏，先扣掉残余均值，再使用 Hann 窗。
        """
        if y is None or y.size < 4 or fs_hz <= 0.0:
            return np.empty(0, dtype=np.float64), np.empty(0, dtype=np.float64)

        max_fft_points = 32768
        yy = np.asarray(y[-max_fft_points:], dtype=np.float64)
        yy = yy - float(np.mean(yy))
        n = int(yy.size)
        if n < 4:
            return np.empty(0, dtype=np.float64), np.empty(0, dtype=np.float64)

        window = np.hanning(n)
        coherent_gain = float(np.sum(window))
        if coherent_gain <= 0.0:
            return np.empty(0, dtype=np.float64), np.empty(0, dtype=np.float64)

        spec = np.fft.rfft(yy * window)
        amp = (2.0 * np.abs(spec)) / coherent_gain
        amp[0] = np.abs(spec[0]) / coherent_gain
        if n % 2 == 0 and amp.size > 1:
            amp[-1] = np.abs(spec[-1]) / coherent_gain
        freq = np.fft.rfftfreq(n, d=1.0 / fs_hz)
        return freq.astype(np.float64), amp.astype(np.float64)

    def _freq_axis_unit(self, max_freq_hz: float) -> tuple[float, str]:
        """
        根据频域横轴最大频率自动选择显示单位。
        返回 scale 和 unit：显示值 = Hz / scale。
        """
        f = abs(float(max_freq_hz))
        if f >= 1_000_000.0:
            return 1_000_000.0, "MHz"
        if f >= 1_000.0:
            return 1_000.0, "kHz"
        return 1.0, "Hz"

    def _format_freq_value(self, freq_hz: float) -> str:
        scale, unit = self._freq_axis_unit(freq_hz)
        val = float(freq_hz) / scale
        if abs(val) >= 100.0:
            return f"{val:.1f} {unit}"
        if abs(val) >= 10.0:
            return f"{val:.2f} {unit}"
        return f"{val:.3f} {unit}"

    def _autoscale_freq_axis(self, ax, freq_display: np.ndarray, amp: np.ndarray, unit: str):
        if freq_display is None or amp is None or freq_display.size == 0 or amp.size == 0:
            ax.set_xlabel("Frequency (Hz)")
            ax.set_xlim(0, 1)
            ax.set_ylim(0, 1)
            return

        ax.set_xlabel(f"Frequency ({unit})")
        ax.set_xlim(0, max(1.0, float(freq_display[-1])))
        ymax = float(np.max(amp))
        if ymax <= 0.0:
            ymax = 1e-6
        ax.set_ylim(0.0, ymax * 1.15)

    def _on_plot(self, data):
        now = time.time()
        if now - self.last_plot_time < 0.08:
            return
        self.last_plot_time = now

        if isinstance(data, dict):
            v_zero_time = np.asarray(data.get("zeroed_time", np.empty(0)), dtype=np.float64)
            v_zero_fft = np.asarray(data.get("zeroed_fft", np.empty(0)), dtype=np.float64)
            fft_fs_hz = float(data.get("fft_fs") or 0.0)
        else:
            # 兼容旧事件格式
            v_zero_time = np.asarray(data, dtype=np.float64)
            v_zero_fft = v_zero_time
            fft_fs_hz = float(self.latest_status.get("fft_effective_fs") or self.latest_status.get("fs") or 0.0)

        if v_zero_time.size == 0:
            return

        x_zero = np.arange(v_zero_time.size)
        self.line_time.set_data(x_zero, v_zero_time)
        self._autoscale_time_axis(self.ax_time, v_zero_time)

        freq_hz, amp = self._compute_fft(v_zero_fft, fft_fs_hz)
        if freq_hz.size and amp.size:
            scale, unit = self._freq_axis_unit(float(freq_hz[-1]))
            freq_display = freq_hz / scale
            self.line_freq.set_data(freq_display, amp)
            self._autoscale_freq_axis(self.ax_freq, freq_display, amp, unit)
        else:
            scale, unit = 1.0, "Hz"
            self.line_freq.set_data([], [])
            self._autoscale_freq_axis(self.ax_freq, freq_hz, amp, unit)

        if freq_hz.size and amp.size:
            peak_idx = int(np.argmax(amp[1:]) + 1) if amp.size > 1 else 0
            self.fft_stat_var.set(
                f"频域统计: FFT点数={min(v_zero_fft.size, 32768)}  "
                f"有效采样率={self._format_freq_value(fft_fs_hz)}  "
                f"横轴单位={unit}  "
                f"主峰≈{self._format_freq_value(freq_hz[peak_idx])}  "
                f"幅值≈{amp[peak_idx]:.9f} Vpk"
            )
        else:
            self.fft_stat_var.set("频域统计: 等待足够数据")

        self.canvas.draw_idle()

    def connect(self):
        ip = self.ip_var.get().strip()
        try:
            port = int(self.port_var.get())
        except ValueError:
            messagebox.showerror("错误", "端口号无效")
            return
        self.worker.command("connect", ip=ip, port=port)

    def disconnect(self):
        self.worker.command("disconnect")

    def get_param(self):
        self.worker.command("get")

    def get_net_param(self):
        self.worker.command("get_net")

    def set_net_param(self):
        try:
            server_ip = self.device_server_ip_var.get().strip()
            server_port = int(self.device_server_port_var.get().strip())
            device_id = int(self.device_id_var.get().strip())
            # 提前校验，避免发错公网IP。
            build_set_net_payload(server_ip, server_port, device_id, save=True, reconnect=True)
        except Exception as e:
            messagebox.showerror("错误", f"主动连接参数无效: {e}")
            return

        if not messagebox.askyesno(
            "确认写入",
            "将把下位机的服务器公网IP/端口/设备ID写入Flash，并让下位机断开当前连接后重连。\n\n"
            f"服务器: {server_ip}:{server_port}\n设备ID: {device_id}\n\n确认继续？"
        ):
            return

        self.worker.command(
            "set_net",
            server_ip=server_ip,
            server_port=server_port,
            device_id=device_id,
            save=True,
            reconnect=True,
        )

    def _get_range_index(self) -> int:
        text = self.range_var.get()
        try:
            return int(text.split("-")[0].strip())
        except Exception:
            return 0

    def set_param(self):
        try:
            fs = int(self.fs_var.get())
            bits = int(self.bits_var.get())
            rng = self._get_range_index()
        except ValueError:
            messagebox.showerror("错误", "参数无效")
            return

        if fs not in VALID_FS:
            messagebox.showerror("错误", "采样率不在允许范围")
            return
        if bits not in VALID_BITS:
            messagebox.showerror("错误", "位数不在允许范围")
            return
        if rng not in VALID_RANGES:
            messagebox.showerror("错误", "增益档位不在允许范围")
            return

        self.worker.command("set", fs=fs, bits=bits, range=rng)

    def start_capture(self):
        try:
            points = int(self.plot_points_var.get())
            if points < 1000:
                points = 1000
            if points > 200000:
                points = 200000
        except ValueError:
            points = 16000

        try:
            fs_voltage = float(self.fs_voltage_var.get())
            if fs_voltage <= 0.0:
                fs_voltage = 4.096
        except ValueError:
            fs_voltage = 4.096

        try:
            dbg_frames = int(self.data_debug_count_var.get())
            if dbg_frames < 0:
                dbg_frames = 0
            if dbg_frames > 100:
                dbg_frames = 100
        except ValueError:
            dbg_frames = 8

        try:
            zero_tau = float(self.zero_tau_var.get())
            if zero_tau < 0.02:
                zero_tau = 0.02
            if zero_tau > 300.0:
                zero_tau = 300.0
        except ValueError:
            zero_tau = 2.0

        self.worker.command(
            "start",
            periodic_get=bool(self.periodic_get_var.get()),
            range=self._get_range_index(),
            plot_points=points,
            fs_voltage=fs_voltage,
            auto_zero_enabled=bool(self.auto_zero_var.get()),
            zero_tau_s=zero_tau,
            save_zeroed_wav=bool(self.save_zeroed_wav_var.get()),
            data_debug_enable=bool(self.data_debug_var.get()),
            data_debug_max_frames=dbg_frames,
            save_enabled=bool(self.save_enable_var.get()),
            save_format=self.save_format_var.get().strip(),
            save_path=self.save_path_var.get().strip()
        )

    def stop_capture(self):
        self.worker.command("stop")

    def clear_stats(self):
        self.worker.command("clear_stats")
        self.raw_stat_var.set("raw统计: 等待数据")
        self.volt_zero_stat_var.set("输入端归零后电压统计: 等待数据")
        self.fft_stat_var.set("频域统计: 等待数据")
        self.zero_state_var.set("自动归零: 打开，等待数据")
        self.raw_judge_var.set("短接判断: 等待数据")
        self.append_log("统计已清零")

    def choose_save_file(self):
        fmt = (self.save_format_var.get() or "raw").lower()
        if fmt not in VALID_SAVE_FORMATS:
            fmt = "raw"

        if fmt == "raw":
            filetypes = [("RAW files", "*.raw"), ("All files", "*.*")]
        elif fmt == "wav":
            filetypes = [("WAV files", "*.wav"), ("All files", "*.*")]
        else:
            filetypes = [("BIN files", "*.bin"), ("All files", "*.*")]

        path = filedialog.asksaveasfilename(
            title=f"选择 {fmt.upper()} 保存基础文件名，实际保存会自动加 YYYYMMDD_HH.MM",
            defaultextension="." + fmt,
            initialfile=os.path.basename(normalize_save_path(self.save_path_var.get().strip(), fmt)),
            filetypes=filetypes
        )
        if path:
            self.save_path_var.set(normalize_save_path(path, fmt))

    def append_log(self, msg: str):
        # 同时输出到 PyCharm/命令行控制台，便于 GUI 窗口未显示或日志区异常时排查。
        try:
            print(msg, flush=True)
        except Exception:
            pass

        try:
            self.log_text.insert(tk.END, msg + "\n")
            self.log_text.see(tk.END)
        except Exception:
            pass

    def on_close(self):
        """关闭 GUI 时，主动关闭 GUI TCP 连接、Bridge 连接和 9000/9001 监听端口。"""
        if getattr(self, "_closing", False):
            return
        self._closing = True

        self.append_log("[SYS] 正在关闭上位机：停止采集线程并释放 9000/9001 端口...")

        try:
            self.worker.command("shutdown")
            # 立即置位，避免 worker 还没处理到 cmd_queue。
            self.worker.stop_event.set()
            try:
                self.worker._close_socket()
            except Exception:
                pass
        except Exception as e:
            self.append_log(f"[SYS] 关闭采集线程时异常：{e}")

        try:
            if getattr(self, "bridge", None) is not None:
                self.bridge.stop()
        except Exception as e:
            self.append_log(f"[SYS] 停止 Bridge 时异常：{e}")

        self._close_deadline = time.time() + 2.0
        self.root.after(100, self._finish_close_when_ready)

    def _finish_close_when_ready(self):
        bridge_alive = False
        worker_alive = False

        try:
            bridge_alive = getattr(self, "bridge", None) is not None and self.bridge.is_alive()
        except Exception:
            bridge_alive = False

        try:
            worker_alive = getattr(self, "worker", None) is not None and self.worker.is_alive()
        except Exception:
            worker_alive = False

        if (bridge_alive or worker_alive) and (time.time() < getattr(self, "_close_deadline", 0)):
            self.root.after(100, self._finish_close_when_ready)
            return

        if bridge_alive:
            self.append_log("[SYS] Bridge 线程未能在2秒内完全退出，程序退出后系统会释放端口。")
        else:
            self.append_log("[SYS] Bridge 已停止，9000/9001 监听端口已释放。")

        try:
            self.root.quit()
        except Exception:
            pass
        try:
            self.root.destroy()
        except Exception:
            pass


def main():
    print("[BOOT] adc_tcp_gui_5g_active_auto_bridge.py started", flush=True)
    root = tk.Tk()

    # 强制把 GUI 窗口拉到前台，避免 PyCharm 运行后窗口被挡住，用户误以为没有启动。
    try:
        root.lift()
        root.attributes("-topmost", True)
        root.after(1200, lambda: root.attributes("-topmost", False))
    except Exception:
        pass

    AdcGuiApp(root)
    print("[BOOT] Tk GUI mainloop entered", flush=True)
    root.mainloop()


if __name__ == "__main__":
    main()
