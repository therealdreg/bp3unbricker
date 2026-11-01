#!/usr/bin/env python3
# MIT License - bp3unbricker
# ---------------------------------------------------------------------------
# Copyright (c) [2026] by David Reguera Garcia aka Dreg
# https://github.com/therealdreg/bp3unbricker
# https://www.rootkit.es
# X @therealdreg
# dreg@rootkit.es
# ---------------------------------------------------------------------------
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""bp3unbricker.py: the computer side of bp3unbricker, in one file.

It programs a Bus Pirate v3 (PIC24FJ64GA002) with any .hex file through a
Raspberry Pi Pico or Pico 2 running the bp3unbricker firmware. It runs on Windows,
Linux and macOS, with Python 3.8 or later and pyserial (pip install pyserial).
README.md explains everything.

Commands:
  info FILE.hex         check a .hex file (no Pico needed)
  flash [FILE.hex]      erase, program, verify every word, read back, run;
                        without FILE: the image built into the Pico
  verify [FILE.hex]     compare the whole PIC with FILE (or the built-in image)
  read OUT.hex          save the whole PIC as Intel HEX
  bootloader [mit|ds30] write only a bootloader, then read back: bp3mitloader
                        (mit, the default) or the ds30 bootloader of the
                        built-in image
  jumper                hold PGC low like the PGC-PGD jumper: the bootloader
                        waits for buzzloader until 'release'
  id | blank | erase | status | log | release
                        the console command of the same name
  run "COMMAND"         any command of the Pico's console
  term                  interactive console (Ctrl+C to quit)
  install UF2|DIR       copy the firmware to the Pico
  bp PORT info|hello|boot|raw TEXT   talk to the Bus Pirate's own port

Exit status: 0 success, 1 the operation failed, 2 bad file or usage.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import string
import struct
import sys
import threading
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None

VERSION = "1.4.0"  # the firmware's BP3_VERSION, in CMakeLists.txt
ROOT = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(ROOT, "bp3unbricker.c")

FLASH_WORDS = 22016             # program flash, PC 0x000000..0x00ABFE
ROW_WORDS = 64
ROWS = FLASH_WORDS // ROW_WORDS  # 344
BLANK = 0xFFFFFF
# The Bus Pirate v3 flash (buzzpirat), in address order:
RESET_WORDS = 2                            # reset GOTO 0x00A800
BL_PAGE_INDEX = 0xA800 // 2                # bootloader page, up to CW1
BL_VERSION_INDEX = 0xABFA // 2             # version word: 0x0405 is v4.5
CW2_INDEX = 0xABFC // 2
CW1_INDEX = 0xABFE // 2
BL_CODE = range(BL_PAGE_INDEX, CW2_INDEX)  # the bootloader page without CW2/CW1: 0x00A800-0x00ABFB
BP3_CW2, BP3_CW1 = 0x00F9DF, 0x003F7F      # the Bus Pirate v3 firmware's configuration words
BL_WORDS = RESET_WORDS + FLASH_WORDS - BL_PAGE_INDEX  # 514: what a bootloader alone keeps (IMAGE_BL_WORDS)
BOOTLOADERS = ("mit", "ds30")  # the console's 'bootloader [mit|ds30]': bp3mitloader, the image's ds30 bootloader


class HexError(Exception):
    pass


class PicoError(Exception):
    pass


def need_serial():
    if serial is None:
        sys.exit("pyserial is missing: pip install pyserial")


def word_mask(index):
    """Bits that read back as written: the configuration words' upper byte is
    unimplemented and CW1<15> is reserved (reads 0 even when erased)."""
    return 0x7FFF if index == CW1_INDEX else 0xFFFF if index == CW2_INDEX else 0xFFFFFF


def pc(index):
    return f"0x{index * 2:06X}"


def file_sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


# ======================================================================= .hex files


def parse_hex_record(line):
    """(address, type, data) of one Intel HEX record line; HexError says what is wrong with it."""
    if not line.startswith(":"):
        raise HexError("does not start with ':'")
    try:
        rec = bytes.fromhex(line[1:])
    except ValueError:
        raise HexError("not hexadecimal")
    if len(rec) < 5 or len(rec) != rec[0] + 5:
        raise HexError("wrong record length")
    if sum(rec) & 0xFF:
        raise HexError("bad checksum")
    return (rec[1] << 8) | rec[2], rec[3], rec[4:-1]


def write_lines(path, lines):
    """Text lines as an ASCII file with LF line ends on every system."""
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def read_hex(path):
    """Strict Intel HEX reader: {byte address: value}."""
    with open(path, "rb") as f:
        raw = f.read()
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        raise HexError("not an Intel HEX text file")
    data, base, eof = {}, 0, False
    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        where = f"line {lineno}"
        if eof:
            raise HexError(f"{where}: data after the end-of-file record")
        try:
            addr, rtype, payload = parse_hex_record(line)
        except HexError as e:
            raise HexError(f"{where}: {e}")
        if rtype == 0:
            # Linear addresses, also when a record runs past a 64 KB boundary:
            # MPLAB writes PIC24 files like that.
            for i, b in enumerate(payload):
                a = base + addr + i
                if data.get(a, b) != b:
                    raise HexError(f"{where}: byte 0x{a:X} is given twice with different values")
                data[a] = b
        elif rtype == 1:
            eof = True
        elif rtype in (2, 4):
            if len(payload) != 2:
                raise HexError(f"{where}: bad address record")
            value = (payload[0] << 8) | payload[1]
            base = value << 4 if rtype == 2 else value << 16
        elif rtype in (3, 5):
            pass  # start address: has no meaning for a PIC
        else:
            raise HexError(f"{where}: unknown record type {rtype:02X}")
    if not eof:
        raise HexError("no end-of-file record: the file is truncated")
    if not data:
        raise HexError("the file holds no data")
    return data


