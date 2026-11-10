"""Run the actual adc_stream.c Thumb code with mocked HAL, without a board.

Requires arm-none-eabi GCC/binutils on PATH and: py -m pip install unicorn
Run from any directory: py tests/test_adc_dc_remove.py
"""

import math
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build" / "test-python"))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_MCLASS, UC_MODE_THUMB
from unicorn.arm_const import (
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP,
)


class Stream:
    def __init__(self, enabled, optimization="-O2"):
        out = ROOT / "build" / "dc-tests"
        out.mkdir(parents=True, exist_ok=True)
        elf = out / f"stream-{int(enabled)}.elf"
        gcc = shutil.which("arm-none-eabi-gcc")
        assert gcc, "arm-none-eabi-gcc must be on PATH"
        subprocess.run([
            gcc, "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", optimization,
            "-g", "-Wall", "-Wextra", "-Werror", "-nostartfiles",
            f"-DAPP_ADC_DC_REMOVE_ENABLE={int(enabled)}",
            "-Itests/stubs", "-Idevice", "device/adc_stream.c",
            "device/app_config.c", "tests/stubs/adc_stream_hal.c",
            "-Ttests/stubs/adc_stream.ld", "-Wl,-e,AdcStream_Init", "-lc", "-lgcc",
            "-o", str(elf),
        ], cwd=ROOT, check=True)
        nm = str(Path(gcc).with_name("arm-none-eabi-nm" + Path(gcc).suffix))
        self.symbols = {}
        for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
            fields = line.split()
            if len(fields) == 3:
                self.symbols[fields[2]] = int(fields[0], 16)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(0x10000, 0x100000)
        self.uc.mem_map(0x20000000, 0x40000)
        # Load ELF32 PT_LOAD segments at their virtual addresses, including .data.
        image = elf.read_bytes()
        assert image[:6] == b"\x7fELF\x01\x01"
        phoff = struct.unpack_from("<I", image, 28)[0]
        phsize, phnum = struct.unpack_from("<HH", image, 42)
        for i in range(phnum):
            kind, offset, addr, _, size, _, _, _ = struct.unpack_from(
                "<8I", image, phoff + i * phsize)
            if kind == 1 and size:
                self.uc.mem_write(addr, image[offset:offset + size])
        self.bits = 24
        self.call("AdcStream_Init")

    def call(self, name, *args):
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1), args):
            self.uc.reg_write(reg, value)
        self.uc.reg_write(UC_ARM_REG_SP, 0x2003F000)
        self.uc.reg_write(UC_ARM_REG_LR, 0xF0001)
        self.uc.emu_start(self.symbols[name] | 1, 0xF0000, count=20000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == 0xF0000, f"{name} did not return"
        return self.uc.reg_read(UC_ARM_REG_R0)

    def start(self, fs, bits):
        self.bits = bits
        self.uc.mem_write(0x20030000, struct.pack("<IBBBB", fs, bits, 0, 0, 0))
        assert self.call("AdcStream_Start", 0x20030000) == 0

    def feed(self, samples, drain=True, statuses=None):
        bps = self.bits // 8
        statuses = statuses if statuses is not None else [1] * len(samples)
        frame = b"".join(bytes([status]) + (x & ((1 << self.bits) - 1)).to_bytes(bps, "big")
                         for x, status in zip(samples, statuses))
        assert len(frame) <= 16384
        self.uc.mem_write(0x20020000, frame)
        self.call("AdcStream_OnContinuousDmaChunk", 0x20020000, len(frame))
        return self.drain() if drain else []

    def drain(self):
        self.call("AdcStream_FlushPartialBlock")
        result = []
        while (addr := self.call("AdcStream_PeekReadyBlock")) != 0:
            state, bps, count, frame_len, data_bytes, _ = struct.unpack(
                "<BBHHHI", self.uc.mem_read(addr, 12))
            assert state == 1 and bps == self.bits // 8
            assert data_bytes == count * bps and frame_len == 28 + data_bytes
            payload = self.uc.mem_read(addr + 12 + 26, data_bytes)
            result.extend(int.from_bytes(payload[i:i + bps], "big", signed=True)
                          for i in range(0, len(payload), bps))
            self.call("AdcStream_ReleaseReadyBlock")
        return result


def reference(samples, fs, bits):
    fs = (fs * 125 + 64) // 128
    omega = 6283185 * 100
    denominator = fs * 1000000 + omega
    alpha = (omega * 1048576 + denominator // 2) // denominator
    mean = None
    result = []
    limit = 1 << (bits - 1)
    for x in samples:
        value = x * 65536
        if mean is None:
            mean = value
        else:
            mean += ((value - mean) * alpha) >> 20
        delta = value - mean
        y = abs(delta) // 65536 * (-1 if delta < 0 else 1)
        result.append(max(-limit, min(limit - 1, y)))
    return result


def main():
    stream = Stream(True)
    rng = random.Random(12711)
    for fs in (25000, 50000, 100000, 200000, 400000):
        stream.uc.mem_write(0x20030000, struct.pack("<IBBBB", fs, 24, 0, 0, 0))
        assert stream.call("AppConfig_ActualFsHz", 0x20030000) == (fs * 125 + 64) // 128
        for bits in (16, 24):
            limit = 1 << (bits - 1)
            for dc in (0, limit - 1, -limit, limit // 3, -limit // 3):
                stream.start(fs, bits)
                assert stream.feed([dc] * 1000) == [0] * 1000
            samples = ([limit - 1] * 20 + [-limit] * 20 + [limit - 1] * 20
                       + [rng.randrange(-limit, limit) for _ in range(3000)])
            expected = reference(samples, fs, bits)
            stream.start(fs, bits)
            actual = []
            for i in range(0, len(samples), 337):
                actual.extend(stream.feed(samples[i:i + 337]))
            assert actual == expected, (fs, bits, "sample or chunk boundary mismatch")
            # Old STATUS frames must not change filter state or sample sequence.
            stream.start(fs, bits)
            valid = [123, -234, 345, -456]
            assert stream.feed([123, limit - 1, -234, -limit, 345, -456],
                               statuses=[1, 0, 1, 0, 1, 1]) == reference(valid, fs, bits)
            assert stream.call("AdcStream_GetSampleSeq") == len(valid)
    print("PASS: all 5 rates, 16/24-bit DC, signed data, saturation, restart, chunk and STATUS handling")

    # Fill all 24 blocks, then change the DC level while samples are dropped.
    stream.start(400000, 24)
    samples = [1000000] * (24 * 790) + [-1000000] * 2000
    for i in range(0, len(samples), 4096):
        stream.feed(samples[i:i + 4096], drain=False)
    assert stream.call("AdcStream_GetOverrunCount") == 2000
    stream.drain()
    assert stream.feed([-1000000]) == reference(samples + [-1000000], 400000, 24)[-1:]
    print("PASS: filter continues tracking DC through TCP backpressure drops")

    for fs in (25000, 400000):
        for frequency in (100, 1000):
            stream.start(fs, 24)
            actual_fs = (fs * 125 + 64) // 128
            samples = [int(2000000 + 500000 * math.sin(2 * math.pi * frequency * i / actual_fs))
                       for i in range(actual_fs // 5)]
            output = []
            for i in range(0, len(samples), 2000):
                output.extend(stream.feed(samples[i:i + 2000]))
            skip = actual_fs // 20
            n = len(output) - skip
            sine = sum(y * math.sin(2 * math.pi * frequency * i / actual_fs)
                       for i, y in enumerate(output) if i >= skip)
            cosine = sum(y * math.cos(2 * math.pi * frequency * i / actual_fs)
                         for i, y in enumerate(output) if i >= skip)
            gain = 2 * math.hypot(sine, cosine) / n / 500000
            assert (0.69 < gain < 0.72) if frequency == 100 else (0.97 < gain < 1.01)
            # The actual clock rate does not give an integer number of tone cycles.
            finite_window_mean = 500000 / (n * abs(math.sin(math.pi * frequency / actual_fs)))
            assert abs(sum(output[skip:]) / n) < finite_window_mean + 2
            print(f"PASS: fs={fs}, tone={frequency}Hz, gain={gain:.5f}")

    raw = Stream(False)
    for bits in (16, 24):
        limit = 1 << (bits - 1)
        samples = [-limit, limit - 1, -1, 0, 1, 1234, -5678] * 100
        raw.start(400000, bits)
        assert raw.feed(samples) == samples
    print("PASS: disabled filter preserves original 16/24-bit sample bytes")


if __name__ == "__main__":
    main()