def pack_words(words):
    """3 bytes per word, little-endian: the bytes every CRC-32 of an image is computed over
    (image_t.data in bp3unbricker.c)."""
    return b"".join(w.to_bytes(3, "little") for w in words)


class Image:
    """The 22016 instructions of the program flash; 0xFFFFFF where the file says nothing."""

    def __init__(self, name, words):
        self.name = name
        self.words = words
        self.packed = pack_words(words)
        self.crc32 = zlib.crc32(self.packed)

    def row_words(self, row):
        return self.words[row * ROW_WORDS:(row + 1) * ROW_WORDS]

    def row_blank(self, row):
        return all(w == BLANK for w in self.row_words(row))

    @property
    def used_rows(self):
        return sum(1 for r in range(ROWS) if not self.row_blank(r))

    @property
    def used_words(self):
        return sum(1 for w in self.words if w != BLANK)


def config_problems(cw2, cw1):
    """Settings that make a Bus Pirate unreadable, stuck or broken (DS39881E 24.1)."""
    p = []
    if cw1 & (1 << 14):
        p.append("CW1 JTAGEN=1: the JTAG port takes four of the PIC's pins")
    if not cw1 & (1 << 13):
        p.append("CW1 GCP=0: code protection, the PIC cannot be read or verified after a reset")
    if not cw1 & (1 << 12):
        p.append("CW1 GWRP=0: flash write protection, the bootloader cannot update the firmware")
    if not cw1 & (1 << 11):
        p.append("CW1 DEBUG=0: the PIC starts in debug mode and does not run on its own")
    if not cw1 & (1 << 10):
        p.append("CW1 bit 10 is reserved and must be 1")
    if (cw1 >> 8) & 3 == 0:
        p.append("CW1 ICS=00 is reserved")
    if not cw1 & (1 << 6) and not cw1 & (1 << 7):
        p.append("CW1 WINDIS=0 needs FWDTEN=1")
    if not cw2 & (1 << 13):
        p.append("CW2 WUTSEL=x0 is reserved")
    if not cw2 & (1 << 11):
        p.append("CW2 SOSCSEL=x0 is reserved")
    if (cw2 >> 8) & 7 == 6:
        p.append("CW2 FNOSC=110 is reserved")
    return p


def load_image(path, config):
    """Read a .hex file into (Image, notes, problems). config says what to do when the file has
    no configuration words: 'file' refuses it, 'add' adds the Bus Pirate v3 values, 'none'
    leaves them erased and 'info' only says so."""
    data = read_hex(path)
    words = [BLANK] * FLASH_WORDS
    lanes = {}
    outside, phantom = [], []
    for a in sorted(data):
        index, lane = divmod(a, 4)
        if index >= FLASH_WORDS:
            outside.append(a)
            continue
        lanes.setdefault(index, set()).add(lane)
        if lane == 3:
            if data[a]:
                phantom.append(a)
        else:
            words[index] = (words[index] & ~(0xFF << (8 * lane))) | (data[a] << (8 * lane))
    if outside:
        raise HexError(f"{len(outside)} bytes outside the program flash (PC 0x000000-0x00ABFE), "
                       f"the first at PC 0x{outside[0] // 2:06X}: not a PIC24FJ64GA002 image")
    if phantom:
        raise HexError(f"non-zero phantom byte at PC 0x{phantom[0] // 2:06X}: not a PIC24 .hex file")
    partial = [i for i, got in sorted(lanes.items()) if got != {0, 1, 2, 3}]
    if partial:
        raise HexError(f"incomplete instruction at PC {pc(partial[0])} ({len(partial)} in total)")

    notes, problems = [], []
    has_cw2, has_cw1 = CW2_INDEX in lanes, CW1_INDEX in lanes
    if has_cw2 != has_cw1:
        raise HexError("the file has only one of the two configuration words")
    if not has_cw2:
        if config == "add":
            words[CW2_INDEX], words[CW1_INDEX] = BP3_CW2, BP3_CW1
            notes.append(f"configuration words added (Bus Pirate v3): CW2=0x{words[CW2_INDEX]:04X} "
                         f"CW1=0x{words[CW1_INDEX]:04X}")
        elif config == "none":
            notes.append("no configuration words: they stay erased (watchdog on, FRC without PLL)")
        elif config == "info":
            notes.append("no configuration words in the file (flash needs --add-config or --no-config)")
        else:
            raise HexError("the file has no configuration words (CW1/CW2 at PC 0x00ABFE/0x00ABFC).\n"
                           "  Erased, they leave the watchdog on and the clock without PLL: a Bus Pirate\n"
                           "  firmware will not run. Use --add-config to add the Bus Pirate v3 values\n"
                           f"  (CW2=0x{BP3_CW2:04X} CW1=0x{BP3_CW1:04X}), or --no-config to program the file as it is.")
    else:
        problems = config_problems(words[CW2_INDEX] & 0xFFFF, words[CW1_INDEX] & 0xFFFF)
        if words[CW1_INDEX] & 0x8000:
            notes.append("CW1 bit 15 is set: it is reserved and always reads 0, so it is not compared")
    if all(words[i] == BLANK for i in BL_CODE):
        notes.append("no bootloader in 0x00A800-0x00ABFB: the firmware runs, but cannot be updated "
                     "over USB until a bootloader is programmed")
    return Image(os.path.basename(path), words), notes, problems


def bootloader_alone(image):
    """The ds30 bootloader of a Bus Pirate v3 image alone, as the Pico's 'bootloader ds30' writes it
    (image_word() of a bootloader_only image in bp3unbricker.c): the reset GOTO and the page
    0x00A800..0x00ABFE, every other word erased."""
    words = [w if i < RESET_WORDS or i >= BL_PAGE_INDEX else BLANK for i, w in enumerate(image.words)]
    blver = image.words[BL_VERSION_INDEX]
    return Image(f"bootloader v{blver >> 8}.{blver & 0xFF} of {image.name}", words)


def bootloader_expand(part):
    """A bootloader alone as bp3unbricker.c keeps it (the reset GOTO, then the bootloader page) as
    all 22016 words, the ones in between erased."""
    return part[:RESET_WORDS] + [BLANK] * (BL_PAGE_INDEX - RESET_WORDS) + part[RESET_WORDS:]


def describe(path, image, notes, problems):
    """What info, flash and verify print about FILE before they use it."""
    def field(label, text):
        print(f"{label:<11}: {text}")

    used = [i for i, w in enumerate(image.words) if w != BLANK]
    w0, w1 = image.words[0], image.words[1]
    field("File", f"{path} (sha256 {file_sha256(path)})")
    if used:
        field("Program", f"{len(used)} instructions in {image.used_rows} of {ROWS} rows, "
                         f"PC {pc(used[0])}-{pc(used[-1])}")
    else:
        field("Program", "every word is erased (0xFFFFFF)")
    if (w0 >> 16) == 0x04:
        field("Reset", f"GOTO 0x{(w0 & 0xFFFE) | ((w1 & 0x7F) << 16):06X}")
    if any(image.words[i] != BLANK for i in BL_CODE):
        blver = image.words[BL_VERSION_INDEX]
        version = f", version word 0x{blver:04X} (v{blver >> 8}.{blver & 0xFF})" if blver < 0x1000 else ""
        field("Bootloader", f"present in 0x00A800-0x00ABFB{version}")
    cw2, cw1 = image.words[CW2_INDEX] & 0xFFFF, image.words[CW1_INDEX] & 0xFFFF
    erased = " (erased)" if image.words[CW1_INDEX] == BLANK else ""
    field("Config", f"CW2=0x{cw2:04X} CW1=0x{cw1:04X}{erased}")
    field("crc32", f"{image.crc32:08X} (of the {FLASH_WORDS}-word image)")
    for note in notes:
        field("Note", note)
    for problem in problems:
        field("WARNING", problem)


# ======================================================================= the image inside bp3unbricker.c

IMAGE_BEGIN = "/* BEGIN GENERATED IMAGE */"
IMAGE_END = "/* END GENERATED IMAGE */"


def embedded_from_source():
    """(image, its ds30 bootloader alone, bp3mitloader alone) from the image section of bp3unbricker.c, CRCs
    checked, and the ds30 one checked against the image."""
    with open(SOURCE, encoding="utf-8") as f:
        text = f.read()
    try:
        section = text[text.index(IMAGE_BEGIN):text.index(IMAGE_END)]
    except ValueError:
        raise HexError(f"{SOURCE}: no generated image section")
    arrays = {name: bytes(int(v) for v in re.findall(r"\d+", body))
              for name, body in re.findall(r"static const uint8_t (\w+)\[\w+\] = \{([^}]*)\}", section)}
    names = re.findall(r'\.name = "([^"]+)"', section)
    crcs = [int(v, 16) for v in re.findall(r"\.crc32 = 0x([0-9A-F]{8})u", section)]
    sizes = {"image_data": FLASH_WORDS * 3, "ds30_data": BL_WORDS * 3, "mit_data": BL_WORDS * 3}
    if {name: len(data) for name, data in arrays.items()} != sizes or len(names) != 3 or len(crcs) != 3:
        raise HexError(f"{SOURCE}: the image section is damaged")

    def words(data):
        return [int.from_bytes(data[3 * i:3 * i + 3], "little") for i in range(len(data) // 3)]

    image = Image(names[0], words(arrays["image_data"]))
    ds30 = bootloader_alone(image)
    mit = Image(names[2], bootloader_expand(words(arrays["mit_data"])))
    if [image.crc32, ds30.crc32, mit.crc32] != crcs or ds30.name != names[1] or \
            bootloader_expand(words(arrays["ds30_data"])) != ds30.words:
        raise HexError(f"{SOURCE}: the image section does not match its CRCs")
    return image, ds30, mit


# ======================================================================= the Pico

RPI_VID = 0x2E8A
RESULT_RE = re.compile(r"^\[[\w ]+: (.*), \d+ ms\]$")    # '[name: result, N ms]' ends an operation
UNSAFE_NAME_CHAR_RE = re.compile(r"[^A-Za-z0-9._+()-]")  # what the Pico's 'load' turns into '_'
UPLOAD_NAME_MAX_LEN = 47                                 # what the Pico keeps of an upload's name
UPLOAD_ROW_TRIES = 3                                     # sends of one row before the upload is given up


def pico_ports():
    """The serial ports of Raspberry Pi boards: a Pico running bp3unbricker, or any other
    Pico SDK USB serial firmware."""
    return {p.device for p in list_ports.comports() if p.vid == RPI_VID}


def find_port():
    ports = sorted(pico_ports())
    if not ports:
        raise PicoError("no Raspberry Pi Pico found (is bp3unbricker running on it? try --port)")
    if len(ports) > 1:
        raise PicoError(f"several Raspberry Pi serial ports ({', '.join(ports)}): choose one with --port")
    return ports[0]


def safe_name(name):
    """The name the Pico gives an upload (loader_begin() in bp3unbricker.c section 8): upload()
    expects it back in the 'LOAD OK' reply."""
    return UNSAFE_NAME_CHAR_RE.sub("_", name)[:UPLOAD_NAME_MAX_LEN] or "uploaded-image"


def row_line(image, row):
    """One row in the 'load' protocol: 'R rrr', the row's 64 words and the CRC-32 of their bytes."""
    words = image.row_words(row)
    crc = zlib.crc32(pack_words(words))
    return f"R {row:03X} {''.join(f'{w:06X}' for w in words)} {crc:08X}"


class Pico:
    """The bp3unbricker console on the Pico's USB serial port (default: the only Raspberry Pi one)."""

    def __init__(self, port=None):
        port = port or find_port()
        try:
            # Never 1200 baud: that reboots the Pico into its USB bootloader.
            self.ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=10)
        except serial.SerialException as e:
            hint = " (Linux: add yourself to the 'dialout' group)" if sys.platform.startswith("linux") else ""
            raise PicoError(f"cannot open {port}: {e}{hint}")
        self.ser.dtr = True  # the firmware prints its banner and a prompt when DTR comes up
        self.pending = ""    # received, not read yet
        self.port = port
        self._sync()

    def close(self):
        self.ser.close()

    def _fill(self):
        data = self.ser.read(self.ser.in_waiting or 1)
        if data:
            self.pending += data.decode("utf-8", "replace")

    def read_line(self, timeout):
        deadline = time.monotonic() + timeout
        while "\n" not in self.pending:
            if time.monotonic() > deadline:
                return None
            self._fill()
        line, self.pending = self.pending.split("\n", 1)
        return line.rstrip("\r")

    def _at_prompt(self):
        return self.pending.endswith("> ") and "\n" not in self.pending

    def wait_prompt(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pending = self.pending.rpartition("\n")[2]  # complete lines are not needed here
            if self._at_prompt():
                self.pending = ""
                return True
            self._fill()
        return False

    def _sync(self):
        """Opening the port prints the banner and a prompt; if not, ask for one.
        A run started with the BOOTSEL button may still be going on."""
        if self.wait_prompt(3):
            return
        self.ser.write(b"help\r")
        print(f"waiting for the Pico on {self.port}...", file=sys.stderr)
        if not self.wait_prompt(120):
            raise PicoError(f"no prompt from the Pico on {self.port}")

    def run(self, cmd, timeout=600, show=True, on_line=None, needs_result=True):
        """Run a console command; return (ok, lines). ok means a '[name: OK, N ms]'
        line came; with needs_result=False a command that prints none is ok too."""
        self.ser.write(cmd.encode("ascii") + b"\r")
        lines, result, echo = [], None, True
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read_line(0.2)
            if line is None:
                if self._at_prompt():
                    self.pending = ""
                    return result == "OK" or (result is None and not needs_result), lines
                continue
            if echo and line.strip() == cmd:
                echo = False
                continue
            echo = False
            lines.append(line)
            m = RESULT_RE.match(line.strip())
            if m:
                result = m.group(1)
            if on_line:
                on_line(line)
            elif show:
                print(line, flush=True)
        raise PicoError(f"'{cmd}' did not finish within {timeout} s")

    def start_load(self, name):
        """Send 'load NAME' and return the Pico's answer after the echo of the command:
        'LOAD READY 344 64' when it takes an upload, None if it says nothing."""
        self.ser.write(f"load {name}\r".encode("ascii"))
        for _ in range(3):  # the echo of the command comes first
            reply = self.read_line(5)
            if reply is None or reply.startswith(("LOAD READY", "ERROR", "Unknown")):
                break
        return reply

    def send_line(self, line):
        """One line of the 'load' protocol; the Pico's reply, or None after 10 s."""
        self.ser.write(line.encode("ascii") + b"\n")
        return self.read_line(10)

    def upload(self, image, show=True):
        """Send the image with the 'load' protocol (bp3unbricker.c section 8).
        Returns how many times a refused row was sent again."""
        name = safe_name(image.name)
        resent = 0
        reply = self.start_load(name)
        if reply != f"LOAD READY {ROWS} {ROW_WORDS}":
            raise PicoError(f"the Pico does not take uploads ({reply!r}): update its firmware")
        rows = [r for r in range(ROWS) if not image.row_blank(r)]
        t0 = time.monotonic()
        for n, row in enumerate(rows, 1):
            line = row_line(image, row)
            for attempt in range(1, UPLOAD_ROW_TRIES + 1):
                reply = self.send_line(line)
                if reply == f"OK {row:03X}":
                    break
                if reply is None or not reply.startswith(f"ERR {row:03X}"):
                    self.ser.write(b"X\n")
                    raise PicoError(f"row {row}: unexpected answer {reply!r}")
                if attempt < UPLOAD_ROW_TRIES:
                    resent += 1
                    print(f"  row {row}: {reply}, sending it again ({attempt})", flush=True)
            else:
                self.ser.write(b"X\n")
                raise PicoError(f"row {row} was refused {UPLOAD_ROW_TRIES} times (last answer: {reply})")
            if show and (n % 32 == 0 or n == len(rows)):
                print(f"\r  sent {n}/{len(rows)} rows", end="", flush=True)
        if show:
            print()
        reply = self.send_line(f"E {image.crc32:08X}")
        if not reply or not reply.startswith(f"LOAD OK {name} crc32 {image.crc32:08X}"):
            raise PicoError(f"the upload was not accepted: {reply!r}")
        self.wait_prompt(5)
        if show:
            print(f"  {reply} ({time.monotonic() - t0:.1f} s)")
        return resent

    def read_chip(self, show=True):
        """Read all 22016 words with 'dump': record checksums, coverage and the
        dump's CRC-32 are checked. Returns (words, hex lines)."""
        hex_lines, end = [], []

        def collect(line):
            if line.startswith(":"):
                hex_lines.append(line)
            elif line.startswith("END HEX"):
                end.append(line)
            elif show and not line.startswith("BEGIN HEX") and not RESULT_RE.match(line.strip()):
                print(line, flush=True)

        ok, _ = self.run("dump", timeout=300, on_line=collect)
        if not ok or not end:
            raise PicoError("the read failed")
        m = re.search(r"crc32=([0-9A-F]{8})", end[0])
        if not m:
            raise PicoError(f"the read failed: no crc32 in '{end[0]}'")
        printed = int(m.group(1), 16)
        data, base = {}, 0
        for line in hex_lines:
            try:
                addr, rtype, payload = parse_hex_record(line)
            except HexError:
                raise PicoError(f"damaged record in the read: {line}")
            if rtype == 4:
                base = ((payload[0] << 8) | payload[1]) << 16
            elif rtype == 0:
                for i, b in enumerate(payload):
                    data[base + addr + i] = b
        if sorted(data) != list(range(FLASH_WORDS * 4)):
            raise PicoError("the read does not cover every word exactly once")
        words = [data[4 * i] | (data[4 * i + 1] << 8) | (data[4 * i + 2] << 16) for i in range(FLASH_WORDS)]
        crc = zlib.crc32(pack_words(words))
        if crc != printed:
            raise PicoError(f"the read was damaged on the way (crc32 {crc:08X}, the Pico sent {printed:08X})")
        return words, hex_lines


def with_pico(args, action):
    """Open the Pico, apply --speed, run action(pico), which returns True on success, and return the
    exit status. An error of the action is reported here, before the speed is restored. main()
    reports an error while opening. pyserial is checked here, after the command has checked its
    arguments and FILE."""
    need_serial()
    pico = Pico(args.port)
    try:
        if args.speed:
            pico.run(f"speed {args.speed}", show=False, needs_result=False)
        return 0 if action(pico) else 1
    except (OSError, PicoError) as e:  # serial.SerialException is an OSError
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 1
    finally:
        if args.speed:
            try:
                pico.run("speed normal", show=False, needs_result=False, timeout=10)
            except (OSError, PicoError):
                pass
        pico.close()


# ======================================================================= programming

IMAGE_LINE_RE = re.compile(r"^Image: .* \(crc32 ([0-9A-F]{8})", re.M)


def written_crc32(lines):
    """The crc32 on the 'Image: NAME (crc32 X, N rows used)' line that an operation prints
    before it writes an image ('flash', 'bootloader', 'test'), or None."""
    m = IMAGE_LINE_RE.search("\n".join(lines))
    return int(m.group(1), 16) if m else None


def differing_words(expected, words):
    """Indexes of the words that differ from expected in a bit that reads back as written."""
    return [i for i in range(FLASH_WORDS) if (expected[i] ^ words[i]) & word_mask(i)]


def check_readback(pico, image):
    """Read the whole PIC and compare it with image: True if every word matches."""
    words, _ = pico.read_chip()
    bad = differing_words(image.words, words)
    if not bad:
        print(f"READBACK OK: all {FLASH_WORDS} words of the PIC match {image.name}")
        return True
    for i in bad[:10]:
        print(f"  PC {pc(i)}: {image.name} has {image.words[i]:06X}, the PIC has {words[i]:06X}")
    print(f"READBACK FAILED: {len(bad)} words differ from {image.name}")
    return False


def checked_image(args, refuse_problems):
    """Read and check FILE before the Pico is touched; refuse_problems: refuse configuration words
    that would leave the Bus Pirate unusable."""
    if args.add_config and args.no_config:
        raise HexError("--add-config and --no-config exclude each other")
    config = "add" if args.add_config else "none" if args.no_config else "file"
    image, notes, problems = load_image(args.file, config)
    describe(args.file, image, notes, problems)
    if problems and refuse_problems:
        raise HexError("the configuration words above would leave the Bus Pirate unusable; "
                       "check the file, or pass --force to write them anyway")
    return image


def cmd_info(args):
    try:
        image, notes, problems = load_image(args.file, "info")
    except (OSError, HexError) as e:
        print(f"{args.file}: {e}", file=sys.stderr)
        return 2
    describe(args.file, image, notes, problems)
    return 0


def flash_file(pico, image, readback):
    print(f"Sending {image.name} to the Pico...")
    pico.upload(image)
    ok, lines = pico.run("flash loaded")
    written = written_crc32(lines)
    if written is not None and written != image.crc32:
        print(f"ERROR: the Pico wrote an image with crc32 {written:08X}, not {image.crc32:08X}")
        return False
    if not ok:
        return False
    if not readback:
        return True
    print("Reading the whole PIC back to compare it with the file...")
    return check_readback(pico, image)


def cmd_flash(args):
    if not args.file:
        return with_pico(args, lambda pico: pico.run("flash")[0])
    try:
        image = checked_image(args, refuse_problems=not args.force)
    except (OSError, HexError) as e:
        print(f"{args.file}: {e}", file=sys.stderr)
        return 2
    return with_pico(args, lambda pico: flash_file(pico, image, readback=not args.no_readback))


def verify_file(pico, image):
    print("Reading the whole PIC...")
    return check_readback(pico, image)


def cmd_verify(args):
    if not args.file:
        return with_pico(args, lambda pico: pico.run("verify")[0])
    try:
        image = checked_image(args, refuse_problems=False)
    except (OSError, HexError) as e:
        print(f"{args.file}: {e}", file=sys.stderr)
        return 2
    return with_pico(args, lambda pico: verify_file(pico, image))


def cmd_read(args):
    def read(pico):
        words, hex_lines = pico.read_chip()
        write_lines(args.out, hex_lines)
        crc = zlib.crc32(pack_words(words))
        print(f"saved {args.out}: {FLASH_WORDS} words, crc32 {crc:08X} (checked)")
        return True

    return with_pico(args, read)


def flash_bootloader(pico, which, readback):
    """The Pico's 'bootloader mit' (bp3mitloader) or 'bootloader ds30' (the ds30 bootloader of its image),
    checked against the CRC its 'status' gives and, with readback, against the image section of
    bp3unbricker.c and a full read of the PIC."""
    _, lines = pico.run("status", show=False, needs_result=False)
    pico_bl = re.search(rf"^Bootloader {which}: .*, crc32 ([0-9A-F]{{8}}) \(OK", "\n".join(lines), re.M)
    if not pico_bl:  # an older firmware, or that bootloader corrupt in the Pico's flash
        print(f"ERROR: the Pico's firmware has no usable 'bootloader {which}': update it (install)")
        return False
    bl_crc = int(pico_bl.group(1), 16)
    if readback:
        try:
            _, ds30, mit = embedded_from_source()
        except (OSError, HexError) as e:
            print(f"ERROR: {e}\n  The result cannot be checked here (--no-readback skips that).")
            return False
        bl = mit if which == "mit" else ds30
        if bl.crc32 != bl_crc:
            print(f"ERROR: {SOURCE} does not hold the bootloader built into the Pico "
                  f"(crc32 {bl.crc32:08X}, the Pico's is {bl_crc:08X})")
            return False
    ok, lines = pico.run(f"bootloader {which}")
    if not ok:
        return False
    if written_crc32(lines) != bl_crc:
        print(f"ERROR: the Pico did not write the bootloader image (crc32 {bl_crc:08X})")
        return False
    if not readback:
        return True
    print(f"Reading the whole PIC back to compare it with {bl.name}...")
    if not check_readback(pico, bl):
        return False
    n = bl.used_words
    print(f"Only the bootloader is on the PIC: {n} words, the other {FLASH_WORDS - n} erased.")
    if which == "mit":
        print("It waits for a firmware, MODE LED on: buzzloader --dev=<the Bus Pirate's port> --hex=<firmware.hex>")
    else:
        print("To load a firmware: python bp3unbricker.py jumper, then buzzloader --dev=<the Bus Pirate's port>\n"
              "--hex=<firmware.hex>, then python bp3unbricker.py release")
    return True


def cmd_bootloader(args):
    return with_pico(args, lambda pico: flash_bootloader(pico, args.which, readback=not args.no_readback))


def cmd_passthrough(args):
    """jumper, id, blank, erase, status, log, release: the console command of the same name.
    status and log print no result line of their own; log replays the last operation's, so its
    exit status is that operation's result."""
    needs_result = args.action not in ("status", "log")
    return with_pico(args, lambda pico: pico.run(args.action, needs_result=needs_result)[0])


def cmd_run(args):
    if not args.command.isascii():
        print(f"ERROR: the Pico's console takes ASCII only: {args.command!r}", file=sys.stderr)
        return 2
    # A newline would end it, and the console would run the rest too. To an empty line it answers nothing,
    # not even a prompt.
    if not args.command.strip() or not args.command.isprintable():
        print(f"ERROR: one command of the Pico's console, without control characters: {args.command!r}",
              file=sys.stderr)
        return 2
    return with_pico(args, lambda pico: pico.run(args.command, needs_result=False)[0])


def cmd_term(args):
    def term(pico):
        def reader():
            while True:
                try:
                    data = pico.ser.read(pico.ser.in_waiting or 1)
                except serial.SerialException:
                    return
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()

        threading.Thread(target=reader, daemon=True).start()
        sys.stdout.write("> ")
        sys.stdout.flush()
        try:
            for line in sys.stdin:
                pico.ser.write(line.rstrip("\r\n").encode("ascii", "replace") + b"\r")
            time.sleep(1)  # end of input: let the last answer arrive
        except KeyboardInterrupt:
            pass
        return True

    return with_pico(args, term)


# ======================================================================= the Bus Pirate's own port

def bp_read_until_quiet(ser, quiet_s=0.4, max_s=4.0):
    """Read until the line has been quiet for quiet_s seconds (or max_s elapsed)."""
    out = bytearray()
    start = last = time.monotonic()
    while time.monotonic() - start < max_s:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            out += chunk
            last = time.monotonic()
        elif time.monotonic() - last > quiet_s:
            break
    return bytes(out)


def bp_talk(port, what, text=""):
    """Talk to a Bus Pirate v3 (firmware or bootloader) over its FTDI port.
    info: the firmware's 'i'; hello: the bootloader's HELLO (0xC1); boot: leave the
    bootloader (0xE4) and print the firmware's 'i'; raw: TEXT + newline."""
    with serial.Serial(port, 115200, timeout=0.05) as ser:
        ser.reset_input_buffer()
        if what == "boot":
            # The bootloader v4.5 HELLO parser computes rx - 0xC1 - 0x23, so 0xE4 starts
            # the firmware, even while PGC is held low. A bootloader in the middle of a
            # session times out back to its hello stage after 500 ms.
            time.sleep(1.5)
            ser.reset_input_buffer()
            ser.write(b"\xE4")
            reply = bp_read_until_quiet(ser, quiet_s=0.5, max_s=3.0)
            if b"HiZ>" not in reply:
                ser.write(b"\n")
                bp_read_until_quiet(ser, quiet_s=0.3, max_s=2.0)
            ser.write(b"i\n")
            return bp_read_until_quiet(ser, quiet_s=0.5, max_s=3.0)
        if what == "hello":
            ser.write(b"\xC1")
            return bp_read_until_quiet(ser, quiet_s=0.3, max_s=2.0)
        ser.write(b"\n")
        bp_read_until_quiet(ser, quiet_s=0.3, max_s=1.5)
        ser.write(b"i\n" if what == "info" else text.encode() + b"\n")
        return bp_read_until_quiet(ser)


def cmd_bp(args):
    reply = bp_talk(args.dev, args.what, args.text or "")
    if args.what == "hello":
        print("reply:", reply.hex(" ") or "<nothing>")
        if len(reply) >= 4 and reply[3] == ord("K"):
            print(f"bootloader: device id 0x{reply[0]:02X}, version {reply[1]}.{reply[2] >> 4}.{reply[2] & 0xF}")
            return 0
        return 1
    sys.stdout.write(reply.decode("latin-1") or "<nothing>\n")
    return 0 if reply else 1


# ======================================================================= UF2: install

UF2_MAGIC = (0x0A324655, 0x9E5D5157)
UF2_FLAG_FAMILY_ID = 0x00002000
# UF2 family IDs, named after the Pico SDK's boot/uf2.h
RP2040_FAMILY = 0xE48BFF56
RP2350_ARM_S_FAMILY = 0xE48BFF59
RP2350_RISCV_FAMILY = 0xE48BFF5A
RP2350_ARM_NS_FAMILY = 0xE48BFF5B
# Code families each boot ROM runs.
CHIP_FAMILIES = {
    "RP2040": {RP2040_FAMILY},
    "RP2350": {RP2350_ARM_S_FAMILY, RP2350_RISCV_FAMILY, RP2350_ARM_NS_FAMILY},
}
BOOT_DRIVE_CHIPS = {"RPI-RP2": "RP2040", "RP2350": "RP2350"}  # the chip of each "Board-ID" of INFO_UF2.TXT


def uf2_families(path):
    """{family_id: block_count} for the 512-byte UF2 blocks of a file."""
    with open(path, "rb") as f:
        data = f.read()
    families = {}
    for off in range(0, len(data) - 511, 512):
        magic0, magic1, flags, _, _, _, _, family = struct.unpack_from("<8I", data, off)  # the block header
        if (magic0, magic1) == UF2_MAGIC and flags & UF2_FLAG_FAMILY_ID:
            families[family] = families.get(family, 0) + 1
    return families


def uf2_chips(path):
    """Chips whose boot ROM would run the code in this file."""
    families = set(uf2_families(path))
    return [chip for chip, chip_families in CHIP_FAMILIES.items() if families & chip_families]


def boot_drives():
    """{mount point: chip} for every mounted UF2 boot drive (RPI-RP2, RP2350)."""
    if os.name == "nt":
        candidates = [f"{letter}:\\" for letter in string.ascii_uppercase]
    else:
        user = os.environ.get("USER", "")
        candidates = (glob.glob(f"/media/{user}/*") + glob.glob("/media/*") + glob.glob(f"/run/media/{user}/*")
                      + glob.glob("/Volumes/*"))
    drives = {}
    for mount in candidates:
        try:
            with open(os.path.join(mount, "INFO_UF2.TXT"), encoding="ascii", errors="replace") as f:
                info = f.read()
        except OSError:
            continue
        for line in info.splitlines():
            if line.startswith("Board-ID:"):
                chip = BOOT_DRIVE_CHIPS.get(line.split(":", 1)[1].strip())
                if chip:
                    drives[mount] = chip
    return drives


def wait_for(probe):
    """probe() every 0.25 s until it returns something; None after 10 s."""
    for _ in range(40):
        time.sleep(0.25)
        found = probe()
        if found:
            return found
    return None


def cmd_install(args):
    """Copy the right UF2 to the Pico. A board running bp3unbricker (or any Pico SDK USB
    serial firmware) is first rebooted into BOOTSEL by opening its port at 1200 baud;
    otherwise hold BOOTSEL while plugging it in. Given a folder, the .uf2 in it that
    matches the chip of the boot drive is used. A missing file, a file that is no Pico UF2 and a
    folder without one are refused (exit status 2) before the Pico is touched."""
    if os.path.isdir(args.uf2):
        found = [f for f in glob.glob(os.path.join(glob.escape(args.uf2), "*.uf2")) if os.path.isfile(f)]
        if not any(uf2_chips(f) for f in found):
            print(f"{args.uf2}: no UF2 file for a Pico or a Pico 2 in this folder", file=sys.stderr)
            return 2
    elif not os.path.isfile(args.uf2):
        print(f"{args.uf2}: no such file", file=sys.stderr)
        return 2
    elif not uf2_chips(args.uf2):
        print(f"{args.uf2}: not a UF2 file for a Pico or a Pico 2", file=sys.stderr)
        return 2
    need_serial()
    drives = boot_drives()
    if not drives:
        ports = [args.port] if args.port else sorted(pico_ports())
        if len(ports) > 1:
            print(f"several boards running ({', '.join(ports)}): choose one with --port", file=sys.stderr)
            return 1
        if ports:
            print(f"rebooting {ports[0]} into BOOTSEL (1200 baud touch)")
            try:
                serial.Serial(ports[0], 1200).close()
            except serial.SerialException:
                pass
            drives = wait_for(boot_drives)
    if not drives:
        print("no RPI-RP2 or RP2350 drive: hold BOOTSEL while plugging the board in", file=sys.stderr)
        return 1
    if len(drives) > 1:
        print(f"several boards in BOOTSEL mode ({', '.join(drives)}): leave only one", file=sys.stderr)
        return 1
    drive, chip = next(iter(drives.items()))
    path = args.uf2
    if os.path.isdir(path):
        matches = [f for f in sorted(glob.glob(os.path.join(glob.escape(path), "*.uf2")))
                   if os.path.isfile(f) and chip in uf2_chips(f)]
        if len(matches) != 1:
            print(f"{path}: expected one .uf2 for {chip}, found {len(matches)}", file=sys.stderr)
            return 1
        path = matches[0]
    else:
        chips = uf2_chips(path)
        if chip not in chips:
            print(f"{path} is built for {', '.join(chips) or 'no known chip'}, but the board is an {chip}",
                  file=sys.stderr)
            return 1
    before = pico_ports()
    print(f"copying {path} ({os.path.getsize(path)} bytes) to {drive} ({chip})")
    shutil.copy(path, drive)
    new = wait_for(lambda: pico_ports() - before)  # the board reboots into the new firmware
    if new:
        print("running the new firmware:", sorted(new))
    else:
        print("copied; no new serial port yet")
    return 0


# ======================================================================= command line

# The project's banner, the same as the firmware's (print_banner() in bp3unbricker.c) except for its second line
BANNER = "\n".join(["-" * 78,
                    f" bp3unbricker.py {VERSION}",
                    " Bus Pirate v3 unbricker: programs its PIC24FJ64GA002 over ICSP",
                    " https://github.com/therealdreg/bp3unbricker",
                    " MIT License - Copyright (c) 2026 David Reguera Garcia aka Dreg",
                    " X @therealdreg - https://www.rootkit.es - dreg@rootkit.es",
                    "-" * 78])


def main(argv=None):
    ap = argparse.ArgumentParser(prog="bp3unbricker.py", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="See README.md. Exit status: 0 ok, 1 failed, 2 bad file or usage.")
    ap.add_argument("--version", action="version", version=f"bp3unbricker.py {VERSION}")
    ap.add_argument("--port", help="the Pico's serial port (default: the only Raspberry Pi one)")
    ap.add_argument("--speed", choices=("slow", "normal", "fast"), help="PGC clock for this run")
    sub = ap.add_subparsers(dest="action", required=True, metavar="COMMAND")

    p = sub.add_parser("info", help="check a .hex file (no Pico needed)")
    p.add_argument("file")
    p.set_defaults(func=cmd_info)
    for name, text, func in (("flash", "erase, program and verify (no FILE: the built-in image)", cmd_flash),
                             ("verify", "compare the PIC with FILE (no FILE: the built-in image)", cmd_verify)):
        p = sub.add_parser(name, help=text)
        p.add_argument("file", nargs="?")
        p.add_argument("--add-config", action="store_true",
                       help="FILE has no configuration words: add the Bus Pirate v3 ones")
        p.add_argument("--no-config", action="store_true", help="FILE has no configuration words: leave them erased")
        if name == "flash":
            p.add_argument("--force", action="store_true", help="write configuration words that look wrong")
            p.add_argument("--no-readback", action="store_true", help="skip the final read-back comparison")
        p.set_defaults(func=func)
    p = sub.add_parser("read", help="save the whole PIC as Intel HEX")
    p.add_argument("out")
    p.set_defaults(func=cmd_read)
    p = sub.add_parser("bootloader", help="only a bootloader, then a full read-back: bp3mitloader, or the "
                                          "built-in image's ds30 bootloader")
    p.add_argument("which", nargs="?", choices=BOOTLOADERS, default="mit",
                   help="mit: bp3mitloader (the default); ds30: the ds30 bootloader v4.5 of the built-in image")
    p.add_argument("--no-readback", action="store_true", help="skip the final read-back comparison")
    p.set_defaults(func=cmd_bootloader)
    for name, text in (("jumper", "PGC held low like the PGC-PGD jumper: bootloader mode for buzzloader"),
                       ("id", "device ID, revision, configuration and calibration words"),
                       ("blank", "check that the PIC is erased"), ("erase", "erase the PIC"),
                       ("status", "the Pico's wiring check and last result"),
                       ("log", "everything the last operation printed"),
                       ("release", "release the lines and let the Bus Pirate run")):
        sub.add_parser(name, help=text).set_defaults(func=cmd_passthrough)
    p = sub.add_parser("run", help="any command of the Pico's console")
    p.add_argument("command")
    p.set_defaults(func=cmd_run)
    sub.add_parser("term", help="interactive console").set_defaults(func=cmd_term)
    p = sub.add_parser("install", help="copy the firmware to the Pico (reboots it into BOOTSEL by itself)")
    p.add_argument("uf2", help="a .uf2 file, or a folder with one per chip")
    p.set_defaults(func=cmd_install)
    p = sub.add_parser("bp", help="talk to the Bus Pirate's own serial port")
    p.add_argument("dev", help="the Bus Pirate's serial port")
    p.add_argument("what", choices=("info", "hello", "boot", "raw"))
    p.add_argument("text", nargs="?", help="for raw: what to send")
    p.set_defaults(func=cmd_bp)

    args = ap.parse_args(argv)
    if argv is None and sys.stderr and sys.stderr.isatty():  # a person at a terminal (pythonw has no stderr)
        print(BANNER, file=sys.stderr)
    # For bp, main() checks pyserial before anything else. install checks it after its file, and the commands
    # that open the Pico check it in with_pico(), so a bad FILE or usage is exit 2 even without pyserial.
    if args.func is cmd_bp:
        need_serial()
    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 1
    except (OSError, HexError, PicoError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
