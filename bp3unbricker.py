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
Raspberry Pi Pico or Pico 2 running the bp3unbricker firmware, and holds the
tools to build, install and test that firmware. Windows, Linux and macOS;
Python 3.8+ and pyserial (pip install pyserial). README.md explains everything.

Programming the Bus Pirate (through the Pico):
  info FILE.hex         check a .hex file (no Pico needed)
  flash [FILE.hex]      erase, program, verify every word, read back, run;
                        without FILE: the image built into the Pico
  verify [FILE.hex]     compare the whole PIC with FILE (or the built-in image)
  read OUT.hex          save the whole PIC as Intel HEX
  bootloader            only the bootloader of the built-in image, read back
  jumper                PGC held low like the PGC-PGD jumper: the bootloader
                        waits for buzzloader until 'release'
  id | blank | erase | status | log | release
  run "COMMAND"         any command of the Pico's console
  term                  interactive console (Ctrl+C to quit)
  install [UF2|DIR]     copy the firmware to the Pico (dist/ by default)
  bp PORT info|hello|boot|raw TEXT   talk to the Bus Pirate's own port

Development:
  build                 build both UF2 files into dist/ (Pico SDK)
  hex2c FILE.hex        make FILE.hex the image embedded in bp3unbricker.c
  make-test-image OUT   full-flash test image (random, zeros or ones)
  decode CAPTURE.csv    decode a Saleae capture of the ICSP lines
  sniff / sniff-analog  capture the ICSP lines with a Saleae Logic 2
  pgd-levels DIR        PGD at every latched bit of an analog capture

Tests:
  test-sim              the firmware against a PIC24 simulator (host gcc)
  test-hex              the checks of this program that need no hardware
  test-cli [NAME...]    every command against emulated hardware (no hardware)
  test-loader           upload protocol fault injection (Pico)
  test-bootloader       'bootloader' + buzzloader, every firmware .hex (Pico + Bus Pirate)
  soak                  long run of everything (Pico + Bus Pirate)

Exit status: 0 success, 1 the operation failed, 2 bad file or usage.
"""
import argparse
import collections
import contextlib
import datetime
import glob
import hashlib
import io
import json
import os
import random
import re
import shutil
import string
import struct
import subprocess
import sys
import tempfile
import threading
import time
import types
import warnings
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None

VERSION = "1.3.2"  # CMakeLists.txt's BP3_VERSION must be the same: build and test-hex check it
ROOT = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(ROOT, "bp3unbricker.c")
DIST = os.path.join(ROOT, "dist")
LOGS = os.path.join(ROOT, "logs")

FLASH_WORDS = 22016             # program flash, PC 0x000000..0x00ABFE
ROW_WORDS = 64
ROWS = FLASH_WORDS // ROW_WORDS  # 344
PAGE_WORDS = 8 * ROW_WORDS      # 512: an erase page, what buzzloader writes at a time
BLANK = 0xFFFFFF
# The Bus Pirate v3 flash (buzzpirat), in address order:
RESET_WORDS = 2                            # reset GOTO 0x00A800
USRAPP_INDEX = 0xA7FC // 2                 # the bootloader's GOTO to the firmware
BL_PAGE_INDEX = 0xA800 // 2                # bootloader page, up to CW1
BL_PAGE = BL_PAGE_INDEX // PAGE_WORDS      # 42: the bootloader refuses to erase its own page
BL_JUMP_INDEX = 0xABF8 // 2                # bljump: a BRA into the bootloader, for older firmware
BL_VERSION_INDEX = 0xABFA // 2             # version word: 0x0405 is v4.5
CW2_INDEX = 0xABFC // 2
CW1_INDEX = 0xABFE // 2
BL_CODE = range(BL_PAGE_INDEX, CW2_INDEX)  # the bootloader page without CW2/CW1: 0x00A800-0x00ABFB
GOTO_BOOTLOADER = [0x04A800, 0x000000]     # the reset GOTO 0x00A800 (a list: compared with word lists)
BP3_CW2, BP3_CW1 = 0x00F9DF, 0x003F7F      # the Bus Pirate v3 firmware's configuration words


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

EOF_RECORD = ":00000001FF"


def hex_record(addr, data, rtype=0):
    """One Intel HEX record line: data at the low 16 bits of the byte address addr (a base_record()
    before it gives the upper 16), then its checksum."""
    rec = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, rtype]) + bytes(data)
    return ":" + (rec + bytes([(-sum(rec)) & 0xFF])).hex().upper()


def base_record(upper):
    """Extended linear address record: the upper 16 bits of the byte addresses that follow."""
    return hex_record(0, [upper >> 8, upper & 0xFF], 4)


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


def write_hex(path, words):
    """{word index: 24-bit word} as an Intel HEX file in the MPLAB layout."""
    lines, upper = [], None
    indexes = sorted(words)
    start = 0
    while start < len(indexes):
        end = start + 1  # up to 4 consecutive words per record, as MPLAB writes them
        while end < len(indexes) and end - start < 4 and indexes[end] == indexes[end - 1] + 1:
            end += 1
        chunk, start = indexes[start:end], end
        byte_addr = chunk[0] * 4
        if byte_addr >> 16 != upper:
            upper = byte_addr >> 16
            lines.append(base_record(upper))
        lines.append(hex_record(byte_addr, b"".join(words[k].to_bytes(3, "little") + b"\0" for k in chunk)))
    lines.append(EOF_RECORD)
    write_lines(path, lines)


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
    """.hex -> (Image, notes, problems). config: 'file' (required in the file),
    'add' (the Bus Pirate v3 values when the file has none), 'none' (leave
    them erased) or 'info' (just say whether they are there)."""
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
    """The ds30 bootloader of a Bus Pirate v3 image alone, as the Pico's 'bootloader' writes it
    (image_word() of a bootloader_only image in bp3unbricker.c): the reset GOTO and the page
    0x00A800..0x00ABFE, every other word erased."""
    words = [w if i < RESET_WORDS or i >= BL_PAGE_INDEX else BLANK for i, w in enumerate(image.words)]
    blver = image.words[BL_VERSION_INDEX]
    return Image(f"bootloader v{blver >> 8}.{blver & 0xFF} of {image.name}", words)


def bootloader_problems(words):
    """Why words does not hold the ds30 bootloader where buzzloader and the firmware expect it."""
    p = []
    if words[:RESET_WORDS] != GOTO_BOOTLOADER:
        p.append(f"reset is not GOTO 0x00A800 ({words[0]:06X} {words[1]:06X})")
    if words[BL_PAGE_INDEX] == BLANK:
        p.append("nothing at 0x00A800")
    if words[BL_JUMP_INDEX] >> 16 != 0x37:
        p.append(f"no BRA (bljump) at 0x00ABF8 ({words[BL_JUMP_INDEX]:06X})")
    if words[BL_VERSION_INDEX] >> 8 != 0x04:
        p.append(f"no v4.x version word at 0x00ABFA ({words[BL_VERSION_INDEX]:06X})")
    if words[CW2_INDEX] == BLANK or words[CW1_INDEX] == BLANK:
        p.append("configuration words erased")
    return p


def describe(path, image, notes, problems):
    """What info, flash, verify and hex2c print about FILE before they use it."""
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


def image_section(path, image):
    """Section 12 of bp3unbricker.c for this image: its bytes once, the bootloader as a view."""
    bl = bootloader_alone(image)
    lines = [IMAGE_BEGIN,
             f"/* Written by `python bp3unbricker.py hex2c` from {image.name}",
             f" * (sha256 {file_sha256(path)}). Do not edit: run hex2c again. */",
             "static const uint8_t image_data[IMAGE_BYTES] = {"]
    for i in range(0, len(image.packed), 24):
        lines.append(",".join(str(b) for b in image.packed[i:i + 24]) + ",")
    lines += ["};", "",
              "const image_t embedded_image = {",
              f'    .name = "{image.name}",',
              "    .data = image_data,",
              f"    .crc32 = 0x{image.crc32:08X}u,",
              f"    .used_rows = {image.used_rows}u,",
              "};", "",
              "/* Its bootloader alone: the same bytes, every other word read as erased. */",
              "const image_t bootloader_image = {",
              f'    .name = "{bl.name}",',
              "    .data = image_data,",
              f"    .crc32 = 0x{bl.crc32:08X}u,",
              f"    .used_rows = {bl.used_rows}u,",
              "    .bootloader_only = true,",
              "};",
              IMAGE_END]
    return "\n".join(lines)


def embedded_from_source():
    """(image, its bootloader alone) from section 12 of bp3unbricker.c, CRCs checked."""
    with open(SOURCE, encoding="utf-8") as f:
        text = f.read()
    try:
        section = text[text.index(IMAGE_BEGIN):text.index(IMAGE_END)]
    except ValueError:
        raise HexError(f"{SOURCE}: no generated image section")
    data = bytes(int(v) for v in re.findall(r"\d+", section.split("{", 1)[1].split("}", 1)[0]))
    names = re.findall(r'\.name = "([^"]+)"', section)
    crcs = [int(v, 16) for v in re.findall(r"\.crc32 = 0x([0-9A-F]{8})u", section)]
    if len(data) != FLASH_WORDS * 3 or len(names) != 2 or len(crcs) != 2:
        raise HexError(f"{SOURCE}: the image section is damaged")
    image = Image(names[0], [int.from_bytes(data[3 * i:3 * i + 3], "little") for i in range(FLASH_WORDS)])
    bl = bootloader_alone(image)
    if image.crc32 != crcs[0] or bl.crc32 != crcs[1] or bl.name != names[1]:
        raise HexError(f"{SOURCE}: the image section does not match its CRCs")
    return image, bl


def cmd_hex2c(args):
    """Make FILE.hex the image embedded in the firmware (section 12 of bp3unbricker.c)."""
    name = os.path.basename(args.file)  # the image name: a C string literal in section 12, as it is
    # ?? starts a trigraph, which -std=c11 (test-sim) turns into another character
    if not (name.isascii() and name.isprintable()) or '"' in name or "\\" in name or "??" in name:
        print(f"ERROR: rename {name!r}: the image name in bp3unbricker.c must be printable ASCII "
              "without \", \\ or ??", file=sys.stderr)
        return 2
    try:
        image, notes, problems = load_image(args.file, "file")
    except (OSError, HexError) as e:
        print(f"{args.file}: {e}", file=sys.stderr)
        return 2
    describe(args.file, image, notes, problems)
    bad = bootloader_problems(image.words) + problems
    if bad:
        print("refused, no Bus Pirate v3 ds30 bootloader + firmware image: " + "; ".join(bad), file=sys.stderr)
        return 2
    try:
        with open(args.source, encoding="utf-8") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError) as e:  # missing, a folder, not UTF-8 text
        print(f"{args.source}: {e}", file=sys.stderr)
        return 2
    start, end = text.find(IMAGE_BEGIN), text.find(IMAGE_END)
    if start < 0 or end < start:
        print(f"{args.source}: no '{IMAGE_BEGIN}' ... '{IMAGE_END}' section", file=sys.stderr)
        return 2
    text = text[:start] + image_section(args.file, image) + text[end + len(IMAGE_END):]
    with open(args.source, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    bl = bootloader_alone(image)
    print(f"wrote {args.source}: {image.name} crc32 {image.crc32:08X}, {image.used_rows} rows; "
          f"{bl.name}: {bl.used_words} words, {bl.used_rows} rows, crc32 {bl.crc32:08X}")
    return 0


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
    """Open the Pico, apply --speed, run action(pico) -> bool and return the exit status. An error
    of the action is reported here, before the speed is restored; main() reports one while opening.
    pyserial is checked here, after the command has checked its arguments and FILE."""
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


def flash_bootloader(pico, readback):
    """The Pico's 'bootloader', checked against the CRCs its 'status' gives and, with readback,
    against the image section of bp3unbricker.c and a full read of the PIC."""
    _, lines = pico.run("status", show=False, needs_result=False)
    status = "\n".join(lines)
    pico_image = re.search(r", image .+ crc32 ([0-9A-F]{8}) \(OK", status)  # the name may hold spaces
    pico_bl = re.search(r"^Bootloader alone: .*, crc32 ([0-9A-F]{8}) \(OK", status, re.M)
    if not pico_image or not pico_bl:
        print("ERROR: the Pico's firmware has no usable 'bootloader' command: update it (install)")
        return False
    bl_crc = int(pico_bl.group(1), 16)
    if readback:
        try:
            embedded, bl = embedded_from_source()
        except (OSError, HexError) as e:
            print(f"ERROR: {e}\n  The result cannot be checked here (--no-readback skips that).")
            return False
        if embedded.crc32 != int(pico_image.group(1), 16) or bl.crc32 != bl_crc:
            print(f"ERROR: {SOURCE} does not hold the image built into the Pico "
                  f"(crc32 {embedded.crc32:08X}, the Pico's is {pico_image.group(1)})")
            return False
    ok, lines = pico.run("bootloader")
    if not ok:
        return False
    if written_crc32(lines) != bl_crc:
        print(f"ERROR: the Pico did not write the bootloader image (crc32 {bl_crc:08X})")
        return False
    if not readback:
        return True
    print("Reading the whole PIC back to compare it with the bootloader of the built-in image...")
    if not check_readback(pico, bl):
        return False
    n = bl.used_words
    print(f"Only the bootloader is on the PIC: {n} words, the other {FLASH_WORDS - n} erased.\n"
          "To load a firmware: python bp3unbricker.py jumper, then buzzloader --dev=<the Bus Pirate's port>\n"
          "--hex=<firmware.hex>, then python bp3unbricker.py release")
    return True


def cmd_bootloader(args):
    return with_pico(args, lambda pico: flash_bootloader(pico, readback=not args.no_readback))


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
    # A newline would end it and the console would run the rest too; to an empty line it answers nothing, not
    # even a prompt.
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


def bp_firmware_line(port):
    """The firmware's version line ('' if no firmware answers)."""
    reply = bp_talk(port, "info").decode("latin-1")
    return next((ln.strip() for ln in reply.splitlines() if "irmware" in ln), "")  # Firmware or firmware


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


# ======================================================================= UF2: build and install

UF2_MAGIC = (0x0A324655, 0x9E5D5157)
UF2_FLAG_FAMILY_ID = 0x00002000
# UF2 family IDs, named after the Pico SDK's boot/uf2.h
RP2040_FAMILY = 0xE48BFF56
ABSOLUTE_FAMILY = 0xE48BFF57  # picotool's RP2350-E10 workaround block
RP2350_ARM_S_FAMILY = 0xE48BFF59
RP2350_RISCV_FAMILY = 0xE48BFF5A
RP2350_ARM_NS_FAMILY = 0xE48BFF5B
# Code families each boot ROM runs (RP2350 also takes 'absolute' and 'data' blocks).
CHIP_FAMILIES = {
    "RP2040": {RP2040_FAMILY},
    "RP2350": {RP2350_ARM_S_FAMILY, RP2350_RISCV_FAMILY, RP2350_ARM_NS_FAMILY},
}
BOOT_DRIVE_CHIPS = {"RPI-RP2": "RP2040", "RP2350": "RP2350"}  # INFO_UF2.TXT "Board-ID" -> chip
BUILD_BOARDS = {"pico": RP2040_FAMILY, "pico2": RP2350_ARM_S_FAMILY}  # PICO_BOARD -> UF2 family of its code


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


def sdk_tools_env():
    """os.environ with the newest cmake and ninja of the Raspberry Pi Pico VS Code extension
    (~/.pico-sdk), when it has them, first in PATH."""
    tool_dirs = []
    sdk = os.path.join(os.path.expanduser("~"), ".pico-sdk")
    for pattern in ("cmake/*/bin", "ninja/*"):
        found = glob.glob(os.path.join(sdk, pattern))
        if found:  # by version number, not by name: v4.10.0 after v4.3.4 (the paths differ only there)
            tool_dirs.append(max(found, key=lambda path: [int(n) for n in re.findall(r"\d+", path)]))
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(tool_dirs + [env.get("PATH", "")])
    return env


def bp3_version():
    """BP3_VERSION in the CMakeLists.txt next to this program: the firmware's version (None if it sets none).
    Quoted or not, it is the same value for CMake."""
    with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r'^set\(BP3_VERSION "?([^")\s]+)"?\)', f.read(), re.M)
    return m.group(1) if m else None


def cmd_build(args):
    """Check that CMakeLists.txt gives the firmware this program's VERSION; build the UF2 for each board and
    check its families; only then copy both into dist/ and write SHA256SUMS.txt, so that a failed build
    leaves dist/ as it was."""
    firmware_version = bp3_version()
    if firmware_version != VERSION:
        found = f"BP3_VERSION {firmware_version}" if firmware_version else "no BP3_VERSION"
        print(f"ERROR: bp3unbricker.py is {VERSION} but CMakeLists.txt has {found}: make them the same",
              file=sys.stderr)
        return 1
    env = sdk_tools_env()
    # Windows looks the program up in our own PATH, not in env's: resolve it here.
    cmake = shutil.which("cmake", path=env["PATH"])
    if not cmake or not shutil.which("ninja", path=env["PATH"]):
        print("cmake or ninja not found (install the Raspberry Pi Pico VS Code extension)", file=sys.stderr)
        return 1
    built = []
    for board, family in BUILD_BOARDS.items():
        build_dir = os.path.join(ROOT, f"build-{board}")
        subprocess.run([cmake, "-G", "Ninja", "-S", ROOT, "-B", build_dir, f"-DPICO_BOARD={board}"],
                       check=True, env=env)
        subprocess.run([cmake, "--build", build_dir], check=True, env=env)
        name = f"bp3unbricker-{board}.uf2"
        uf2 = os.path.join(build_dir, name)
        families = uf2_families(uf2)
        if set(families) - {family, ABSOLUTE_FAMILY}:
            print(f"{uf2}: unexpected UF2 families {[hex(x) for x in families]}", file=sys.stderr)
            return 1
        if family not in families:
            print(f"{uf2}: no blocks for family {family:#x}", file=sys.stderr)
            return 1
        built.append((name, uf2, families[family]))
    os.makedirs(DIST, exist_ok=True)
    sums = []
    for name, uf2, blocks in built:
        out = os.path.join(DIST, name)
        shutil.copyfile(uf2, out)
        digest = file_sha256(out)
        sums.append(f"{digest}  {name}")
        print(f"{out}: {os.path.getsize(out)} bytes, {blocks} blocks, sha256 {digest}")
    write_lines(os.path.join(DIST, "SHA256SUMS.txt"), sums)
    return 0


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
    matches the chip of the boot drive is used."""
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
        matches = [f for f in sorted(glob.glob(os.path.join(path, "*.uf2"))) if chip in uf2_chips(f)]
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


# ======================================================================= test images

def safe_cw1(v):
    """CW1 that keeps the chip readable and reprogrammable, as safe_cw1() in bp3unbricker.c:
    r15=0, JTAGEN=0, GCP=1, GWRP=1, DEBUG=1, r10=1, ICS=11 (PGC1/PGD1),
    WINDIS=1 (WINDIS=0 needs FWDTEN=1), r5=1."""
    v |= (1 << 13) | (1 << 12) | (1 << 11) | (1 << 10) | (3 << 8) | (1 << 6) | (1 << 5)
    v &= ~((1 << 15) | (1 << 14))
    return 0xFF0000 | (v & 0xFFFF)


def safe_cw2(v):
    """CW2 with WUTSEL/SOSCSEL never x0, r3=1 and FNOSC never 110 (all reserved), as safe_cw2()
    in bp3unbricker.c."""
    v |= (1 << 13) | (1 << 11) | (1 << 3)
    if (v >> 8) & 7 == 6:
        v &= ~(1 << 9)  # FNOSC 110 -> 100 (SOSC)
    return 0xFF0000 | (v & 0xFFFF)


def cmd_make_test_image(args):
    """A full-flash test image: every word random (or zeros, or ones), bootloader page
    included. The configuration words of 'random' and 'zeros' are constrained so that the chip
    stays readable over ICSP and flash takes the image; 'ones' is an erased chip, configuration
    words included (JTAGEN=1), and says that flash needs --force for it."""
    rng = random.Random(args.seed)
    if args.pattern == "random":
        words = {i: rng.getrandbits(24) for i in range(FLASH_WORDS)}
        cw2, cw1 = rng.getrandbits(16), rng.getrandbits(16)
    else:
        words = dict.fromkeys(range(FLASH_WORDS), 0 if args.pattern == "zeros" else BLANK)
        cw2 = cw1 = 0
    if args.pattern != "ones":  # 'ones' is an erased chip, configuration words included
        words[CW2_INDEX], words[CW1_INDEX] = safe_cw2(cw2), safe_cw1(cw1)
    write_hex(args.out, words)
    print(f"{args.out}: {len(words)} words, CW2=0x{words[CW2_INDEX]:06X} CW1=0x{words[CW1_INDEX]:06X}")
    if args.pattern == "ones":
        print("flash needs --force for this image (erased CW1: JTAGEN=1)")
    return 0


def safe_random_image(seed):
    """Random words on every row, but a safe start (GOTO 0x200, then BRA $ at 0x200) and the
    Bus Pirate v3 configuration words: the PIC never runs random code between soak rounds."""
    rng = random.Random(seed)
    words = [rng.getrandbits(24) for _ in range(FLASH_WORDS)]
    words[0], words[1] = 0x040200, 0x000000
    words[0x200 // 2], words[0x200 // 2 + 1] = 0x37FFFF, 0x000000  # BRA $, then a NOP
    words[CW2_INDEX], words[CW1_INDEX] = BP3_CW2, BP3_CW1
    return Image(f"soak-random-{seed:08X}", words)


# ======================================================================= Saleae captures and the ICSP decoder

SALEAE_CHANNEL_MAP = "0=MCLR,1=VDD,2=PGD,3=PGC"  # --map by default: the channels of the simulator's trace
ICSP_LINES = ("MCLR", "VDD", "PGD", "PGC")       # the lines --map places; all of them but VDD, it must
SALEAE_PORT = 10430                              # the Logic 2 automation server
SALEAE_THRESHOLD_V = 3.3
SALEAE_BUFFER_MB = 8192


def need_numpy():
    try:
        import numpy
    except ImportError:
        sys.exit("numpy is missing: pip install numpy")
    return numpy


def tcp_port(text):
    """--saleae-port: a TCP port, 1 to 65535 (anything else is a usage error)."""
    port = int(text)
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError(f"{text} is not a TCP port (1 to 65535)")
    return port


def need_saleae():
    """The Logic 2 automation API, and the errors a Logic 2 session can end in: the API's own, and the
    gRPC ones it passes on as they are (Logic 2 not answering on the port, for one)."""
    try:
        from saleae import automation
        import grpc  # the API's transport, installed with it
    except ImportError:
        sys.exit("the Logic 2 automation API is missing: pip install logic2-automation")
    return automation, (automation.errors.SaleaeError, grpc.RpcError)


def sniff_command(args):
    """The command after '--' that sniff and sniff-analog run during the capture, in the current
    folder; None, said on stderr, if there is none or its program cannot be found (refused before
    the capture starts)."""
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    if not cmd:
        print("ERROR: missing command after --", file=sys.stderr)
        return None
    # Windows also starts a program that sits next to this Python: CreateProcess looks there before PATH
    beside_python = os.name == "nt" and shutil.which(cmd[0], path=os.path.dirname(sys.executable))
    if not (shutil.which(cmd[0]) or beside_python):
        print(f"ERROR: the command after -- cannot run: '{cmd[0]}' not found", file=sys.stderr)
        return None
    return cmd


def sniff_lines(args):
    """sniff's and sniff-analog's --map: {line: channel}; None, said on stderr, for a wrong one (refused
    before the capture starts)."""
    try:
        return channel_numbers(args.map)
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return None


def save_command_output(out, stdout, stderr):
    """Save what the captured command printed (cmd_stdout.txt, cmd_stderr.txt in out) and show the end of each."""
    for name, text in (("cmd_stdout.txt", stdout), ("cmd_stderr.txt", stderr)):
        with open(os.path.join(out, name), "w", encoding="utf-8") as f:
            f.write(text)
    print(stdout[-3000:])
    print(stderr[-2000:], file=sys.stderr)


def logic2_failed(automation, error, port):
    """Report an error of a Logic 2 session (Logic 2 refused the capture, the capture failed, or nothing
    answers on PORT) in one ERROR line, the first sentence of its message: a refused sample rate goes on
    with a 6 KB list of the allowed ones. Returns the exit status: 2 when Logic 2 refused the options
    (InvalidRequestError), 1 otherwise."""
    if isinstance(error, automation.errors.SaleaeError):
        message = str(error).strip() or type(error).__name__  # IncompatibleApiVersionError, for one, has none
    elif error.code().name == "UNAVAILABLE":  # a gRPC error: Logic 2 is closed, or its automation server off
        message = f"no answer on port {port} (is it open, with its automation server on?)"
    else:
        message = error.code().name + (f": {error.details()}" if error.details() else "")
    print("ERROR: Logic 2: " + re.split(r"(?<=\.)\s|\n", message, maxsplit=1)[0], file=sys.stderr)
    return 2 if isinstance(error, automation.errors.InvalidRequestError) else 1


def cmd_sniff(args):
    """Capture the ICSP lines (on the channels --map gives) with a Saleae Logic 2 (automation server on,
    port 10430) while a command runs; export the digital transitions as CSV (and analog with --analog-rate).
    run.json records --map for decode."""
    cmd, names = sniff_command(args), sniff_lines(args)  # before the Saleae API: exit 2 even without it
    if not (cmd and names):
        return 2
    automation, logic2_errors = need_saleae()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    channels = sorted(names.values())
    analog = channels if args.analog_rate else []
    device_cfg = automation.LogicDeviceConfiguration(
        enabled_digital_channels=channels, digital_sample_rate=args.rate,
        digital_threshold_volts=args.threshold, enabled_analog_channels=analog,
        analog_sample_rate=args.analog_rate or None)
    capture_cfg = automation.CaptureConfiguration(buffer_size_megabytes=args.buffer_mb,
                                                  capture_mode=automation.ManualCaptureMode())
    log = {"cmd": cmd, "map": args.map, "rate": args.rate, "analog_rate": args.analog_rate}
    try:
        with automation.Manager.connect(port=args.saleae_port) as manager:
            with manager.start_capture(device_configuration=device_cfg, capture_configuration=capture_cfg) as capture:
                time.sleep(args.pre)
                t0 = time.monotonic()
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                        errors="replace")
                # communicate(), not wait(): a command that prints more than the pipe holds would never finish
                try:
                    stdout, stderr = proc.communicate(timeout=args.max_seconds or None)
                except subprocess.TimeoutExpired:
                    stdout = None  # --max-seconds is over: the capture stops, the command runs on
                if not args.max_seconds:
                    time.sleep(args.post)
                try:
                    capture.stop()
                finally:  # also when the capture failed: the command is not left running
                    if stdout is None:
                        stdout, stderr = proc.communicate()  # the rest of its output, once it ends
                log["elapsed_s"] = round(time.monotonic() - t0, 3)
                capture.export_raw_data_csv(directory=out, digital_channels=channels, analog_channels=analog or None)
                capture.save_capture(filepath=os.path.join(out, "capture.sal"))
    except logic2_errors as e:
        return logic2_failed(automation, e, args.saleae_port)
    log["returncode"] = proc.returncode
    save_command_output(out, stdout, stderr)
    with open(os.path.join(out, "run.json"), "w", encoding="utf-8") as f:
        json.dump(log, f, indent=2)
    print(json.dumps(log))
    return proc.returncode


def cmd_sniff_analog(args):
    """Triggered mixed-signal capture: the ICSP lines (on the channels --map gives) in digital, and analog
    channels (MCLR's, PGD's and PGC's by default: what pgd-levels reads), armed on a rising MCLR; --after
    seconds kept after the trigger, exported in Saleae binary format. run.json records --map for pgd-levels."""
    cmd, names = sniff_command(args), sniff_lines(args)  # before the Saleae API: exit 2 even without it
    if not (cmd and names):
        return 2
    automation, logic2_errors = need_saleae()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    channels = sorted(names.values())
    analog = args.analog or sorted(names[line] for line in ("MCLR", "PGD", "PGC"))
    device_cfg = automation.LogicDeviceConfiguration(
        enabled_digital_channels=channels, digital_sample_rate=args.rate,
        digital_threshold_volts=SALEAE_THRESHOLD_V, enabled_analog_channels=analog,
        analog_sample_rate=args.analog_rate)
    # --after seconds are kept after the trigger, and 50 ms before it
    capture_cfg = automation.CaptureConfiguration(
        buffer_size_megabytes=SALEAE_BUFFER_MB,
        capture_mode=automation.DigitalTriggerCaptureMode(
            trigger_type=automation.DigitalTriggerType.RISING, trigger_channel_index=names["MCLR"],
            after_trigger_seconds=args.after, trim_data_seconds=args.after + 0.05))
    output = []  # what the command prints: stdout, stderr
    try:
        with automation.Manager.connect(port=args.saleae_port) as manager:
            with manager.start_capture(device_configuration=device_cfg, capture_configuration=capture_cfg) as capture:
                time.sleep(1.0)  # let the capture arm before MCLR rises
                # Started here, not in the thread: a command that cannot start ends the capture at once
                # instead of leaving wait() to wait for a trigger that never comes.
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                        errors="replace")
                reader = threading.Thread(target=lambda: output.extend(proc.communicate()))  # the pipes never fill up
                reader.start()
                try:
                    capture.wait()
                finally:  # also when the capture failed: the command is not left running
                    reader.join()
                capture.export_raw_data_binary(directory=out, digital_channels=channels, analog_channels=analog)
                capture.save_capture(filepath=os.path.join(out, "capture.sal"))
    except logic2_errors as e:
        return logic2_failed(automation, e, args.saleae_port)
    save_command_output(out, *output)
    with open(os.path.join(out, "run.json"), "w", encoding="utf-8") as f:
        json.dump({"cmd": cmd, "map": args.map, "rate": args.rate, "analog_rate": args.analog_rate, "analog": analog,
                   "returncode": proc.returncode}, f, indent=2)
    print(json.dumps({"returncode": proc.returncode, "files": sorted(os.listdir(out))}))
    return proc.returncode


# ICSP decoder for a Saleae digital CSV export (the lines on the channels --map gives; by
# default ch0=MCLR, ch1=VDD, ch2=PGD, ch3=PGC). Three levels: sessions (MCLR low -> 32-bit
# key -> MCLR high ... MCLR low); ICSP commands (SIX: 4-bit 0000 + 24-bit instruction;
# REGOUT: 4-bit 0001 + 8 idle clocks + 16 bits from the target; LSb first, sampled on PGC
# rising edges); and a symbolic emulation of the executed instructions that tracks where
# every byte that leaves the chip through VISI came from and what reaches the flash write
# latches, so a capture turns back into memory contents.

KEY_NAMES = {0x4D434851: "ICSP", 0x4D434850: "EICSP"}  # entry keys "MCHQ", "MCHP" (Enhanced ICSP)
FIRST_SIX_EXTRA = 5  # the first SIX after entry takes 5 extra clocks
SFR_TBLPAG, SFR_NVMCON, SFR_NVMKEY, SFR_VISI = 0x0032, 0x0760, 0x0766, 0x0784
SFR_NAMES = {SFR_TBLPAG: "TBLPAG", SFR_NVMCON: "NVMCON", SFR_NVMKEY: "NVMKEY", SFR_VISI: "VISI"}
ADDR_MODES = {0: "W{}", 1: "[W{}]", 2: "[W{}--]", 3: "[W{}++]", 4: "[--W{}]", 5: "[++W{}]"}  # 6, 7: unused


def read_digital_csv(path):
    """Time, the 0/1 level of every column and the channel of each column of a Saleae Logic 2 digital CSV export:
    "Channel N" in the header, or the columns in order from 0 when they have other names (channels renamed in
    Logic 2). ValueError for another file, an analog export (volts) among them."""
    np = need_numpy()
    try:
        import pandas as pd
    except ImportError:
        pd = None
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            columns = [re.fullmatch(r"Channel (\d+)", name.strip()) for name in f.readline().split(",")[1:]]
        if pd:
            data = pd.read_csv(path).to_numpy()
        else:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)  # "input contained no data": refused below
                data = np.loadtxt(path, delimiter=",", skiprows=1, ndmin=2)  # 2-D even with one row
        # empty, another number of columns than the header names, or levels other than 0 and 1
        if not len(data) or len(columns) != data.shape[1] - 1 or not np.isin(data[:, 1:], (0, 1)).all():
            raise ValueError
        channels = [int(m.group(1)) for m in columns] if all(columns) else list(range(len(columns)))
        return data[:, 0].astype(np.float64), data[:, 1:].astype(np.int8), channels
    except ValueError:  # text that is not comma-separated numbers, or no row of them
        raise ValueError(f"{path}: not a Saleae digital export") from None


def edges(levels):
    """Sample indexes where a 0/1 (or boolean) signal rises, and where it falls."""
    np = need_numpy()
    step = np.diff(levels.astype(np.int16))
    return np.flatnonzero(step == 1) + 1, np.flatnonzero(step == -1) + 1


def lsb_first(bits):
    """The number these 0/1 samples send, LSb first."""
    return sum(b << k for k, b in enumerate(bits.tolist()))


def find_sessions(t, mclr, pgc, pgd):
    """Each programming session: its key, the PGC rising edges while MCLR is high, and its samples from
    the key on (start_idx: MCLR falls for the key; end_idx: MCLR falls again)."""
    mclr_rises, mclr_falls = edges(mclr)
    pgc_rises = edges(pgc)[0]
    sessions = []
    for rise in mclr_rises:
        falls_before = mclr_falls[mclr_falls < rise]
        if not len(falls_before):
            continue
        key_clocks = pgc_rises[(pgc_rises > falls_before[-1]) & (pgc_rises < rise)]
        key = 0
        for e in key_clocks[-32:]:
            key = (key << 1) | int(pgd[e])  # the key goes MSb first
        falls_after = mclr_falls[mclr_falls > rise]
        end = falls_after[0] if len(falls_after) else len(t) - 1
        sessions.append({"mclr_rise_t": t[rise], "mclr_end_t": t[end], "key": key, "key_clocks": len(key_clocks),
                         "rise_idx": pgc_rises[(pgc_rises > rise) & (pgc_rises < end)],
                         "start_idx": falls_before[-1], "end_idx": end})
    return sessions


def icsp_commands(bits):
    """The framing of an ICSP session, bits = PGD at each PGC clock: (first clock, 4-bit code, the
    clock after the code) of every command while a whole code is left. The first command has
    FIRST_SIX_EXTRA extra clocks before its code; 24 clocks follow each code (SIX: the instruction;
    REGOUT: 8 idle clocks, then 16 bits from the PIC), unless the session ends first."""
    first, code_at = 0, FIRST_SIX_EXTRA
    while code_at + 4 <= len(bits):
        yield first, lsb_first(bits[code_at:code_at + 4]), code_at + 4
        first = code_at = code_at + 4 + 24


def parse_icsp(t, pgd, rise_idx):
    """Split the bit stream of an ICSP session into commands, (name, value, time) and for REGOUT
    the number of idle ones too; also returns the number of bits left over."""
    np = need_numpy()
    bits = pgd[rise_idx].astype(np.int64)
    n, i, cmds = len(bits), 0, []  # i: the first bit not read yet
    for first, code, i in icsp_commands(bits):
        when = t[rise_idx[first]]
        if first == 0 and bits[:FIRST_SIX_EXTRA].any():
            cmds.append(("BAD_EXTRA", int("".join(map(str, bits[:FIRST_SIX_EXTRA])), 2), when))
        if code > 1:
            cmds.append(("BAD", code, when))
            break
        if i + 24 > n:
            cmds.append(("TRUNC", n - i, when))
            break
        if code == 0:  # SIX: a 24-bit instruction
            cmds.append(("SIX", lsb_first(bits[i:i + 24]), when))
        else:          # REGOUT: 8 idle clocks, then 16 bits from the PIC
            cmds.append(("REGOUT", lsb_first(bits[i + 8:i + 24]), when, int(bits[i:i + 8].sum())))
        i += 24
    return cmds, n - i


def sfr_name(addr):
    return SFR_NAMES.get(addr, f"0x{addr:04X}")


def file_reg(op):
    """MOV Ws, f / MOV f, Wd: the data address f."""
    return ((op >> 4) & 0x7FFF) << 1


def bit_operands(op):
    """BSET/BCLR f, #bit: (f, bit)."""
    return ((op >> 1) & 0xFFF) << 1, ((op >> 13) & 0x7) << 1 | (op & 1)


def ws_operand(op):
    """MOV, TBLRD, TBLWT Ws, Wd: the source Ws as (addressing mode, register)."""
    return (op >> 4) & 7, op & 0xF


def wd_operand(op):
    """MOV, TBLRD, TBLWT Ws, Wd and CLR Wd: the destination Wd as (addressing mode, register)."""
    return (op >> 11) & 7, (op >> 7) & 0xF


def operand_text(mode, reg):
    return ADDR_MODES[mode].format(reg)


def disasm(op):
    """The instruction subset ICSP uses."""
    hi = op >> 16
    if op == 0x000000:
        return "NOP"
    if hi == 0x04:
        return f"GOTO 0x{op & 0xFFFE:04X}"
    if (op >> 20) == 0x2:
        return f"MOV #0x{(op >> 4) & 0xFFFF:04X}, W{op & 0xF}"
    if (op >> 19) == 0x11:  # 1000 1fff... MOV Ws, f
        return f"MOV W{op & 0xF}, {sfr_name(file_reg(op))}"
    if (op >> 19) == 0x10:  # 1000 0fff... MOV f, Wd
        return f"MOV {sfr_name(file_reg(op))}, W{op & 0xF}"
    if hi == 0xEB and not op & 0x8000 and (op & 0x7F) == 0:  # 1110 1011 0Bqq qddd d000 0000 (SETM: 1Bqq)
        mode, reg = wd_operand(op)
        if mode <= 5:
            return f"CLR{'.B' if op & 0x4000 else ''} {operand_text(mode, reg)}"
    if hi in (0xBA, 0xBB):
        (smode, sreg), (dmode, dreg) = ws_operand(op), wd_operand(op)
        if smode <= 5 and dmode <= 5:
            name = ("TBLRD" if hi == 0xBA else "TBLWT") + ("H" if op & 0x8000 else "L")
            b = ".B" if op & 0x4000 else ""
            return f"{name}{b} {operand_text(smode, sreg)}, {operand_text(dmode, dreg)}"
    if hi in (0xA8, 0xA9):
        f, bit = bit_operands(op)
        return f"{'BSET' if hi == 0xA8 else 'BCLR'} {sfr_name(f)}, #{bit}"
    if (op >> 19) == 0xF and ((op >> 15) & 0xF) == 0:  # 0111 1www wBhh hddd dggg ssss, no offset
        (smode, sreg), (dmode, dreg) = ws_operand(op), wd_operand(op)
        if smode <= 5 and dmode <= 5:
            b = ".B" if op & 0x4000 else ""
            return f"MOV{b} {operand_text(smode, sreg)}, {operand_text(dmode, dreg)}"
    return f"??? 0x{op:06X}"


class ProgramByte:
    """A data byte that holds a program memory byte, known only when it leaves through VISI.
    addr: 2 * PC + byte 0..3, the byte address of a .hex file."""
    __slots__ = ("addr",)

    def __init__(self, addr):
        self.addr = addr

    def __repr__(self):
        return f"<{self.addr // 4 * 2:06X}.{self.addr % 4}>"


class IcspEmulator:
    """Executes the ICSP instruction subset with byte-level provenance."""

    def __init__(self):
        self.ram = {}               # data byte address -> int or ProgramByte
        self.latches = {}           # program byte address (2 * PC + byte) -> value (TBLWT)
        self.reads = {}             # program byte address -> value seen via REGOUT
        self.flash_writes = []      # (NVMCON without WR, TBLPAG, last TBLWT address, {PC: 24-bit word})
        self.erases = []            # (NVMCON without WR, TBLPAG, last TBLWT address)
        self.last_tblwt_addr = None
        self.unknown = {}           # opcode "XXXXXX" -> count

    # data memory helpers (W0..W15 live at 0x0000..0x001F)
    def rd8(self, a):
        return self.ram.get(a, 0)

    def rd8_int(self, a):
        """A byte that holds a ProgramByte reads as 0."""
        v = self.rd8(a)
        return v if isinstance(v, int) else 0

    def wr8(self, a, v):
        self.ram[a] = v

    def rd16_int(self, a):
        return self.rd8_int(a) | (self.rd8_int(a + 1) << 8)

    def wr16(self, a, v):
        self.wr8(a, v & 0xFF)
        self.wr8(a + 1, (v >> 8) & 0xFF)

    def w(self, n):
        return self.rd16_int(2 * n)

    def set_w(self, n, v):
        self.wr16(2 * n, v & 0xFFFF)

    def ea(self, mode, reg, byte):
        """Effective address of an indirect operand, applying pre/post modifiers."""
        step = 1 if byte else 2
        val = self.w(reg)
        if mode == 1:
            return val
        if mode == 2:
            self.set_w(reg, val - step)
            return val
        if mode == 3:
            self.set_w(reg, val + step)
            return val
        if mode == 4:
            self.set_w(reg, val - step)
            return val - step
        if mode == 5:
            self.set_w(reg, val + step)
            return val + step
        raise ValueError(mode)

    def data_addr(self, mode, reg, byte):
        """Data memory address of a Ws/Wd operand: the register itself in mode 0."""
        return 2 * reg if mode == 0 else self.ea(mode, reg, byte)

    def program_addr(self, mode, reg, byte):
        """Program memory address TBLPAG:Wn of a table instruction's operand."""
        offset = self.ea(mode, reg, byte) if mode else self.w(reg)
        return (self.tblpag() << 16) | (offset & 0xFFFF)

    def tblpag(self):
        return self.rd16_int(SFR_TBLPAG) & 0xFF

    def execute(self, op):
        hi = op >> 16
        if op == 0 or hi == 0x04:   # NOP; GOTO (ICSP feeds its 2nd word as a NOP)
            return
        if (op >> 20) == 0x2:
            self.set_w(op & 0xF, (op >> 4) & 0xFFFF)
            return
        if (op >> 19) == 0x11:
            f, src = file_reg(op), 2 * (op & 0xF)
            self.wr8(f, self.rd8(src))
            self.wr8(f + 1, self.rd8(src + 1))
            return
        if (op >> 19) == 0x10:
            f, dst = file_reg(op), 2 * (op & 0xF)
            self.wr8(dst, self.rd8(f))
            self.wr8(dst + 1, self.rd8(f + 1))
            return
        if hi == 0xEB and not op & 0x8000 and (op & 0x7F) == 0:  # CLR (SETM has bit 15 set)
            (mode, reg), byte = wd_operand(op), bool(op & 0x4000)
            if mode <= 5:
                a = self.data_addr(mode, reg, byte)  # CLR.B Wn: only the low byte of Wn
                self.wr8(a, 0)
                if not byte:
                    self.wr8(a + 1, 0)
                return
        if hi in (0xBA, 0xBB):
            (smode, _), (dmode, _) = ws_operand(op), wd_operand(op)
            if smode <= 5 and dmode <= 5:
                self._table_op(op)
                return
        if hi in (0xA8, 0xA9):
            f, bit = bit_operands(op)
            v = self.rd16_int(f)
            v = v | (1 << bit) if hi == 0xA8 else v & ~(1 << bit)
            self.wr16(f, v)
            if hi == 0xA8 and f == SFR_NVMCON and bit == 15:
                self._nvm_start(v)
            return
        if (op >> 19) == 0xF and ((op >> 15) & 0xF) == 0:
            (smode, sreg), (dmode, dreg) = ws_operand(op), wd_operand(op)
            if smode <= 5 and dmode <= 5:
                byte = bool(op & 0x4000)
                src = self.data_addr(smode, sreg, byte)
                vals = [self.rd8(src)] if byte else [self.rd8(src), self.rd8(src + 1)]
                dst = self.data_addr(dmode, dreg, byte)
                for k, v in enumerate(vals):
                    self.wr8(dst + k, v)
                return
        key = f"{op:06X}"
        self.unknown[key] = self.unknown.get(key, 0) + 1

    def _table_op(self, op):
        """TBLRDL/TBLRDH/TBLWTL/TBLWTH{.B}: program memory at TBLPAG:Wn <-> data memory."""
        is_write = (op >> 16) == 0xBB
        high, byte = bool(op & 0x8000), bool(op & 0x4000)
        (smode, sreg), (dmode, dreg) = ws_operand(op), wd_operand(op)
        if is_write:  # data memory -> the write latches (the source operand is evaluated first)
            src = self.data_addr(smode, sreg, byte)
            pa = self.program_addr(dmode, dreg, byte)
        else:         # program memory -> data memory
            pa = self.program_addr(smode, sreg, byte)
            dst = self.data_addr(dmode, dreg, byte)
        first_lane = 2 if high else 0  # TBLRDH/TBLWTH: byte 2 and the phantom byte 3
        lanes = [first_lane + (pa & 1)] if byte else [first_lane, first_lane + 1]  # bytes of the word
        base = (pa & ~1) * 2  # byte index into a 4-bytes-per-word image
        if is_write:
            for k, lane in enumerate(lanes):
                self.latches[base + lane] = self.rd8_int(src + k)
            self.last_tblwt_addr = pa
        else:
            for k, lane in enumerate(lanes):
                self.wr8(dst + k, ProgramByte(base + lane))

    def _nvm_start(self, nvmcon):
        nvmop, pag = nvmcon & 0x7FFF, self.tblpag()
        if nvmop in (0x404F, 0x4042):    # chip erase, page erase
            self.erases.append((nvmop, pag, self.last_tblwt_addr))
        elif nvmop in (0x4001, 0x4003):  # row write, word write
            img = {a: w.get(0, 0xFF) | (w.get(1, 0xFF) << 8) | (w.get(2, 0xFF) << 16)
                   for a, w in sorted(bytes_by_pc(self.latches).items())}
            self.flash_writes.append((nvmop, pag, self.last_tblwt_addr, img))
        self.latches = {}

    def regout(self, val):
        """REGOUT clocked out VISI: attribute its two bytes to program memory."""
        for k in range(2):
            s = self.rd8(SFR_VISI + k)
            if isinstance(s, ProgramByte):
                self.reads[s.addr] = (val >> (8 * k)) & 0xFF


def bytes_by_pc(byte_values):
    """{PC address: {byte 0..3: value}} from {2 * PC + byte: value} (what a session read, or the
    write latches)."""
    words = {}
    for b, v in byte_values.items():
        words.setdefault((b // 4) * 2, {})[b % 4] = v
    return words


def channel_numbers(channel_map, source="--map"):
    """--map, like SALEAE_CHANNEL_MAP: {name: channel}. ValueError unless it places MCLR, PGD and PGC, and
    VDD if it likes, each on its own channel; its message names the map's source."""
    hint = f"(the default is {SALEAE_CHANNEL_MAP})"
    try:
        pairs = [(v.strip().upper(), int(k)) for k, v in (item.split("=") for item in channel_map.split(","))]
        if min(channel for _, channel in pairs) < 0:  # -1 would quietly be the capture's last channel
            raise ValueError
    except ValueError:  # an item without one '=', or a channel that is not a number (or is negative)
        raise ValueError(f"{source} {channel_map}: not a list of CHANNEL=NAME {hint}") from None
    names = dict(pairs)
    other = [name for name in names if name not in ICSP_LINES]
    if other:
        raise ValueError(f"{source} {channel_map}: {other[0]} is not one of {', '.join(ICSP_LINES)} {hint}")
    if len(names) < len(pairs) or len(set(names.values())) < len(pairs):
        raise ValueError(f"{source} {channel_map}: a line or a channel given twice {hint}")
    missing = [name for name in ICSP_LINES if name != "VDD" and name not in names]
    if missing:
        raise ValueError(f"{source} {channel_map}: no channel for {', '.join(missing)} {hint}")
    return names


def capture_map(folder):
    """The --map that sniff or sniff-analog captured FOLDER with (its run.json), or None."""
    try:
        with open(os.path.join(folder, "run.json"), encoding="utf-8") as f:
            recorded = json.load(f)
    except (OSError, ValueError):  # no run.json (a capture of an older version, say), or not JSON
        return None
    found = recorded.get("map") if isinstance(recorded, dict) else None  # sniff up to 1.3.1 recorded none
    return found if isinstance(found, str) else None


def chosen_map(given, folder):
    """decode's and pgd-levels' map: --map (given), else the one in the capture's run.json (in folder), else the
    default; and where it came from, which their messages name."""
    if given:
        return given, "--map"
    recorded = capture_map(folder)
    return (recorded, "the map in run.json") if recorded else (SALEAE_CHANNEL_MAP, "the default map")


def decode_capture(path, channel_map=SALEAE_CHANNEL_MAP, listing=0, tail=0, quiet=False, source="the default map"):
    """Decode a capture: (sessions, all reads, all flash writes, (t, pgc, pgd) for timing_report). source: where the
    map came from (chosen_map()); with the default one, a capture without VDD's channel is decoded without VDD."""
    names = channel_numbers(channel_map, source)  # before numpy is needed: a wrong --map is exit 2 even without it
    np = need_numpy()
    t, lv, channels = read_digital_csv(path)
    absent = [name for name in names if names[name] not in channels]
    if "VDD" in absent and source == "the default map":  # VDD is optional, and nobody asked for it
        del names["VDD"]
        absent.remove("VDD")
    if absent:
        raise ValueError(f"{source} {channel_map}: {path} has no channel {names[absent[0]]} ({absent[0]})")
    line = {name: lv[:, channels.index(channel)] for name, channel in names.items()}
    mclr, pgc, pgd = line["MCLR"], line["PGC"], line["PGD"]
    say = (lambda *a: None) if quiet else print
    vdd = f"VDD const={len(np.unique(line['VDD'])) == 1} level={line['VDD'][0]}" if "VDD" in line else "no VDD"
    say(f"rows={len(t)} span={t[-1]:.3f}s  {vdd}")
    sessions = find_sessions(t, mclr, pgc, pgd)
    all_reads, all_writes = {}, []
    for si, s in enumerate(sessions):
        kind = KEY_NAMES.get(s["key"], "???")
        s["kind"] = kind
        say(f"\n== session {si}: key=0x{s['key']:08X} ({kind}) key_clocks={s['key_clocks']} "
            f"MCLR high {s['mclr_rise_t']:.6f}..{s['mclr_end_t']:.6f} "
            f"({(s['mclr_end_t'] - s['mclr_rise_t']) * 1e3:.1f} ms) clocks={len(s['rise_idx'])}")
        if kind != "ICSP" or not len(s["rise_idx"]):
            continue
        cmds, left = parse_icsp(t, pgd, s["rise_idx"])
        counts = {}
        for c in cmds:
            counts[c[0]] = counts.get(c[0], 0) + 1
        s["commands"], s["leftover"] = counts, left
        say(f"   commands: {counts}  leftover bits: {left}")
        emu = IcspEmulator()
        for c in cmds:
            if c[0] == "SIX":
                emu.execute(c[1])
            elif c[0] == "REGOUT":
                emu.regout(c[1])
        if emu.unknown:
            say(f"   unknown opcodes: {emu.unknown}")
        if emu.erases:
            erases = [(hex(nvmop), hex(pag), hex(addr) if addr is not None else None)
                      for nvmop, pag, addr in emu.erases]
            say(f"   erases: {erases}")
        if emu.flash_writes:
            say(f"   flash writes: {len(emu.flash_writes)} (first rows: "
                f"{[hex(min(img)) for _, _, _, img in emu.flash_writes[:4]]})")
        words = bytes_by_pc(emu.reads)
        if words:
            say(f"   reads: {len(words)} words, PC 0x{min(words):06X}..0x{max(words):06X}")
        all_reads.update(emu.reads)
        all_writes.extend(emu.flash_writes)
        shown = cmds[:listing]
        if tail:
            shown = (shown + [("...",)] if shown else []) + cmds[-tail:]
        for c in shown:
            if c[0] == "SIX":
                say(f"   {c[2]:.7f} SIX    {c[1]:06X}  {disasm(c[1])}")
            elif c[0] == "REGOUT":
                say(f"   {c[2]:.7f} REGOUT {c[1]:04X}        (idle ones={c[3]})")
            elif c[0] == "...":
                say("   ...")
            else:
                say(f"   {c[2]:.7f} {c[0]} {c[1]}")
    return sessions, all_reads, all_writes, (t, pgc, pgd)


def timing_report(t, pgc, pgd, sessions):
    """PGC high and low times, and the PGD setup time before PGC rises, in ns, in the programming sessions (ICSP,
    or EICSP: a PICkit's programming executive) from their key on: outside them the lines are not ICSP (a released
    PGC floats while the PIC boots)."""
    np = need_numpy()
    inside = np.zeros(len(t), bool)
    for s in sessions:
        if s["kind"] != "???":
            inside[s["start_idx"]:s["end_idx"] + 1] = True
    rises, falls = (e[inside[e]] for e in edges(pgc))
    if len(rises) < 2 or len(falls) < 2:
        print("\nno PGC clock inside an ICSP or EICSP session: nothing to time")
        return

    def ns_to_next(events, targets):
        """Time from each event to the first target edge at or after it (events with none dropped)."""
        k = np.searchsorted(targets, events)
        ok = k < len(targets)
        return (t[targets[k[ok]]] - t[events[ok]]) * 1e9

    def spread(ns):
        return f"min {ns.min():.0f} p1 {np.percentile(ns, 1):.0f} median {np.median(ns):.0f}"

    high = ns_to_next(rises, falls)
    low = ns_to_next(falls, rises)
    low = low[low < 5000]  # ignore gaps between commands
    print(f"\nPGC high time ns: {spread(high)}")
    print(f"PGC low  time ns: {spread(low)} (within bursts)")
    pgd_changes = np.flatnonzero(np.diff(pgd.astype(np.int16))) + 1
    pgd_changes = pgd_changes[inside[pgd_changes]]
    setup = ns_to_next(pgd_changes[pgc[pgd_changes] == 0], rises)  # only where PGD changed while PGC was low
    setup = setup[setup < 5000]
    if len(setup):
        print(f"PGD change -> next PGC rise ns: {spread(setup)}")
    print(f"PGD changes while PGC high: {len(pgd_changes[pgc[pgd_changes] == 1])}")


def cmd_decode(args):
    channel_map, source = chosen_map(args.map, os.path.dirname(os.path.abspath(args.csv)))
    try:
        sessions, reads, writes, (t, pgc, pgd) = decode_capture(args.csv, channel_map, args.list, args.tail,
                                                                source=source)
    except (OSError, ValueError) as e:  # a wrong --map, or a capture that does not exist or is not a digital export
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    if all(s["kind"] == "???" for s in sessions):  # with PGD and PGC swapped, every key reads as garbage
        print(f"no ICSP session (an entry key after an MCLR pulse): are the lines where {source} says "
              f"({channel_map})?", file=sys.stderr)
    if args.dump_reads:
        words = bytes_by_pc(reads)
        with open(args.dump_reads, "w") as f:
            json.dump({f"{a:06X}": {str(k): v for k, v in sorted(b.items())} for a, b in sorted(words.items())}, f)
    if args.dump_writes:
        out = [{"nvmcon": hex(nvmop), "tblpag": pag, "addr": hex(addr) if addr is not None else None,
                "words": {f"{k:06X}": f"{v:06X}" for k, v in img.items()}} for nvmop, pag, addr, img in writes]
        with open(args.dump_writes, "w") as f:
            json.dump(out, f)
    if args.timing:
        timing_report(t, pgc, pgd, sessions)
    return 0


ANALOG_HEADER = struct.Struct("<8siidQQQ")  # '<SALEAE>', version, type, begin, sample rate, downsample, n


def read_analog(path):
    """Sample rate and volts of a Saleae Logic 2 binary export, analog_N.bin (format version 0):
    ANALOG_HEADER (type 1), then n float32 volts. ValueError for another file, an export cut short, or
    a header with a sample rate or downsample of 0."""
    np = need_numpy()
    with open(path, "rb") as f:
        header = f.read(ANALOG_HEADER.size)
    try:
        magic, _, kind, _, rate, downsample, n = ANALOG_HEADER.unpack(header)
    except struct.error:  # shorter than the header: an empty or cut file, a digital export without transitions
        magic = kind = None
    if magic != b"<SALEAE>" or kind != 1 or 0 in (rate, downsample) or os.path.getsize(path) < len(header) + 4 * n:
        raise ValueError(f"{path}: not a Saleae analog export")
    return rate / downsample, np.asarray(np.memmap(path, dtype="<f4", mode="r", offset=ANALOG_HEADER.size, shape=(n,)))


def latch_samples(rate):
    """40 ns in samples of an analog capture at rate, at least one (2 at 50 MS/s): pgd-levels reads each
    bit that long before PGC falls, and measures PGD that close to the edge where the bit is latched."""
    return max(1, round(40e-9 * rate))


def cmd_pgd_levels(args):
    """PGD level at every latched bit of a sniff-analog capture (analog MCLR, PGD and PGC, on the
    channels --map gives). Each PGC clock of the session is classified from the decoded commands:
      cmd   4-bit command       the Pico drives PGD, the PIC latches on PGC rising
      six   24 instruction bits  the Pico drives PGD, the PIC latches on PGC rising
      idle  8 REGOUT clocks      PGD changes hands, nobody samples it
      data  16 VISI bits         the PIC drives PGD, the Pico samples at the end of PGC high
    Only clocks where PGD keeps its level from the previous to the next bit are measured
    (otherwise the analog filter mixes the data edge in)."""
    channel_map, source = chosen_map(args.map, args.dir)
    try:
        names = channel_numbers(channel_map, source)
        rate, pgc = read_analog(os.path.join(args.dir, f"analog_{names['PGC']}.bin"))
        _, pgd = read_analog(os.path.join(args.dir, f"analog_{names['PGD']}.bin"))
        _, mclr = read_analog(os.path.join(args.dir, f"analog_{names['MCLR']}.bin"))
    except (OSError, ValueError) as e:  # a wrong --map, no such file, or not a whole analog export
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    ns_per_sample = 1e9 / rate
    offset = latch_samples(rate)
    threshold_v = 1.65  # VDD / 2
    mclr_rises, mclr_falls = edges(mclr > threshold_v)
    # the session runs from the second MCLR rise (the first is the entry pulse, the second follows the
    # key) to the next MCLR fall; without either, it is empty
    start = mclr_rises[1] if len(mclr_rises) > 1 else len(mclr)
    end = next(iter(mclr_falls[mclr_falls > start]), start)
    rises, falls = edges(pgc > threshold_v)
    rises = rises[(rises >= start) & (rises < end)]
    falls = falls[(falls > rises[0]) & (falls < end)] if len(rises) else falls[:0]
    n = min(len(rises), len(falls))

    def not_icsp(what):  # with PGD and PGC swapped, say, the clocks are PGD's changes
        print(f"ERROR: {args.dir}: {what}: are the lines where {source} says ({channel_map})?", file=sys.stderr)
        return 2

    if not n:  # fewer than two MCLR rises, MCLR still high where the capture ends, or no whole PGC clock
        return not_icsp("no complete ICSP session (entry pulse, key, PGC clocks, then MCLR low)")
    rises, falls = rises[:n], falls[:n]
    bits = pgd[falls - offset] > threshold_v  # 40 ns before PGC falls: what both sides use

    phase, k = [], 0  # k: the first clock not classified yet
    for first, code, k in icsp_commands(bits):
        phase += ["cmd"] * (k - first)
        if code == 0:
            phase += ["six"] * 24
        elif code == 1:
            phase += ["idle"] * 8 + ["data"] * 16
        else:
            return not_icsp(f"command {code} at clock {k} is neither SIX nor REGOUT")
        k += 24
    if not phase:  # every session starts with SIXes
        return not_icsp(f"{n} PGC clocks, not one whole command")
    leftover = f", {n - k} clocks left over" if k < n else ""
    print(f"{args.dir}: {n} clocks, {phase.count('six') // 24} SIX, {phase.count('data') // 16} REGOUT{leftover}")
    before_rise, after_fall = int(200 / ns_per_sample), int(300 / ns_per_sample)
    stats = {}
    for i in range(1, min(n - 1, len(phase))):  # every clock of a command, with a bit on each side
        if not bits[i - 1] == bits[i] == bits[i + 1]:
            continue
        bit = bool(bits[i])
        latch = pgd[falls[i] - offset:falls[i] + 1] if phase[i] == "data" else pgd[rises[i] - offset:rises[i] + offset]
        clock = pgd[rises[i] - before_rise:falls[i] + after_fall]
        at_latch, in_clock = stats.setdefault((phase[i], bit), ([], []))
        at_latch.append(latch.min() if bit else latch.max())
        in_clock.append(clock.min() if bit else clock.max())
    print(f"{'phase':6s} {'bit':3s} {'clocks':>6s}  {'worst at the latch':>18s}  {'worst in the clock':>18s}")
    for (p, bit), (at_latch, in_clock) in sorted(stats.items()):
        worst = min if bit else max
        print(f"{p:6s} {int(bit):3d} {len(at_latch):6d}  {worst(at_latch):16.2f} V  {worst(in_clock):16.2f} V")
    print("The PIC needs a 1 above VIH 2.64 V and a 0 below VIL 0.66 V (VDD 3.3 V).")
    return 0


# ======================================================================= tests

class Checks:
    """ok / FAIL lines and a failure count."""

    def __init__(self):
        self.failures = 0

    def __call__(self, cond, what, rec=None):
        print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
        if not cond:
            self.failures += 1
            if rec is not None:
                rec.setdefault("failed", []).append(what)

    def summary(self, what):
        print(f"\n{self.failures} {what} FAILED" if self.failures else f"\nALL {what} PASSED")
        return 1 if self.failures else 0


def host_cc():
    """$CC, else MSYS2 UCRT64 gcc on Windows, else gcc."""
    msys = r"C:\msys64\ucrt64\bin\gcc.exe"
    return os.environ.get("CC") or (msys if os.name == "nt" and os.path.exists(msys) else "gcc")


HOST_TESTS_TIMEOUT_S = 600  # the host tests take about 30 s: a run that goes on for this long is stuck


def blocked_file_runs(exe, work, since, env):
    """The host tests again, all at once, each with one file their run wrote in work (since: when it started)
    blocked by a folder of that name: each must stop where it first writes the file, with exit status 1 and one
    '  !! cannot write FILE: reason' line, before it shows a record it captured or a verdict. The console tests
    print records of their own (a dump): records count before their first title only. Returns the Checks."""
    check = Checks()
    print("\n==== the host tests again, each file they write blocked ====")
    runs = []
    for name in sorted(os.listdir(work)):
        path = os.path.join(work, name)
        if os.path.isfile(path) and os.path.getmtime(path) >= since - 1 and path != exe and name != "run.log":
            folder = os.path.join(work, "blocked-" + name)
            os.makedirs(os.path.join(folder, name), exist_ok=True)  # fopen(name, "w") fails on a folder
            with open(os.path.join(folder, "out.txt"), "w") as out, open(os.path.join(folder, "err.txt"), "w") as err:
                runs.append((name, folder, subprocess.Popen([exe], cwd=folder, env=env, stdout=out, stderr=err)))
    written = [name for name, _, _ in runs]
    check("sim_trace.csv" in written, f"the host tests wrote {', '.join(written) or 'nothing'}: sim_trace.csv, the "
                                      "decoder check's, among them")
    for name, folder, proc in runs:
        try:
            status = proc.wait(HOST_TESTS_TIMEOUT_S)
        except subprocess.TimeoutExpired:  # it did not stop at the file: stuck somewhere after it
            proc.kill()
            proc.wait()
            status = f"none, stopped after {HOST_TESTS_TIMEOUT_S} s"
        with open(os.path.join(folder, "out.txt"), errors="replace") as out, \
                open(os.path.join(folder, "err.txt"), errors="replace") as err:
            stdout, stderr = out.read(), err.read()
        said = [ln for ln in stderr.splitlines() if re.fullmatch(f"  !! cannot write {re.escape(name)}: .+", ln)]
        records = [ln for ln in stdout.split("\n==== console: ")[0].splitlines() if ln.startswith(":")]
        verdict = [ln for ln in stdout.splitlines() if "check(s) failed" in ln]
        stopped = status == 1 and len(said) == 1 and not records and not verdict
        check(stopped, f"{name} blocked: exit 1, one '  !! cannot write {name}: ...' line, no Intel HEX record, "
                       "no verdict")
        if not stopped:
            print(f"        exit {status}, {len(said)} such lines, {len(records)} records, verdict {verdict[-1:]}")
        shutil.rmtree(folder, ignore_errors=True)
    return check


def cmd_test_sim(args):
    """Compile bp3unbricker.c for the host (-DBP3_HOST_TEST: section 11, a pin-level
    PIC24FJ64GA002 simulator) and run every test case; then run them again with each file they
    wrote blocked (blocked_file_runs()). Then decode the simulator's trace with the ICSP decoder
    and check what it reconstructs (when numpy is there)."""
    cc = args.cc or host_cc()
    work = args.keep or tempfile.mkdtemp(prefix="bp3sim-")
    os.makedirs(work, exist_ok=True)
    exe = os.path.join(work, "bp3unbricker_test" + (".exe" if os.name == "nt" else ""))
    env = dict(os.environ)
    if os.path.isabs(cc):
        env["PATH"] = os.path.dirname(cc) + os.pathsep + env.get("PATH", "")  # its DLLs on Windows
    build = [cc, "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Wno-sign-conversion",
             "-Werror", "-DBP3_HOST_TEST", SOURCE, "-o", exe]
    print(" ".join(build), flush=True)
    if subprocess.run(build, env=env).returncode:
        return 1
    run_log = os.path.join(work, "run.log")
    started = time.time()
    try:
        sim = subprocess.run([exe], cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             timeout=HOST_TESTS_TIMEOUT_S)
    except subprocess.TimeoutExpired as e:  # its output so far: bytes on some systems, whatever text=True says
        stuck = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else e.stdout or ""
        with open(run_log, "w") as f:
            f.write(stuck)
        print(f"ERROR: the host tests did not finish within {HOST_TESTS_TIMEOUT_S} s: stuck after "
              f"{next((ln for ln in reversed(stuck.splitlines()) if ln.startswith('====')), 'their start')} "
              f"(output so far: {run_log})", file=sys.stderr)
        return 1
    with open(run_log, "w") as f:
        f.write(sim.stdout)
    # section titles, failed checks and the verdict ("FAILED: ..." lines are the firmware's own
    # messages, expected in the fault tests)
    print("\n".join(ln for ln in sim.stdout.splitlines() if ln.startswith(("====", "  !!")) or "check(s) failed" in ln))
    print(f"(full output: {run_log})")
    if sim.returncode:
        return 1
    check = blocked_file_runs(exe, work, started, env)
    if check.failures:
        return check.summary("BLOCKED-FILE CHECKS")
    try:
        import numpy  # noqa: F401
    except ImportError:
        check.summary("BLOCKED-FILE CHECKS")
        print("decoder check skipped: numpy is not installed")
        return 0
    print("\n==== the ICSP decoder on the simulator's trace ====")
    image, _ = embedded_from_source()
    sessions, reads, writes, _ = decode_capture(os.path.join(work, "sim_trace.csv"), quiet=True)
    icsp = [s for s in sessions if s.get("kind") == "ICSP"]
    check(len(icsp) == 1 and icsp[0].get("leftover") == 0 and set(icsp[0]["commands"]) == {"SIX", "REGOUT"},
          f"one ICSP session, only SIX and REGOUT: {[s.get('commands') for s in icsp]}")
    words = bytes_by_pc(reads)
    got = {a // 2: w[0] | (w[1] << 8) | (w[2] << 16) for a, w in words.items() if a < 0x10 and len(w) >= 3}
    check(got == {i: image.words[i] for i in range(8)}, "reads of PC 0x000000..0x00000E = the image")
    # what the simulator's trace test writes (bp3unbricker.c section 11): rows 0 and 1, and the
    # last row with the upper byte of CW2 and CW1 at 0xFF
    expected_rows = {r: image.row_words(r) for r in (0, 1, ROWS - 1)}
    expected_rows[ROWS - 1][-2:] = [image.words[CW2_INDEX] | 0xFF0000, image.words[CW1_INDEX] | 0xFF0000]
    decoded = {}
    for _, _, _, img in writes:  # img: {PC address: word} of one row write
        first = min(img)
        decoded[first // (2 * ROW_WORDS)] = [img.get(a, BLANK) for a in range(first, first + 2 * ROW_WORDS, 2)]
    check(decoded == expected_rows, f"row writes decoded: rows {sorted(decoded)} = what was written")
    return check.summary("DECODER CHECKS")


def run_main(*argv):
    """This program with argv, run by test-hex in this process: main()'s exit status (or what it raised,
    by name) and what it printed, as subprocess.run() gives them."""
    with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
        try:
            status = main(list(argv))
        except (Exception, SystemExit) as e:  # a FAIL line, not the end of test-hex
            status = f"{type(e).__name__}: {e}"
    return subprocess.CompletedProcess(argv, status, out.getvalue(), err.getvalue())


def check_version_and_banner(check, hex_path):
    """VERSION is the firmware's (CMakeLists.txt), build refuses another and --version prints it; the banner is the
    firmware's but for its second line, and only a person at a terminal sees it: once the command line is
    understood, before anything else; the three files carry the same license header. hex_path: a .hex file to run a
    harmless 'info' on."""
    from unittest import mock

    with open(SOURCE, encoding="utf-8") as f:
        header = f.read().split("*/", 1)[0].split("\n")[1:-1]  # the lines between the first "/*" and "*/"
    copies = []
    for path in (os.path.abspath(__file__), os.path.join(ROOT, "CMakeLists.txt")):
        with open(path, encoding="utf-8") as f:
            lines = f.read().split("\n")
        first = 1 if lines[0].startswith("#!") else 0
        copies.append(lines[first:first + len(header)])
    check(all(copy == [f"# {ln}" if ln else "#" for ln in header] for copy in copies),
          "the license header: bp3unbricker.py (after its shebang) and CMakeLists.txt carry bp3unbricker.c's, line for "
          "line")
    check(bp3_version() == VERSION, f"VERSION {VERSION} is CMakeLists.txt's BP3_VERSION, the firmware's")
    with tempfile.TemporaryDirectory() as tmp:
        cmake_lists, refusals = os.path.join(tmp, "CMakeLists.txt"), []
        # ROOT, where build reads CMakeLists.txt and makes its folders, and DIST in tmp: build touches nothing else
        with mock.patch.dict(globals(), ROOT=tmp, DIST=os.path.join(tmp, "dist")):
            write_lines(cmake_lists, [f'set(BP3_VERSION "{VERSION}")'])  # quoted: the same value for CMake
            quoted = bp3_version()
            for line, found in (("set(BP3_VERSION 1.3.1)", "BP3_VERSION 1.3.1"),
                                ("project(bp3unbricker)", "no BP3_VERSION")):
                write_lines(cmake_lists, [line])
                with mock.patch.object(subprocess, "run") as cmake:
                    r = run_main("build")
                refusals.append(r.returncode == 1 and not cmake.called and r.stderr == "ERROR: bp3unbricker.py is "
                                f"{VERSION} but CMakeLists.txt has {found}: make them the same\n")
    check(quoted == VERSION, f'BP3_VERSION may be quoted: set(BP3_VERSION "{VERSION}") is {VERSION} too')
    check(all(refusals), "build refuses another BP3_VERSION in CMakeLists.txt, or none, before it runs cmake (exit 1)")
    r = run_main("--version")  # argparse prints it, then raises SystemExit(0)
    check(r.returncode == "SystemExit: 0" and r.stdout == f"bp3unbricker.py {VERSION}\n" and not r.stderr,
          f"--version prints 'bp3unbricker.py {VERSION}' (exit 0)")

    lines = BANNER.split("\n")
    check(len(lines) == 7 and lines[1] == f" bp3unbricker.py {VERSION}" and BANNER.isascii()
          and max(len(ln) for ln in lines) <= 79,
          f"the banner: its 7 lines, ASCII, at most 79 columns, VERSION {VERSION} in the second")
    with open(SOURCE, encoding="utf-8") as f:  # print_banner() up to the end of its first printf
        code = f.read().split("static void print_banner(void) {", 1)[1].split(");", 1)[0]
    rule, *pieces = re.findall(r'"(.*?)"', code)  # the rule, then the printf's format in pieces
    firmware = [rule if ln == "%s" else ln for ln in "".join(pieces).split("\\n")[1:-1]]  # the lines it prints
    check(lines[:1] + lines[2:] == firmware[:1] + firmware[2:],
          "the banner's rules and project lines are the firmware's (print_banner() in bp3unbricker.c)")

    def from_command_line(terminal, *argv):
        """main() as `python bp3unbricker.py ARGV` runs it, stderr a terminal or not: (status, stdout, stderr)."""
        err = io.StringIO()
        err.isatty = lambda: terminal  # True: as sys.stderr is when a person runs this program
        with mock.patch.object(sys, "argv", ["bp3unbricker.py", *argv]), contextlib.redirect_stderr(err):
            with contextlib.redirect_stdout(io.StringIO()) as out:
                try:
                    status = main()
                except SystemExit as e:  # --help, --version, a usage error
                    status = e.code
        return status, out.getvalue(), err.getvalue()

    for argv, what in ((["info", hex_path], "info FILE"),
                       (["info", hex_path + ".missing"], "info on a missing file: its error after the banner")):
        status, printed, said = from_command_line(True, *argv)
        rest = said[len(BANNER) + 1:]  # what follows the banner
        check(said.startswith(BANNER + "\n") and from_command_line(False, *argv) == (status, printed, rest),
              f"the banner goes first to stderr at a terminal, and nowhere when stderr is not one ({what})")
    shown = ["".join(from_command_line(True, *argv)[1:]) for argv in (["--help"], ["--version"], ["info"])]
    check(not any(BANNER in text for text in shown), "a terminal gets no banner for --help, --version or a usage error")


def check_tools(check):
    """The tools that need no hardware: the ICSP decoder, decode and pgd-levels on good and bad input,
    sniff-analog's default sample rates, the refusals of sniff and sniff-analog, and the cmake and ninja that
    build takes. Those that read a capture need numpy."""
    import builtins
    from unittest import mock

    def emulated(*ops):
        """The ICSP emulator after ops, from W0 = 0x1234 and W2 = 0x3333."""
        emu = IcspEmulator()
        emu.set_w(0, 0x1234)
        emu.set_w(2, 0x3333)
        for op in ops:
            emu.execute(op)
        return emu

    def write_analog(folder, channel, volts, rate=50_000_000):
        """analog_<channel>.bin as sniff-analog exports it: the header, then the volts as float32."""
        with open(os.path.join(folder, f"analog_{channel}.bin"), "wb") as f:
            f.write(ANALOG_HEADER.pack(b"<SALEAE>", 0, 1, 0.0, rate, 1, len(volts)))
            f.write(struct.pack(f"<{len(volts)}f", *volts))

    def analog_capture(folder, bits, repeat=1, pgd=2, pgc=3):
        """analog_0.bin (MCLR) and the PGD and PGC channels' files as sniff-analog exports them, at 50 MS/s
        times repeat (each sample repeat times): MCLR pulses (the entry), then stays high while PGC clocks bits
        out on PGD, 200 ns per phase. 40 ns before each PGC edge PGD moves 0.4 V towards VDD/2; 20 ns before
        PGC falls it already has the next bit."""
        h, n = 10, len(bits)
        levels = {0: [0] * h + [1] * h + [0] * h + [1] * (2 * h * n + h) + [0] * h,
                  pgd: [0] * 3 * h + [b for b in bits for _ in range(2 * h)] + [0] * 2 * h,
                  pgc: [0] * 3 * h + ([0] * h + [1] * h) * n + [0] * 2 * h}
        volts = {channel: [3.3 * v for v in values] for channel, values in levels.items()}
        line = volts[pgd]
        for k, b in enumerate(bits):
            fall = 3 * h + 2 * h * (k + 1)  # PGC rises at fall - h
            line[fall - h - 2] = line[fall - 2] = 2.9 if b else 0.4
            line[fall - 1] = line[fall]
        for channel, values in volts.items():
            write_analog(folder, channel, [v for v in values for _ in range(repeat)], 50_000_000 * repeat)

    def digital_capture(path, bits, lines=SALEAE_CHANNEL_MAP, key="ICSP"):
        """A Saleae digital CSV export of a programming session on the channels the --map lines gives, as the
        bench captures one: MCLR high (pulled up), the entry pulse (low, high, low), the entry key of KEY_NAMES'
        key (MSb first), MCLR high while PGC clocks bits out on PGD (LSb first), then MCLR low and high again
        (released: the PIC runs). Rows 1 us apart, but PGC is high 0.5 us in the key; VDD, if there, stays high."""
        names = channel_numbers(lines)
        level = {channel: int(name == "VDD") for name, channel in sorted(names.items(), key=lambda item: item[1])}
        rows, now = ["Time [s]," + ",".join(f"Channel {channel}" for channel in level)], [0.0]

        def put(name, value, after=1e-6):
            now[0] += after
            level[names[name]] = value
            rows.append(f"{now[0]:.7f}," + ",".join(map(str, level.values())))

        def clock(bit, high=1e-6):
            put("PGD", bit)
            put("PGC", 1)
            put("PGC", 0, high)

        for value in (1, 0, 1, 0):  # pulled up, the entry pulse, then low while the key goes in
            put("MCLR", value)
        entry = next(code for code, name in KEY_NAMES.items() if name == key)
        for k in range(31, -1, -1):
            clock(entry >> k & 1, 0.5e-6)
        put("MCLR", 1)
        for b in bits:
            clock(b)
        put("MCLR", 0)
        put("MCLR", 1)
        write_lines(path, rows)

    emu = emulated(0xEB8000)  # SETM W0: the CLR W0 encoding with bit 15 set
    check(disasm(0xEB8000) == "??? 0xEB8000" and emu.w(0) == 0x1234 and emu.unknown == {"EB8000": 1},
          "decode: SETM W0 is an unknown opcode, not CLR W0")
    check(disasm(0xEB4100) == "CLR.B W2" and emulated(0xEB4100).w(2) == 0x3300,
          "decode: CLR.B W2 clears only its low byte")
    unused = (0xEB3000, 0xEB3880, 0xBA0060, 0xBB3000)  # CLR in modes 6 and 7, TBLRDL and TBLWTL in mode 6
    check(all(disasm(op) == f"??? 0x{op:06X}" for op in unused) and len(emulated(*unused).unknown) == len(unused),
          "decode: CLR, TBLRD and TBLWT with the unused addressing modes 6 and 7 are unknown opcodes")
    check([latch_samples(rate) for rate in (50e6, 100e6, 25e6, 10e6)] == [2, 4, 1, 1],
          "pgd-levels: 40 ns is 2 samples at 50 MS/s, 4 at 100, 1 at 25, and still 1 at 10 (at least one)")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "unused")  # the --out of sniff and sniff-analog: they never get to make it
        sniff = []  # the options sniff runs with: a port it refuses is exit 2 instead
        with mock.patch.dict(globals(), cmd_sniff=sniff.append):
            for port in ("1", "65535"):
                run_main("sniff", "--out", out, "--saleae-port", port, "--", sys.executable, "-c", "pass")
        check([options.saleae_port for options in sniff] == [1, 65535],
              "--saleae-port takes 1 and 65535, the first and last port")
        sniff_analog = []  # the options sniff-analog runs with when it is given none
        with mock.patch.dict(globals(), cmd_sniff_analog=sniff_analog.append):
            run_main("sniff-analog", "--out", out)
        rates = [(options.rate, options.analog_rate) for options in sniff_analog]
        check(rates in ([(125_000_000, 50_000_000)], [(250_000_000, 50_000_000)]),
              "sniff-analog's default sample rates are a pair a Logic Pro 8 takes: digital 125 or 250 MS/s, analog 50")
        # both refuse before they need the Saleae API, hidden here: one that did not would stop in need_saleae(),
        # never reaching a Logic 2 that runs on this computer
        with modules_replaced({"saleae": None, "saleae.automation": None, "grpc": None}):
            for command in ("sniff", "sniff-analog"):
                for port in ("0", "65536"):
                    r = run_main(command, "--out", out, "--saleae-port", port, "--", sys.executable, "-c", "pass")
                    check(r.returncode == "SystemExit: 2" and f"{port} is not a TCP port (1 to 65535)" in r.stderr,
                          f"{command} --saleae-port {port}: not a TCP port, a usage error (exit 2)")
                channel_map = "0=MCLR,1=VDD,2=PGD"
                r = run_main(command, "--out", out, "--map", channel_map, "--", sys.executable, "-c", "pass")
                refusal = f"ERROR: --map {channel_map}: no channel for PGC (the default is {SALEAE_CHANNEL_MAP})\n"
                check(r.returncode == 2 and r.stderr == refusal and not os.path.exists(out),
                      f"{command} refuses --map {channel_map} before the capture starts (exit 2)")
        sdk = os.path.join(tmp, ".pico-sdk")
        for folder in ("cmake/v3.31.5/bin", "cmake/v4.3.4/bin", "cmake/v4.10.0/bin", "ninja/v1.9.0", "ninja/v1.12.1"):
            os.makedirs(os.path.join(sdk, folder))
        with mock.patch.dict(os.environ, HOME=tmp, USERPROFILE=tmp):  # ~ is HOME, or USERPROFILE on Windows
            path = sdk_tools_env()["PATH"].split(os.pathsep)
        check(path[:2] == [os.path.join(sdk, "cmake", "v4.10.0", "bin"), os.path.join(sdk, "ninja", "v1.12.1")],
              "build: the newest cmake and ninja of ~/.pico-sdk, by version number (v4.10.0 after v4.3.4)")
        csv_header = "Time [s],Channel 0,Channel 1,Channel 2,Channel 3"
        rows = [csv_header, "0,0,1,0,0", "0.000001,1,1,0,0"]
        csv = os.path.join(tmp, "digital.csv")
        write_lines(csv, rows)
        for channel_map, says in (("0=MCLR,1=VDD,2=PGD", "no channel for PGC"),
                                  ("0=MCLR;1=VDD;2=PGD;3=PGC", "not a list of CHANNEL=NAME"),
                                  ("0=MCLR,1=VDD,2=PGD,-1=PGC", "not a list of CHANNEL=NAME"),
                                  ("0=MCLR,1=MCLR,2=PGD,3=PGC", "a line or a channel given twice"),
                                  ("0=MCLR,0=VDD,2=PGD,3=PGC", "a line or a channel given twice"),
                                  ("0=MCLR,1=VDD,2=PGD,3=CLK", f"CLK is not one of {', '.join(ICSP_LINES)}")):
            r = run_main("decode", csv, "--map", channel_map)
            error = f"ERROR: --map {channel_map}: {says} (the default is {SALEAE_CHANNEL_MAP})\n"
            check(r.returncode == 2 and r.stderr == error, f"decode refuses --map {channel_map}: {says} (exit 2)")
        try:
            import numpy as np
        except ImportError:
            print("  skip  decode and pgd-levels on captures: numpy is not installed")
            return
        check(parse_icsp(np.arange(6) * 1e-6, np.zeros(6, np.int8), np.arange(6)) == ([], 6),
              "decode: a session of 6 PGC clocks, shorter than the first command, is 6 leftover bits")
        # a NOP, a REGOUT, and 2 clocks more at 1: they keep the level of the REGOUT's last bits, so pgd-levels
        # would measure them if it ran past the last whole command
        bits = [0] * (4 + FIRST_SIX_EXTRA + 24) + [1, 0, 0, 0] + [0] * 8 + [1] * 16 + [1, 1]
        reports = []
        for repeat in (1, 2):  # 50 MS/s, then the same signals at 100 MS/s
            analog_capture(tmp, bits, repeat)
            reports.append(run_main("pgd-levels", tmp))
        r = reports[0]
        check(r.returncode == 0 and f"{len(bits)} clocks, 1 SIX, 1 REGOUT, 2 clocks left over" in r.stdout,
              "pgd-levels: 2 clocks after the last whole command are reported, not decoded")
        check(r.returncode == reports[1].returncode == 0 and r.stdout == reports[1].stdout,
              "pgd-levels: the same report at 50 and 100 MS/s (PGD read 40 ns before the PGC edges at any rate)")
        nop = [0] * (4 + 24)  # a SIX with a NOP
        analog_capture(tmp, [1] + [0] * (FIRST_SIX_EXTRA - 1) + nop)  # a 1 in the first command's extra clocks
        r = run_main("pgd-levels", tmp)
        check(r.returncode == 0 and f"{FIRST_SIX_EXTRA + len(nop)} clocks, 1 SIX, 0 REGOUT" in r.stdout,
              "pgd-levels reads the first command's code after its 5 extra clocks, as decode does")
        # PGD and PGC on each other's channel: without --map, a question about it; with it, or with the --map that
        # sniff-analog recorded in run.json, the usual result
        swapped, recorded = "0=MCLR,1=VDD,2=PGC,3=PGD", os.path.join(tmp, "run.json")
        analog_capture(tmp, bits, pgd=3, pgc=2)
        r = run_main("pgd-levels", tmp)
        check(r.returncode == 2 and r.stderr.startswith("ERROR:")
              and f"are the lines where the default map says ({SALEAE_CHANNEL_MAP})?" in r.stderr,
              "pgd-levels with PGD and PGC on each other's channel asks if the lines are where the default map says "
              "(exit 2)")
        r = run_main("pgd-levels", tmp, "--map", swapped)
        check(r.returncode == 0 and r.stdout == reports[0].stdout,
              f"pgd-levels --map {swapped} reads them there: the same report as on the usual channels")
        write_lines(recorded, [json.dumps({"map": swapped})])
        r = run_main("pgd-levels", tmp)
        check(r.returncode == 0 and r.stdout == reports[0].stdout,
              "pgd-levels without --map takes the one sniff-analog recorded in the capture's run.json")
        os.remove(recorded)
        first_six = [0] * (FIRST_SIX_EXTRA + 4 + 24)
        analog_capture(tmp, first_six + [0, 1, 0, 0] + [0] * 24)  # then code 2, LSb first
        r = run_main("pgd-levels", tmp)
        neither = f"command 2 at clock {len(first_six) + 4} is neither SIX nor REGOUT"
        check(r.returncode == 2 and r.stderr.startswith("ERROR:") and neither in r.stderr,
              "pgd-levels refuses a command that is neither SIX nor REGOUT (exit 2)")
        # a capture as the bench makes one: three sessions, the entry pulse's and the release's no programming
        icsp = os.path.join(tmp, "icsp.csv")
        digital_capture(icsp, bits)
        usual = run_main("decode", icsp)
        check(usual.returncode == 0 and "(ICSP)" in usual.stdout and "{'SIX': 1, 'REGOUT': 1}" in usual.stdout
              and usual.stdout.count("(???)") == 2 and not usual.stderr,
              "decode: an ICSP session (the entry key, then a SIX and a REGOUT) between the entry pulse and the "
              "release, and no question about the lines")
        digital_capture(icsp, bits, swapped)
        r = run_main("decode", icsp)
        question = f"are the lines where the default map says ({SALEAE_CHANNEL_MAP})?"
        check(r.returncode == 0 and "(ICSP)" not in r.stdout and r.stderr.endswith(question + "\n"),
              "decode with PGD and PGC on each other's channel finds no ICSP session and asks about the map")
        r = run_main("decode", icsp, "--map", swapped)
        check(r.returncode == 0 and r.stdout == usual.stdout and not r.stderr,
              f"decode --map {swapped} reads them there: the same decode as on the usual channels")
        write_lines(recorded, [json.dumps({"map": swapped})])
        r = run_main("decode", icsp)
        check(r.returncode == 0 and r.stdout == usual.stdout and not r.stderr,
              "decode without --map takes the one sniff recorded in run.json, next to the capture")
        write_lines(recorded, [json.dumps({"map": "0=MCLR,1=VDD,2=PGC"})])
        r = run_main("decode", icsp)
        check(r.returncode == 2 and r.stderr == "ERROR: the map in run.json 0=MCLR,1=VDD,2=PGC: no channel for PGD "
              f"(the default is {SALEAE_CHANNEL_MAP})\n", "decode names where a wrong map came from: run.json")
        # a run.json without a map (sniff up to 1.3.1 wrote none), not JSON, or with a map that is not text: the
        # default map, for decode and for pgd-levels
        digital_capture(icsp, bits)
        analog_capture(tmp, bits)
        for what, text in (("without a map", json.dumps({"cmd": ["python", "x"], "rate": 500000000, "returncode": 0})),
                           ("that is not JSON", "{"), ("whose map is not text", json.dumps({"map": 5}))):
            write_lines(recorded, [text])
            r, levels = run_main("decode", icsp), run_main("pgd-levels", tmp)
            check(r.returncode == 0 and r.stdout == usual.stdout and not r.stderr and levels.returncode == 0
                  and levels.stdout == reports[0].stdout, f"decode and pgd-levels next to a run.json {what}: the "
                  "default map")
        os.remove(recorded)
        elsewhere = "7=MCLR,4=PGD,5=PGC"
        digital_capture(icsp, bits, elsewhere)
        r = run_main("decode", icsp, "--map", elsewhere)
        check(r.returncode == 0 and r.stdout.startswith(usual.stdout.split("  VDD")[0] + "  no VDD\n")
              and r.stdout.split("\n", 1)[1] == usual.stdout.split("\n", 1)[1] and not r.stderr,
              f"decode --map {elsewhere}: any channels, with VDD left out, give the same sessions")
        digital_capture(icsp, bits, "0=MCLR,2=PGD,3=PGC")
        r = run_main("decode", icsp)
        check(r.returncode == 0 and r.stdout == usual.stdout.replace("VDD const=True level=1", "no VDD")
              and not r.stderr, "decode with the default map takes a capture without VDD's channel")
        named = os.path.join(tmp, "named.csv")  # the channels renamed in Logic 2: the columns in order
        digital_capture(named, bits)
        with open(named, encoding="ascii") as f:
            data = f.read().split("\n", 1)[1]
        write_lines(named, ["Time [s],MCLR,VDD,PGD,PGC", data.rstrip("\n")])
        r = run_main("decode", named)
        check(r.returncode == 0 and r.stdout == usual.stdout and not r.stderr,
              "decode reads the columns of channels with other names in order, from channel 0")
        # after the session, the PIC released: PGC floats up for 8 ns and PGD changes meanwhile, which is no ICSP
        digital_capture(icsp, bits)
        with open(icsp, encoding="ascii") as f:
            rows = f.read().splitlines()
        end = float(rows[-1].split(",")[0])
        write_lines(icsp, rows + [f"{end + 1e-6:.9f},1,1,0,1", f"{end + 1.004e-6:.9f},1,1,1,1",
                                  f"{end + 1.008e-6:.9f},1,1,1,0"])
        r = run_main("decode", icsp, "--timing")
        check(r.returncode == 0 and "PGC high time ns: min 500 " in r.stdout
              and "PGD changes while PGC high: 0\n" in r.stdout,
              "decode --timing measures the ICSP session from its key on (PGC high 500 ns there), nothing after it")
        digital_capture(icsp, bits, key="EICSP")  # a PICkit's programming executive (Enhanced ICSP)
        r = run_main("decode", icsp, "--timing")
        check(r.returncode == 0 and "(EICSP)" in r.stdout and "PGC high time ns: min 500 " in r.stdout and not r.stderr,
              "decode --timing measures an EICSP session too, a PICkit's")
        # a pandas that cannot be imported (built for another numpy): its error, not 'not a Saleae digital export'
        real_import = builtins.__import__

        def import_but_pandas(name, *args, **kwargs):
            if name == "pandas":
                raise ValueError("numpy.dtype size changed, may indicate binary incompatibility")
            return real_import(name, *args, **kwargs)

        with mock.patch.object(builtins, "__import__", import_but_pandas):
            r = run_main("decode", icsp)
        check(r.returncode == 2 and r.stderr == "ERROR: numpy.dtype size changed, may indicate binary "
              "incompatibility\n", "decode with a pandas that cannot be imported: its error (exit 2)")
        # no complete ICSP session: without bits, MCLR is high from sample 30 to 40 and PGC never rises;
        # late_clock rises at 35 and falls only after MCLR does
        rise = [0.0] * 10 + [3.3] * 10
        late_clock = [0.0] * 35 + [3.3] * 10 + [0.0] * 5
        for what, session_bits, changed in (("no PGC clock", [], {}), ("MCLR rising only once", bits, {0: rise}),
                                            ("MCLR still high where it ends", bits, {0: rise * 2}),
                                            ("a PGC clock that ends after MCLR falls", [], {3: late_clock})):
            analog_capture(tmp, session_bits)
            for channel, volts in changed.items():
                write_analog(tmp, channel, volts)
            r = run_main("pgd-levels", tmp)
            check(r.returncode == 2 and r.stderr.startswith("ERROR:") and "no complete ICSP session" in r.stderr
                  and r.stderr.endswith(f"are the lines where the default map says ({SALEAE_CHANNEL_MAP})?\n"),
                  f"pgd-levels refuses a capture with {what}: no complete ICSP session, are the lines there? (exit 2)")
        typed = ANALOG_HEADER.pack(b"<SALEAE>", 0, 0, 0.0, 50_000_000, 1, 2) + bytes(8)  # type 0: digital
        cut = ANALOG_HEADER.pack(b"<SALEAE>", 0, 1, 0.0, 50_000_000, 1, 1000) + bytes(8)  # 2 of its 1000 samples
        no_rate = ANALOG_HEADER.pack(b"<SALEAE>", 0, 1, 0.0, 0, 1, 2) + bytes(8)
        no_downsample = ANALOG_HEADER.pack(b"<SALEAE>", 0, 1, 0.0, 50_000_000, 0, 2) + bytes(8)
        for what, data in (("a digital export", typed), ("a digital one without transitions", b"<SALEAE>" + bytes(36)),
                           ("an empty file", b""), ("an analog export cut short", cut),
                           ("an analog export with a sample rate of 0", no_rate),
                           ("an analog export with a downsample of 0", no_downsample)):
            with open(os.path.join(tmp, "analog_2.bin"), "wb") as f:
                f.write(data)
            r = run_main("pgd-levels", tmp)
            check(r.returncode == 2 and r.stderr.startswith("ERROR:") and "not a Saleae analog export" in r.stderr,
                  f"pgd-levels refuses {what} as analog_2.bin (exit 2)")
        one_row = os.path.join(tmp, "one-row.csv")  # a capture without transitions: the levels at the start
        write_lines(one_row, rows[:2])
        r = run_main("decode", one_row, "--timing")
        check(r.returncode == 0 and r.stdout.startswith("rows=1 ") and "nothing to time" in r.stdout,
              "decode reads a capture of one row (no transition), with nothing to time")
        channel_map = "0=MCLR,1=VDD,2=PGD,9=PGC"
        r = run_main("decode", csv, "--map", channel_map)
        check(r.returncode == 2 and r.stderr == f"ERROR: --map {channel_map}: {csv} has no channel 9 (PGC)\n",
              f"decode refuses --map {channel_map}: the capture has no channel 9 (exit 2)")
        not_digital = os.path.join(tmp, "not-digital.csv")
        for what, text in (("a CSV with semicolons, as a spreadsheet may save it", "\n".join(rows).replace(",", ";")),
                           ("an empty file", ""), ("a CSV header without rows", csv_header + "\n"),
                           ("an analog export (volts), as sniff --analog-rate also writes one",
                            csv_header + "\n0.000000,3.25,3.31,0.03,0.02\n")):
            with open(not_digital, "w") as f:
                f.write(text)
            r = run_main("decode", not_digital)
            check(r.returncode == 2 and r.stderr == f"ERROR: {not_digital}: not a Saleae digital export\n",
                  f"decode refuses {what}: not a Saleae digital export (exit 2)")
        missing = os.path.join(tmp, "missing")
        for command, path in (("decode", missing + ".csv"), ("pgd-levels", missing)):
            r = run_main(command, path)
            check(r.returncode == 2 and r.stderr.startswith("ERROR:") and r.stderr.count("\n") == 1,
                  f"{command} refuses a capture that does not exist (exit 2)")


def cmd_test_hex(args):
    """The checks that need no hardware: every damaged or wrong .hex file is refused before anything
    is sent to the Pico, a good file gives exactly the image MPLAB would program, and the image in
    bp3unbricker.c is the .hex it was made from; make-test-image's images; a bad FILE, COMMAND, image
    name or hex2c --source, or no buzzpirat clone, refused before pyserial or a port is needed. Then
    check_version_and_banner() and check_tools(); then all of it again with python -S, without the packages."""
    check = Checks()

    def refused(path, text):
        try:
            load_image(path, "file")
        except HexError as e:
            return text in str(e)
        return False

    def instruction(index, word):
        """The records of one instruction: its base address, then its 3 bytes and the phantom 0."""
        byte_addr = index * 4
        return [base_record(byte_addr >> 16), hex_record(byte_addr, word.to_bytes(3, "little") + b"\0")]

    def config_lines(cw2=BP3_CW2, cw1=BP3_CW1):
        return instruction(CW2_INDEX, cw2) + instruction(CW1_INDEX, cw1)

    embedded, bl = embedded_from_source()
    good_path = os.path.join(ROOT, "stuff", embedded.name)
    with open(good_path) as f:
        good_lines = f.read().split()
    image, _, problems = load_image(good_path, "file")
    check(image.words == embedded.words and image.crc32 == embedded.crc32 and not problems,
          f"bp3unbricker.c holds stuff/{embedded.name} word for word (crc32 {image.crc32:08X}, no warnings)")
    check(all(bl.words[i] == (image.words[i] if i < RESET_WORDS or i >= BL_PAGE_INDEX else BLANK)
              for i in range(FLASH_WORDS))
          and bl.words[:RESET_WORDS] == GOTO_BOOTLOADER and bl.words[USRAPP_INDEX] == BLANK
          and not bootloader_problems(bl.words),
          f"bootloader alone: reset GOTO + page 0x00A800, no GOTO to a firmware ({bl.used_words} words, "
          f"crc32 {bl.crc32:08X} as in bp3unbricker.c)")

    with tempfile.TemporaryDirectory() as tmp:
        def write(name, lines):
            path = os.path.join(tmp, name)
            write_lines(path, lines)
            return path

        bad = list(good_lines)
        bad[100] = bad[100][:-2] + ("00" if bad[100][-2:] != "00" else "01")
        check(refused(write("sum.hex", bad), "bad checksum"), "bad record checksum")
        truncated = write("trunc.hex", good_lines[:-40])
        check(refused(truncated, "truncated"), "file cut short (no end record)")
        base0 = base_record(0)  # the file ends with a higher base address
        lines = good_lines[:-1] + [base0, good_lines[5], good_lines[-1]]
        check(load_image(write("dup.hex", lines), "file")[0].crc32 == image.crc32,
              "a record repeated with the same data is fine")
        addr, rtype, payload = parse_hex_record(good_lines[5])
        conflict = hex_record(addr, bytes([payload[0] ^ 0xFF]) + payload[1:], rtype)
        lines = good_lines[:-1] + [base0, conflict, good_lines[-1]]
        check(refused(write("conflict.hex", lines), "twice with different values"),
              "the same byte given twice with different values")
        lines = config_lines() + [base_record(0x0100), hex_record(0x0000, [1, 2, 3, 0]), EOF_RECORD]
        check(refused(write("outside.hex", lines), "outside the program flash"),
              "data in the executive memory (PC 0x800000): never written")
        lines = config_lines() + [hex_record(0x0100, [1, 2, 3]), EOF_RECORD]
        check(refused(write("partial.hex", lines), "incomplete instruction"), "an instruction with a missing byte")
        lines = config_lines() + [hex_record(0x0100, [1, 2, 3, 4]), EOF_RECORD]
        check(refused(write("phantom.hex", lines), "phantom byte"), "a non-zero phantom byte")
        path = write("noconfig.hex", instruction(0, 0x040200) + [EOF_RECORD])
        check(refused(path, "no configuration words"), "no configuration words: refused by default")
        img = load_image(path, "add")[0]
        check(img.words[CW2_INDEX] == 0xF9DF and img.words[CW1_INDEX] == 0x3F7F,
              "--add-config adds the Bus Pirate v3 configuration words")
        check(load_image(path, "none")[0].words[CW1_INDEX] == BLANK, "--no-config leaves them erased")
        lines = instruction(CW1_INDEX, 0x3F7F) + [EOF_RECORD]
        check(refused(write("onecw.hex", lines), "only one of the two"), "only one configuration word")
        protected = write("protect.hex", config_lines(cw1=0x1F7F) + [EOF_RECORD])  # GCP=0: code protection
        check(any("GCP=0" in p for p in load_image(protected, "file")[2]), "code protection is reported")
        lines = config_lines(cw2=0xFEDF) + [EOF_RECORD]  # FNOSC=110
        check(any("FNOSC" in p for p in load_image(write("fnosc.hex", lines), "file")[2]),
              "a reserved oscillator setting is reported")
        lines = config_lines(cw1=0xBF7F) + [EOF_RECORD]
        _, notes, problems = load_image(write("cw15.hex", lines), "file")
        check(not problems and any("bit 15" in n for n in notes), "CW1 bit 15 set: a note, not compared")
        lines = [base_record(0), hex_record(0xFFFC, [1, 2, 3, 0, 4, 5, 6, 0])] + config_lines() + [EOF_RECORD]
        img = load_image(write("cross.hex", lines), "file")[0]
        check(img.words[0x3FFF] == 0x030201 and img.words[0x4000] == 0x060504,
              "a record running past 64 KB continues linearly (as MPLAB writes it)")
        written = {i: (i * 7919) & 0xFFFFFF for i in range(0, FLASH_WORDS, 97)}
        written[CW2_INDEX], written[CW1_INDEX] = BP3_CW2, BP3_CW1
        path = os.path.join(tmp, "round.hex")
        write_hex(path, written)
        img = load_image(path, "file")[0]
        check(all(img.words[i] == w for i, w in written.items()), "write_hex -> load_image gives the same words")
        # make-test-image writes images that flash accepts, but for 'ones' (an erased chip)
        check(all(safe_cw1(v) & (1 << 6) and not config_problems(safe_cw2(v), safe_cw1(v)) for v in range(0x10000)),
              "safe_cw2/safe_cw1: WINDIS=1, no configuration word that flash refuses (every 16-bit value)")

        def make_test_image(pattern, seed=1):
            """make-test-image's exit status and output lines, and the problems flash finds in its image."""
            path = os.path.join(tmp, f"{pattern}-{seed}.hex")  # a new file: never an earlier image
            r = run_main("make-test-image", path, "--pattern", pattern, "--seed", str(seed))
            return r.returncode, r.stdout.splitlines(), load_image(path, "file")[2]

        # the random CW1 of seeds 3 and 4 has WINDIS=0 and FWDTEN=0: safe_cw1() must set WINDIS
        accepted = [make_test_image("zeros")] + [make_test_image("random", seed) for seed in (3, 4)]
        check(all(status == 0 and len(out) == 1 and not problems for status, out, problems in accepted),
              "make-test-image 'zeros' and random seeds 3 and 4: images that flash accepts (no --force line)")
        status, out, problems = make_test_image("ones")
        check(status == 0 and problems and len(out) == 2 and "--force" in out[1],
              "make-test-image 'ones': an erased chip, and it says flash needs --force")
        # flash, test-loader and run refuse a bad FILE or COMMAND before they need pyserial or open any serial
        # port (exit status 2); python -S leaves out site-packages, and so pyserial
        cli = [sys.executable, "-S", os.path.abspath(__file__), "--port", "NO_SUCH_PORT"]
        r = subprocess.run(cli + ["flash", protected], capture_output=True, text=True)
        check(r.returncode == 2 and "--force" in r.stderr, "flash refuses risky configuration words (exit 2)")
        r = subprocess.run(cli + ["flash", truncated], capture_output=True, text=True)
        check(r.returncode == 2 and "truncated" in r.stderr, "flash refuses a truncated file (exit 2)")
        r = subprocess.run(cli + ["test-loader", truncated], capture_output=True, text=True)
        check(r.returncode == 2 and "truncated" in r.stderr, "test-loader refuses a truncated file (exit 2)")
        r = subprocess.run(cli + ["run", "speed r\u00e1pido"], capture_output=True, text=True)
        check(r.returncode == 2 and r.stderr.startswith("ERROR:"), "run refuses a non-ASCII command (exit 2)")
        r = subprocess.run(cli + ["run", "status\nerase"], capture_output=True, text=True)  # 'erase' would run too
        check(r.returncode == 2 and r.stderr == "ERROR: one command of the Pico's console, without control "
              "characters: 'status\\nerase'\n", "run refuses a command with a control character (exit 2)")
        for command, what in (("status\rerase", "a command with a carriage return: the console ends a line there too"),
                              ("", "an empty command: the console would never answer it")):
            r = run_main("--port", "NO_SUCH_PORT", "run", command)
            check(r.returncode == 2 and r.stderr == "ERROR: one command of the Pico's console, without control "
                  f"characters: {command!r}\n", f"run refuses {what} (exit 2)")
        for command in ("test-bootloader", "soak"):  # the same for a usage error: no buzzpirat clone
            r = subprocess.run(cli + [command, "--bp-port", "NO_SUCH_PORT", "--buzzpirat",
                                      os.path.join(tmp, "no-buzzpirat")], capture_output=True, text=True)
            check(r.returncode == 2 and r.stderr == "ERROR: where is buzzpirat (https://github.com/therealdreg/"
                  "buzzpirat)? --buzzpirat DIR or the BUZZPIRAT environment variable\n",
                  f"{command} refuses to start without a buzzpirat clone (exit 2)")
        # hex2c refuses a file name that a C string literal cannot hold as it is (exit status 2). The name is
        # checked before the file is read: only the non-ASCII one needs a file (Windows refuses the others).
        source, names = os.path.join(tmp, "bp3unbricker.c"), ["fw-\u00f1.hex", 'fw"x.hex', "fw\x01x.hex"]
        if os.name != "nt":
            names.append("fw\\x.hex")  # a folder separator on Windows
        shutil.copyfile(SOURCE, source)
        shutil.copyfile(good_path, os.path.join(tmp, names[0]))

        def hex2c_refuses(name):
            r = run_main("hex2c", os.path.join(tmp, name), "--source", source)
            return r.returncode == 2 and r.stderr.startswith("ERROR:")

        check(all(hex2c_refuses(name) for name in names) and file_sha256(source) == file_sha256(SOURCE),
              "hex2c refuses a file name that a C string cannot hold as it is; the source is unchanged (exit 2)")
        check(hex2c_refuses("fw??(x.hex"), "hex2c refuses a file name with ??: a trigraph under -std=c11 (exit 2)")
        # hex2c refuses a --source that it cannot read: one line 'SOURCE: reason', nothing written (exit status 2)
        not_utf8 = os.path.join(tmp, "latin-1.c")
        with open(not_utf8, "wb") as f:
            f.write(b"/* \xf1 */\n")  # an n with a tilde in Latin-1, as an editor may save it
        sha256 = file_sha256(not_utf8)
        unreadable = [os.path.join(tmp, "missing.c"), tmp, not_utf8]  # missing, a folder, not UTF-8 text
        refusals = [(path, run_main("hex2c", good_path, "--source", path)) for path in unreadable]
        check(all(r.returncode == 2 and r.stderr.startswith(f"{path}: ") and r.stderr.count("\n") == 1
                  for path, r in refusals) and not os.path.exists(unreadable[0]) and file_sha256(not_utf8) == sha256,
              "hex2c refuses a --source that is missing, a folder or not UTF-8: one line, nothing written (exit 2)")
    check_version_and_banner(check, good_path)
    check_tools(check)
    if not sys.flags.no_site:  # all of it again without site-packages: no pyserial, numpy or Logic 2 API
        r = subprocess.run([sys.executable, "-S", os.path.abspath(__file__), "test-hex"], capture_output=True,
                           text=True)
        check(r.returncode == 0 and "ALL HEX TESTS PASSED" in r.stdout,
              "test-hex passes with python -S too: it needs no pyserial, numpy or logic2-automation")
        for line in [ln for ln in r.stdout.splitlines() if ln.startswith("  FAIL")] or r.stderr.splitlines()[-1:]:
            print(f"      {line}")
    return check.summary("HEX TESTS")


def upload_tests(pico, image):
    """The upload protocol against a real Pico: damaged lines are refused, and an image with a
    wrong CRC, an aborted upload or a silent computer never becomes usable. The Bus Pirate is not
    written; FILE.hex (default: the built-in image) must be what the PIC holds now."""
    check = Checks()

    def load_ready(name):
        reply = pico.start_load(name)
        return reply is not None and reply.startswith("LOAD READY")

    send = pico.send_line  # as upload() sends its lines

    def send_all():
        # a list, not a generator: every row goes out even after a refused one, so that the end
        # line meets a complete image
        return all([send(row_line(image, row)) == f"OK {row:03X}" for row in range(ROWS) if not image.row_blank(row)])

    def loaded_line():
        _, lines = pico.run("status", show=False, needs_result=False)
        return next((ln for ln in lines if ln.startswith("Loaded:")), None)

    print("damaged lines are refused, and change nothing")
    check(load_ready("faults"), "load -> LOAD READY")
    good = row_line(image, 5)
    bad_crc = good[:-8] + f"{int(good[-8:], 16) ^ 1:08X}"
    check(send(bad_crc) == "ERR 005 crc", "wrong row CRC -> ERR crc")
    flipped = good[:50] + ("0" if good[50] != "0" else "1") + good[51:]
    check(send(flipped) == "ERR 005 crc", "one data digit changed -> ERR crc")
    check(send(good[:60] + "Z" + good[61:]) == "ERR 005 format", "non-hex digit -> ERR format")
    check(send(good[:-1]) == "ERR 005 format", "short line -> ERR format")
    check(send("R 158" + good[5:]) == "ERR 158 row out of range", "row 344 -> out of range")
    check(send("hello") == "ERR ??? format", "garbage -> ERR format")
    check(send("R" + "0" * 700) == "ERR ??? format", "700-character line -> ERR format")
    check(send_all(), "then every real row -> OK")
    reply = send(f"E {image.crc32 ^ 1:08X}")
    check(reply is not None and reply.startswith("LOAD ERR crc32"), f"wrong image CRC -> {reply}")
    check(pico.wait_prompt(5), "back at the prompt")
    check(loaded_line() is None, "no image is loaded after it")
    ok, lines = pico.run("flash loaded", show=False)
    check(not ok and any("no image loaded" in ln for ln in lines), "'flash loaded' refuses: no image")

    print("abort and silence")
    check(load_ready("abort"), "load -> LOAD READY")
    check(send_all(), "every row -> OK")
    check(send("X") == "LOAD ABORTED", "X -> LOAD ABORTED")
    check(pico.wait_prompt(5) and loaded_line() is None, "nothing loaded after an abort")
    check(load_ready("silent"), "load -> LOAD READY")
    pico.ser.write(row_line(image, 0)[:100].encode())  # the start of a line, then nothing
    t0 = time.monotonic()
    reply = pico.read_line(20)
    elapsed_s = time.monotonic() - t0
    check(reply == "LOAD ERR timeout" and 9 < elapsed_s < 12,
          f"the computer goes silent -> {reply} after {elapsed_s:.1f} s")
    check(pico.wait_prompt(5) and loaded_line() is None, "nothing loaded after a timeout")

    print("a clean upload after all that")
    pico.upload(image)
    line = loaded_line()
    check(line is not None and f"crc32 {image.crc32:08X}" in line, f"status: {line}")
    ok, _ = pico.run("verify loaded", show=False)
    check(ok, "'verify loaded' against the PIC")
    return check.summary("UPLOAD TESTS") == 0


def cmd_test_loader(args):
    if args.file:
        try:
            image = load_image(args.file, "add")[0]
        except (OSError, HexError) as e:
            print(f"{args.file}: {e}", file=sys.stderr)
            return 2
    else:
        image = embedded_from_source()[0]
    return with_pico(args, lambda pico: upload_tests(pico, image))


def buzzpirat_dir(args):
    """The buzzpirat clone that --buzzpirat or $BUZZPIRAT names; None, said on stderr, if there is none."""
    path = args.buzzpirat or os.environ.get("BUZZPIRAT")
    if not path or not os.path.isdir(os.path.join(path, "bin")):
        print("ERROR: where is buzzpirat (https://github.com/therealdreg/buzzpirat)? --buzzpirat DIR "
              "or the BUZZPIRAT environment variable", file=sys.stderr)
        return None
    return path


def buzzpirat_firmware_files(buzzpirat):
    """The firmware-only .hex files of buzzpirat (no bootloader, no upgrader): what buzzloader takes."""
    paths = sorted(glob.glob(os.path.join(buzzpirat, "bin", "stablefirm", "**", "*.hex"), recursive=True))
    paths += sorted(glob.glob(os.path.join(buzzpirat, "bin", "oldfirm", "*.hex")))
    return [p for p in paths if not any(k in os.path.basename(p).lower() for k in ("bootloader", "upgrade", "pickit"))]


def buzzloader_parse(path):
    """buzzloader.c readHEX + fixJumps: the words it sends and the 512-word pages it marks used
    (one per data record, whatever the data, and the page of the GOTO it moves to 0x00A7FC)."""
    words = [BLANK] * FLASH_WORDS
    used, base = set(), 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                break
            rec = bytes.fromhex(line[1:])
            n, addr, rtype, data = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:4 + rec[0]]
            if rtype == 0:
                first = (base | addr) // 4
                for i in range(n // 4):
                    words[first + i] = data[4 * i] | data[4 * i + 1] << 8 | data[4 * i + 2] << 16
                    used.add((first + i) // PAGE_WORDS)
            elif rtype == 4:
                base = (data[0] << 24) | (data[1] << 16)
            elif rtype == 1:
                break
    words[USRAPP_INDEX], words[USRAPP_INDEX + 1] = words[0], words[1]
    used.add(USRAPP_INDEX // PAGE_WORDS)
    words[:RESET_WORDS] = GOTO_BOOTLOADER
    return words, sorted(used)


def buzzloader_result(bootloader_words, path):
    """The PIC after buzzloader FILE on a bootloader-only chip: each used page below the bootloader
    erased and written in ascending order; the first page at or above it (42, 0x00A800) is
    refused by the bootloader (ERROR [50]). Returns (words, pages written, refused)."""
    sent, pages = buzzloader_parse(path)
    chip = list(bootloader_words)
    written = [p for p in pages if p < BL_PAGE]
    for p in written:
        chip[p * PAGE_WORDS:(p + 1) * PAGE_WORDS] = sent[p * PAGE_WORDS:(p + 1) * PAGE_WORDS]
    return chip, written, any(p >= BL_PAGE for p in pages)


BUZZLOADER_HELLO = "Device ID: PIC24FJ64GA002 [d4]"  # buzzloader --hello: the bootloader answers
RETRY_OR_FAILURE_RE = re.compile(r"Attempt \d|mismatch at|FAILED")  # in an operation's output, even one that ended OK
FLASH_ATTEMPTS = 3  # bp3unbricker.c's: a flash erases up to this often (soak counts on it; test-cli checks it)


def log_folder(args, test):
    """The folder for this run's logs: --out (default logs/TEST), then YYYYMMDD-HHMM. A run started
    in the same minute reuses it."""
    path = os.path.join(args.out or os.path.join(LOGS, test), datetime.datetime.now().strftime("%Y%m%d-%H%M"))
    os.makedirs(path, exist_ok=True)
    return path


def write_summary(folder, lines):
    """summary.txt with this system's line ends, like the other logs (write_lines() would force LF
    and ASCII)."""
    with open(os.path.join(folder, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")


def pico_status_line(pico):
    """The first line of 'status' (firmware, board, embedded image), or 'unknown Pico firmware'."""
    _, lines = pico.run("status", show=False, needs_result=False)
    return next((ln for ln in lines if ln.startswith("bp3unbricker")), "unknown Pico firmware")


class SameChip:
    """The "same chip" rule of test-bootloader and soak; each call runs 'id'. The first one is the
    baseline: it must pass, or PicoError stops the test before it writes anything. A later one passes
    only if 'id' passes with the baseline's target line (DEVID 0x0447 and the silicon revision) and
    calibration line."""

    def __init__(self):
        self.baseline = None  # (target line, calibration line) of the first 'id'

    def __call__(self, pico):
        ok, lines = pico.run("id", show=False)
        target = next((ln for ln in lines if ln.startswith("Target: DEVID=0x0447 DEVREV=")), None)
        calibration = next((ln for ln in lines if ln.startswith("Diagnostic/calibration")), None)
        identity = (target, calibration) if ok and target and calibration else None
        if self.baseline is None:
            if identity is None:
                raise PicoError("'id' failed before the test, nothing was written "
                                "(python bp3unbricker.py id shows why)")
            self.baseline = identity
        return identity == self.baseline


def readback_diff(pico, expected):
    """Read the whole PIC and count the words that differ from the list expected."""
    words, _ = pico.read_chip(show=False)
    return len(differing_words(expected, words))


def bp_firmware_line_after_start(port):
    """bp_firmware_line() once the PIC, just released or flashed, has had 1.5 s to start its firmware."""
    time.sleep(1.5)
    return bp_firmware_line(port)


def bootloader_tests(pico, args, buzzpirat):
    """'bootloader' + buzzloader on real hardware, for every firmware-only .hex of buzzpirat:
    'bootloader' and a full read-back (only the bootloader); the Bus Pirate silent; 'jumper' and
    buzzloader --hello; buzzloader --hex=FILE (every page below the bootloader, ERROR [50] only
    when the file has data in the bootloader's page); 'release' and the firmware answering; a full
    read-back equal to what the bootloader + buzzloader should leave; DEVID/DEVREV and calibration
    words unchanged. Before: 'bootloader' twice, 'jumper' with a firmware on the PIC. After: 'flash'."""
    buzzloader_exe = args.buzzloader or os.path.join(buzzpirat, "bin", "buzzloader",
                                                     "buzzloader.exe" if os.name == "nt" else "buzzloader_x64_linux")
    out = log_folder(args, "test-bootloader")
    check = Checks()
    embedded, bl = embedded_from_source()
    files = buzzpirat_firmware_files(buzzpirat)
    same_chip = SameChip()

    def pico_op(cmd):
        """A console command, quiet: ok only if it passed at the first attempt, without a mismatch."""
        ok, lines = pico.run(cmd, show=False)
        return ok and not any(RETRY_OR_FAILURE_RE.search(ln) for ln in lines), lines

    def run_buzzloader(*opts, log_path=None):
        try:
            r = subprocess.run([buzzloader_exe, f"--dev={args.bp_port}", *opts], capture_output=True, text=True,
                               timeout=300)
            rc, output = r.returncode, r.stdout + r.stderr
        except subprocess.TimeoutExpired as e:  # a failed check like any other: the test goes on
            rc, output = None, f"buzzloader did not finish within {e.timeout} s\n"
        if log_path:
            with open(log_path, "w") as f:
                f.write(output)
        return rc, output

    def write_bootloader(rec):
        t0 = time.monotonic()
        ok, lines = pico_op("bootloader")
        rec["bootloader_s"] = round(time.monotonic() - t0, 1)
        check(ok and written_crc32(lines) == bl.crc32, f"'bootloader' OK, no retry, crc32 {bl.crc32:08X}", rec)
        diff = readback_diff(pico, bl.words)
        rec["bootloader_readback_diff"] = diff
        check(diff == 0, f"read-back: only the bootloader ({diff} words differ)", rec)

    def cycle(n, path, log):
        name = os.path.basename(path)
        rec = {"cycle": n, "file": name}
        print(f"\n--- cycle {n}: {name}", flush=True)
        t0 = time.monotonic()
        write_bootloader(rec)
        check(bp_firmware_line(args.bp_port) == "", "the Bus Pirate is silent: no firmware", rec)
        ok, _ = pico_op("jumper")
        check(ok, "'jumper'", rec)
        time.sleep(0.3)
        rc, text = run_buzzloader("--hello")
        check(rc == 0 and BUZZLOADER_HELLO in text, "buzzloader --hello: the bootloader answers", rec)
        time.sleep(1.0)  # after a hello the bootloader expects an address; it is back to hello 500 ms later
        expected, written, refused = buzzloader_result(bl.words, path)
        t1 = time.monotonic()
        rc, text = run_buzzloader(f"--hex={path}", log_path=os.path.join(out, f"{n:02d}-{name}.log"))
        rec["buzzloader_s"] = round(time.monotonic() - t1, 1)
        pages = sorted({int(m) for m in re.findall(r"Writing page (\d+) row \d+, [0-9a-f]+\.\.\.OK", text)})
        errors = re.findall(r"ERROR \[([0-9a-f]{2})\]", text)
        rec.update({"pages_written": len(pages), "buzzloader_errors": errors, "buzzloader_exit": rc})
        check(pages == written, f"buzzloader wrote the {len(written)} pages below the bootloader", rec)
        if refused:
            check(errors == ["50"] and f"Erasing page {BL_PAGE}" in text,
                  f"page {BL_PAGE} (configuration words in the file) refused by the bootloader: ERROR [50]", rec)
        else:
            check(rc == 0 and not errors and "Firmware updated successfully" in text,
                  "buzzloader: 'Firmware updated successfully'", rec)
        ok, _ = pico_op("release")
        answer = bp_firmware_line_after_start(args.bp_port)
        rec["bus_pirate"] = answer[:100]
        check(ok and answer != "", f"the firmware runs: {answer[:70]}", rec)
        diff = readback_diff(pico, expected)
        rec["final_readback_diff"] = diff
        check(diff == 0, f"read-back: exactly the bootloader + {name} ({diff} words differ)", rec)
        if expected == embedded.words:
            ok, _ = pico_op("verify")
            check(ok, "the PIC equals the embedded image (the Pico's verify)", rec)
        check(same_chip(pico), "DEVID/DEVREV and calibration words unchanged", rec)
        rec["seconds"] = round(time.monotonic() - t0, 1)
        log.write(json.dumps(rec) + "\n")
        log.flush()
        return not rec.get("failed")

    board = pico_status_line(pico)
    print(board)
    print(f"{len(files)} firmware files, {args.rounds} round(s); logs in {out}", flush=True)
    check(same_chip(pico), "id before the test")  # the baseline; if this 'id' fails, PicoError stops the test here
    print("\n--- 'bootloader' twice in a row, then 'jumper' with a firmware on the PIC")
    rec = {}
    write_bootloader(rec)
    write_bootloader(rec)
    ok, _ = pico_op("flash")
    check(ok, "flash (the embedded image) again")
    ok, _ = pico_op("jumper")
    time.sleep(0.3)
    rc, text = run_buzzloader("--hello")
    check(ok and rc == 0 and BUZZLOADER_HELLO in text, "with a firmware too, 'jumper' gives the bootloader")
    ok, _ = pico_op("release")
    check(ok and bp_firmware_line_after_start(args.bp_port) != "", "after 'release' the firmware runs")
    results = []
    with open(os.path.join(out, "cycles.jsonl"), "w") as log:
        for n, path in enumerate(files * args.rounds, 1):
            results.append((os.path.basename(path), cycle(n, path, log)))
    print("\n--- the embedded image back")
    ok, _ = pico_op("flash")
    check(ok and readback_diff(pico, embedded.words) == 0, "flash + full read-back of the embedded image")
    check(bp_firmware_line_after_start(args.bp_port) != "", "the Bus Pirate runs it")
    lines = [board, f"cycles: {len(results)}, failed: {sum(not passed for _, passed in results)}, checks failed: "
             f"{check.failures}"] + [f"  {'OK  ' if passed else 'FAIL'} {name}" for name, passed in results]
    write_summary(out, lines)
    print("\n" + "\n".join(lines))
    return not check.failures


def cmd_test_bootloader(args):
    buzzpirat = buzzpirat_dir(args)
    if not buzzpirat:
        return 2
    return with_pico(args, lambda pico: bootloader_tests(pico, args, buzzpirat))


def cmd_soak(args):
    """Long hardware run, nothing simulated. Each round does one operation, rotating through:
      selftest  the Pico's full-flash test, then the built-in image
      random    a new random image on all 344 rows, uploaded, flashed, read back in full
      hexfile   the next buzzpirat .hex, uploaded, flashed, read back; a firmware must answer
      reads     three full reads compared with what was written last
      embedded  'flash' with the built-in image, then a full read
    at normal, fast and slow PGC speeds. After every round 'id' must show the same chip as at the
    start (SameChip). Anything unusual is logged. At the end the built-in image is written back."""
    if args.speed:
        print("ERROR: soak runs every speed in turn: --speed does not apply", file=sys.stderr)
        return 2
    # --max-erases is a hard limit: a round starts only if the worst round and the final round still
    # fit. A flash erases once per attempt, up to FLASH_ATTEMPTS; the worst round, 'test', erases
    # twice before its flash; the final round is one flash.
    flash_erases = FLASH_ATTEMPTS
    round_erases = 2 + flash_erases
    if args.max_erases < flash_erases:
        print(f"ERROR: --max-erases must be at least {flash_erases}: the final round alone may erase that often",
              file=sys.stderr)
        return 2
    buzzpirat = buzzpirat_dir(args)
    if not buzzpirat:
        return 2
    anomaly = re.compile(RETRY_OR_FAILURE_RE.pattern + r"|ERR [0-9A-F?]{3}|NOT BLANK")
    schedule = [("selftest", "normal"), ("random", "fast"), ("hexfile", "normal"), ("reads", "slow"),
                ("random", "normal"), ("hexfile", "fast"), ("embedded", "normal"), ("reads", "fast"),
                ("selftest", "fast"), ("random", "slow"), ("hexfile", "slow"), ("reads", "normal"),
                ("embedded", "fast")]
    embedded = embedded_from_source()[0]
    paths = buzzpirat_firmware_files(buzzpirat) + sorted(glob.glob(os.path.join(buzzpirat, "bin", "**", "*.hex"),
                                                                    recursive=True))
    hex_images = []
    for path in dict.fromkeys(paths):  # the firmware-only files first, each file once
        if "upgrade" in os.path.basename(path).lower():
            continue  # a bootloader upgrader rewrites its own flash when it runs
        try:
            image, _, problems = load_image(path, "add")
        except HexError:
            continue
        if not problems:
            hex_images.append(image)
    if not hex_images:
        print(f"ERROR: no usable .hex file under {os.path.join(buzzpirat, 'bin')} for the hexfile rounds",
              file=sys.stderr)
        return 2
    need_serial()  # after the checks above, as in with_pico(): a usage error is exit 2 even without pyserial
    out = log_folder(args, "soak")
    state = {"pico": Pico(args.port), "expected": None, "erases": 0, "next_file": 0, "readbacks": 0}
    same_chip = SameChip()

    def reconnect():
        try:
            state["pico"].close()
        except OSError:
            pass
        time.sleep(12)  # an upload cut short times out on the Pico after 10 s
        state["pico"] = Pico(args.port)

    def run_counted(cmd):
        """A console command, quiet; counts the chip erases as their lines come, so that a command
        cut short (a timeout, a lost USB link) still counts the ones it did."""
        def count_erase(line):
            if line.startswith("Erasing"):
                state["erases"] += 1

        return state["pico"].run(cmd, timeout=1200, on_line=count_erase)

    def readback(image):
        bad = readback_diff(state["pico"], image.words)
        state["readbacks"] += 1
        return bad

    def confirm_written(ok, lines, image, details):
        """After an operation that should leave image on the PIC: a full read-back; if image is
        there, it is what the next 'reads' round compares with."""
        bad = readback(image) if ok else -1
        ok = ok and bad == 0
        state["expected"] = image if ok else None
        details["words_differ"] = bad
        return ok, lines, details

    def flash_image(image):
        resent = state["pico"].upload(image, show=False)
        ok, lines = run_counted("flash loaded")
        if written_crc32(lines) != image.crc32:
            ok = False
            lines.append(f"ERROR: the Pico did not write crc32 {image.crc32:08X}")
        return confirm_written(ok, lines, image, {"crc32": f"{image.crc32:08X}", "rows_resent": resent})

    def op_selftest():
        seed = random.getrandbits(32)
        ok, lines = run_counted(f"test {seed}")
        return confirm_written(ok, lines, embedded, {"seed": f"{seed:08X}"})

    def op_random():
        return flash_image(safe_random_image(random.getrandbits(32)))

    def op_hexfile():
        image = hex_images[state["next_file"] % len(hex_images)]
        state["next_file"] += 1
        ok, lines, details = flash_image(image)
        details["file"] = image.name
        if ok and image.used_rows > 50:  # a firmware, not just a bootloader: it must run
            state["pico"].run("release", show=False)
            answer = bp_firmware_line_after_start(args.bp_port)
            details["bus_pirate"] = answer[:90] or "NO ANSWER"
            ok = bool(answer)
        return ok, lines, details

    def op_embedded():
        ok, lines = run_counted("flash")
        return confirm_written(ok, lines, embedded, {})

    def op_reads():
        lines = []  # the output of the flash that restores the image first, if one is needed
        if state["expected"] is None:
            ok, lines, details = op_embedded()
            if not ok:
                return ok, lines, details
        bad = [readback(state["expected"]) for _ in range(3)]
        return not any(bad), lines, {"words_differ": bad, "image": state["expected"].name}

    ops = {"selftest": op_selftest, "random": op_random, "hexfile": op_hexfile, "embedded": op_embedded,
           "reads": op_reads}

    def round_passed(rec):
        return rec["ok"] and rec["id_ok"] and not rec["anomalies"]

    def one_round(n, op, speed, log):
        t0 = time.monotonic()
        try:
            state["pico"].run(f"speed {speed}", show=False, needs_result=False)
            ok, lines, details = ops[op]()
        except (PicoError, OSError) as e:
            ok, lines, details = False, [f"EXCEPTION: {e}"], {}
            state["expected"] = None
            reconnect()
        seconds = time.monotonic() - t0
        try:
            state["pico"].run("speed normal", show=False, needs_result=False)
            id_ok = same_chip(state["pico"])
        except (PicoError, OSError) as e:
            lines.append(f"EXCEPTION: {e}")
            id_ok = False
            reconnect()
        anomalies = [ln for ln in lines if anomaly.search(ln) or ln.startswith(("EXCEPTION", "ERROR"))]
        rec = {"round": n, "op": op, "speed": speed, "ok": ok, "id_ok": id_ok, "seconds": round(seconds, 1),
               "erases_total": state["erases"], **details, "anomalies": anomalies[:20]}
        log.write(json.dumps(rec) + "\n")
        log.flush()
        flag = "OK  " if round_passed(rec) else "FAIL"
        extra = " ".join(f"{k}={v}" for k, v in details.items())
        print(f"{datetime.datetime.now():%H:%M:%S} #{n:3d} {flag} {op:<8} {speed:<6} {seconds:6.1f}s "
              f"erases={state['erases']:3d} {extra}" + (f" ANOMALIES: {anomalies[:3]}" if anomalies else ""),
              flush=True)
        return rec

    board = pico_status_line(state["pico"])
    print(f"{board}\nsoak test for {args.minutes} min, at most {args.max_erases} erases; "
          f"{len(hex_images)} .hex files in rotation; log in {out}", flush=True)
    same_chip(state["pico"])  # the baseline; if this 'id' fails, PicoError stops the test here
    print(f"calibration words: {same_chip.baseline[1]}", flush=True)
    records = []
    deadline = time.monotonic() + args.minutes * 60
    n = 0
    with open(os.path.join(out, "rounds.jsonl"), "w") as log:
        while time.monotonic() < deadline and state["erases"] + round_erases + flash_erases <= args.max_erases:
            op, speed = schedule[n % len(schedule)]
            n += 1
            records.append(one_round(n, op, speed, log))
        print("--- final: built-in image back, read back in full ---", flush=True)
        records.append(one_round(n + 1, "embedded", "normal", log))
    failed = [r for r in records if not round_passed(r)]
    rounds = collections.Counter(f"{r['op']} {r['speed']}" for r in records)
    failures = collections.Counter(f"{r['op']} {r['speed']}" for r in failed)
    lines = [board, f"rounds: {len(records)}, failed: {len(failed)}, chip erases: {state['erases']}",
             f"full read-backs compared on the computer: {state['readbacks']} "
             f"({state['readbacks'] * FLASH_WORDS} words)",
             f"rows resent during uploads: {sum(r.get('rows_resent', 0) for r in records)}",
             f"DEVID/DEVREV and calibration words unchanged after every round: {all(r['id_ok'] for r in records)}"]
    lines += [f"  {k:<16} {rounds[k]:3d} rounds, {failures[k]} failed" for k in sorted(rounds)]
    write_summary(out, lines)
    print("\n".join(lines), flush=True)
    state["pico"].close()
    return 1 if failed else 0


# ======================================================================= test-cli: commands against emulated hardware
#
# test-cli runs main() in this process for each scenario of CLI_SCENARIOS against emulated hardware: the Pico's
# console (sections 7-10 of bp3unbricker.c as this program sees them), the Bus Pirate's own port, Logic 2 and its
# gRPC transport, cmake and buzzloader. Only the commands that sniff and sniff-analog run are real processes, and a
# virtual clock is this module's time: the upload's 10 s timeout or soak's reconnect take no real time.
# The emulated console prints only printf formats of bp3unbricker.c (say()), and test-cli checks that sections 1-10
# still have each of them; the limits of an upload (the name's length and characters, the line's length, the 10 s
# per line) and the flash attempts it takes from there too (c_facts()). So a firmware text the emulation does not
# follow fails the test, and so does a change of those numbers that this program does not follow. It decodes what
# this program sends (rows, names, CRCs) on its own, never with this program's encoders.
# The hardware takes its time: the Pico's operations as long as the README says, nothing to read before they end; a
# read that finds nothing waits the port's timeout (a port without one fails: the program would hang);
# after a HELLO the bootloader takes every byte for 500 ms as part of a command; a cut upload goes on until its
# 10 s timeout, also when the port is opened again; Logic 2 arms a triggered capture only 0.5 s after it starts.
# What it cannot see: a format printed where the firmware prints another one, the lines it leaves out (a failed
# operation prints only its ERROR and result lines), how long the bootloader and the Bus Pirate's firmware take to
# start (the waits after 'jumper' and 'release'), and buzzloader's own lines (the hardware test-bootloader checks
# those).

OP_SECONDS = {"flash": 23, "flash loaded": 23, "verify": 8, "verify loaded": 8, "blank": 8, "erase": 8, "dump": 12,
              "bootloader": 17, "test": 70}  # how long the Pico's operations take (README, at normal speed)
HELLO_S = 0.5  # the bootloader takes the bytes after a HELLO as a command until it has heard nothing this long
ARM_S = 0.5  # Logic 2 arms a triggered capture this long after it starts it
CALIBRATION = (0x000FFF, 0x00BA7E, 0x001E0A, 0x000000, 0x00C0DE, 0x000123, 0x000F00, 0x00AB00)  # the words 'id' shows
NO_TARGET = "no target (MCLR not pulled up: Bus Pirate unpowered, not connected or held in reset)"  # pic_status_str()
NO_RESPONSE = "the PIC does not answer over ICSP (check PGC/PGD/MCLR/GND wiring)"


class VirtualClock:
    """This module's time while test-cli runs a scenario: sleep() and a read that finds nothing advance the clock
    at once. A sleep still yields a real slice, up to 50 ms, so that another thread (term's reader) gets to run;
    only the scenario's own thread moves the clock."""

    def __init__(self):
        self.real = time  # the time module: built before the scenario replaces this module's time
        self.offset = 0.0
        self.owner = threading.get_ident()

    def __getattr__(self, name):  # the rest of the time module
        return getattr(self.real, name)

    def monotonic(self):
        return self.real.monotonic() + self.offset

    def sleep(self, seconds):
        part = min(seconds, 0.05)
        self.real.sleep(part)
        self.offset += seconds - part

    def wait(self, seconds):
        """A read with nothing to read: seconds pass at once for the scenario's thread; another thread really
        waits 1 ms, so that a busy reader cannot run out the scenario's deadlines."""
        mine = threading.get_ident() == self.owner
        if mine:
            self.offset += seconds
        self.real.sleep(0 if mine else 0.001)


class EmulatedPortError(OSError):
    """serial.SerialException of the emulated pyserial: an OSError, as in pyserial 3."""


class EmulatedPort:
    """A serial port as this program uses pyserial's: what the device sends waits in out until it is read, and
    until the device is ready (an operation runs until then). A read that finds nothing waits the port's timeout;
    pyserial's None would wait forever, so it fails the scenario: on the bench the program would hang."""

    def __init__(self, bench, timeout):
        self.bench = bench
        self.timeout = timeout
        self.ready = 0.0  # the device says nothing before this time of the bench's clock
        self.out = bytearray()
        self.lock = threading.Lock()
        self.closed = self.lost = False  # lost: the USB link is gone (the knob drop)

    @property
    def in_waiting(self):
        return len(self.out) if self.bench.clock.monotonic() >= self.ready else 0

    def read(self, n=1):
        with self.lock:
            if self.closed or (self.lost and not self.out):
                raise EmulatedPortError("the USB link was lost" if self.lost else "port closed")
            self.tick()
            data = bytes(self.out[:n]) if self.bench.clock.monotonic() >= self.ready else b""
            del self.out[:len(data)]
        if not data:
            if self.timeout is None:
                raise AssertionError("a read without a timeout from a port that says nothing: the program would hang")
            self.bench.clock.wait(self.timeout)
        return data

    def write(self, data):
        with self.lock:
            if self.closed or self.lost:
                raise EmulatedPortError("the USB link was lost" if self.lost else "port closed")
            self.bench.written.append(bytes(data))
            self.receive(bytes(data))
        return len(data)

    def reset_input_buffer(self):
        with self.lock:
            self.out.clear()

    def close(self):
        self.closed = True

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def tick(self):
        """What the device does as time passes: nothing, unless it says otherwise."""


class EmulatedPico(EmulatedPort):
    """The bp3unbricker console as this program sees it: echo and prompt, 'load', and the operations it runs, with
    the bench's faults. An upload belongs to the Pico (the bench), not to the port: one cut short goes on until its
    timeout, and until then a port opened again gets no banner, as the firmware's main loop does not run."""

    def __init__(self, bench, timeout=None):
        super().__init__(bench, timeout)
        self.typed = ""     # the command line being typed
        self.output = None  # what the running operation prints, for 'log'
        if self.upload_timed_out():  # while nobody listened: what the Pico said then is gone
            bench.upload, bench.pending = None, ""
        if not bench.upload:  # the main loop sees a terminal open: the banner (its last line here) and a prompt
            self.say("Type 'help' for commands, 'flash' to unbrick. BOOTSEL held 1 s = flash.\n")
            self.say("> ")

    @staticmethod
    def kept_name(name, facts):
        """loader_begin(): letters, digits and the firmware's NAME_CHARS kept, '_' for anything else, at most
        NAME_MAX_LEN characters (facts: c_facts())."""
        kept = "".join(c if c.isascii() and (c.isalnum() or c in facts["NAME_CHARS"]) else "_" for c in name or "")
        return kept[:facts["NAME_MAX_LEN"]] or "uploaded-image"

    def upload_timed_out(self):
        """read_raw_line(): no line for LOAD_LINE_TIMEOUT_MS since the upload's last one."""
        b = self.bench
        return b.upload and b.clock.monotonic() - b.upload[3] > b.facts["LOAD_LINE_TIMEOUT_MS"] / 1000

    def emit(self, text):
        """Text as it leaves the USB port (the stdio driver turns LF into CR LF)."""
        if self.output is not None:
            self.output.append(text)
        self.bench.console.append(text)
        self.out += text.replace("\n", "\r\n").encode("latin-1")

    def say(self, fmt, *args):
        """printf(fmt, args) of the firmware: fmt is checked against bp3unbricker.c, and so is a literal()."""
        self.bench.formats.add(fmt)
        self.emit(fmt % args)

    def literal(self, text):
        """A string of bp3unbricker.c that a format prints with %s."""
        self.bench.formats.add(text)
        return text

    def reply(self, fmt, *args):
        """A reply of the loader: snprintf(reply, fmt, args), then printf("%s\n", reply) (cmd_load())."""
        self.say("%s\n", self.literal(fmt) % args)

    def receive(self, data):
        b = self.bench
        for c in data.decode("latin-1"):
            if b.upload:
                b.pending += c
                if c == "\n":  # read_raw_line(): LF ends a line, CR is dropped, a line too long comes as "!"
                    line, b.pending = b.pending[:-1].replace("\r", ""), ""
                    self.upload_line(line if len(line) <= b.facts["LOADER_LINE_MAX"] else "!")
            elif c in "\r\n":  # poll_line(): an empty line runs nothing
                if self.typed:
                    line, self.typed = self.typed, ""
                    self.say("\n")
                    b.commands.append(line)
                    start = len(b.console)
                    self.command(line)
                    b.shown[line] = "".join(b.console[start:])
                    if not (b.upload or self.lost):
                        self.say("> ")
            elif " " <= c < "\x7f" and len(self.typed) < 79:
                self.typed += c
                self.emit(c)  # the echo, as it is typed

    def tick(self):
        if self.upload_timed_out():
            self.bench.upload, self.bench.pending = None, ""
            self.say("LOAD ERR timeout\n")
            self.say("> ")

    # ------------------------------------------------------------------ the console (run_command())

    def command(self, line):
        b = self.bench
        name, *args = line.split()
        arg = args[0] if args else None
        ops = {"bootloader": self.write_bootloader, "jumper": self.jumper, "id": self.identify,
               "blank": self.blank_check, "erase": self.erase, "dump": self.dump, "release": self.release}
        name = {"i": "id", "b": "blank", "d": "dump", "read": "dump", "r": "release", "run": "release", "f": "flash",
                "unbrick": "flash", "v": "verify", "h": "help", "?": "help", "s": "status"}.get(name, name)
        if name in ops:
            self.run_op(name, ops[name])
        elif name in ("flash", "verify"):
            if arg and arg != "loaded":
                self.say("usage: %s [loaded]\n", name)
            elif arg and not b.loaded:
                self.say("ERROR: %s\n", self.literal("no image loaded (bp3unbricker.py flash FILE.hex sends one)"))
            else:
                self.run_op(name + " loaded" if arg else name, self.flash if name == "flash" else self.verify,
                            b.loaded if arg else b.embedded)
        elif name == "help":
            self.say("Commands:\n  flash         unbrick: erase, program bootloader+firmware, verify all, run\n")
        elif name == "status":
            self.status()
        elif name == "log":
            self.replay()
        elif name == "load":
            self.start_upload(arg)
        elif name == "test":
            self.run_op("test", self.selftest, int(arg, 0) if arg else 1)
        elif name == "speed":
            b.speed_ns = {"slow": 2000, "normal": 500, "fast": 200}.get(arg, b.speed_ns)
            if arg not in ("slow", "normal", "fast"):
                self.say("usage: speed slow|normal|fast\n")
            self.say("PGC low/high %lu/%lu ns\n", b.speed_ns, b.speed_ns)
        else:
            self.say("Unknown command '%s' (type 'help')\n", line.split()[0])

    def run_op(self, name, fn, *args):
        """run_op(): the operation's lines and its result line, both kept for 'log'. The knob fail ({name:
        status}) makes it fail with that status, nothing written; drop ((name, n)) loses the USB link once the
        lines of the n-th run are out."""
        b = self.bench
        b.runs[name] += 1
        self.output = []
        status = b.knobs.get("fail", {}).get(name)
        if status:
            self.say("ERROR: %s\n", status)
        else:
            status = fn(*args)
        if b.knobs.get("drop") == (name, b.runs[name]):
            self.lost = True
            return
        self.say("[%s: %s, %lu ms]\n", name, self.literal(status), 1234)  # status: pic_status_str()
        self.ready = b.clock.monotonic() + OP_SECONDS.get(name, 0)  # what it printed is there once it has ended
        b.last_op = (name, "".join(self.output), status)
        self.output = None

    def status(self):
        """cmd_status(). The knobs: status_name and status_crc, the image it names; old_firmware, a 1.3.0
        firmware (no 'bootloader'); unpowered, a board that does not pull MCLR up."""
        b = self.bench
        self.say("bp3unbricker %s for %s (%s), image %s crc32 %08lX (%s, %lu rows used)\n", VERSION, "pico",
                 "RP2040 B2", b.knobs.get("status_name", b.embedded.name), b.knobs.get("status_crc", b.embedded.crc32),
                 "OK", b.embedded.used_rows)
        if not b.knobs.get("old_firmware"):
            self.say("Bootloader alone: %s, crc32 %08lX (%s, %lu rows used)\n", b.bootloader.name,
                     b.bootloader.crc32, "OK", b.bootloader.used_rows)
        if b.loaded:
            self.say("Loaded: %s, crc32 %08lX, %lu rows used (sent by the computer; 'flash loaded' writes it)\n",
                     b.loaded.name, b.loaded.crc32, b.loaded.used_rows)
        self.say("Pins: PGC=GP%d PGD=GP%d MCLR=GP%d, PGC low/high %lu/%lu ns\n", 10, 11, 12, b.speed_ns, b.speed_ns)
        self.say("clk_sys %lu MHz, hal_delay_ns(1000) takes %lu ns\n", 125, 1003)
        self.say("Bus Pirate: %s\n", self.literal("NOT detected (no power, MCLR not wired, or held in reset)"
                                                  if b.knobs.get("unpowered") else
                                                  "detected (MCLR pulled up, board powered)"))
        if not b.last_op:
            return self.say("Last operation: none since power-up\n")
        self.say("Last operation: %s (%s), finished ", b.last_op[0], "console")
        self.say("%lu s ago", 5)
        self.say(" after %lu.%lu s: ", 1, 2)
        if b.last_op[2] == "OK":
            self.say("OK\n")
        else:
            self.say("FAILED, %s\n", b.last_op[2])

    def replay(self):
        """oplog_replay()."""
        if not self.bench.last_op:
            return self.say("No operation has run since the Pico was powered.\n")
        self.say("--- output of '%s' (%s), finished ", self.bench.last_op[0], "console")
        self.say("%lu s ago", 5)
        self.say(" ---\n")
        self.emit(self.bench.last_op[1])  # printed once already, by the operation

    # ------------------------------------------------------------------ the operations (section 7)

    def flash(self, image):
        """flash_image(). The knobs, for every flash or for the n-th of the bench ({n: value}): attempts, more
        attempts, each one erasing; flip, a bit of that word left wrong, as a weak cell would; image_crc, the crc32
        its Image line shows."""
        b = self.bench
        b.runs["flashes"] += 1
        self.say("Image: %s (crc32 %08lX, %lu rows used)\n", image.name, b.knobs.get("image_crc", image.crc32),
                 image.used_rows)
        for attempt in range(1, b.knob_of_flash("attempts", 1)):
            self.erase_lines()
            self.say("Attempt %d failed: %s\n", attempt, self.literal("verify failed"))
            self.say("Attempt %d of %d with a slower clock (%lu ns per phase)...\n", attempt + 1,
                     b.facts["FLASH_ATTEMPTS"], b.speed_ns << attempt)
        self.erase_lines()
        self.say("Verifying all %u words...\n", FLASH_WORDS)
        b.chip = b.stored(image.words)
        flip = b.knob_of_flash("flip", None)
        if flip is not None:
            b.chip[flip] ^= 0x000100
        b.starts("running")
        self.say("SUCCESS: %s written and verified in %lu ms.", image.name, 1234)
        if image.words[:RESET_WORDS] == GOTO_BOOTLOADER and image.words[USRAPP_INDEX] == BLANK:
            self.say(" Only the bootloader is on the PIC:\n")
        else:
            self.say(" The Bus Pirate is running.\n")
        return "OK"

    def erase_lines(self):
        """A chip erase: soak counts the erases by this line."""
        self.bench.erases += 1
        self.say("Erasing (user memory only, calibration preserved)...\n")

    def write_bootloader(self):
        b = self.bench
        self.say("Bootloader: %lu words of %s; every other word stays erased\n", b.bootloader.used_words,
                 b.embedded.name)
        return self.flash(b.bootloader)

    def verify(self, image):
        differ = sum(1 for i in range(FLASH_WORDS) if (self.bench.chip[i] ^ image.words[i]) & word_mask(i))
        self.bench.starts("running")  # every read-only operation ends with pic_release_target()
        self.say("Verifying against %s...\n", image.name)
        if differ:
            self.say("VERIFY FAILED: %lu words differ\n", differ)
            return "verify failed"
        self.say("VERIFY OK: all %u words match %s\n", FLASH_WORDS, image.name)
        return "OK"

    def identify(self):
        """ops_identify(). The knob ids gives what each 'id' finds, the last one again and again: a DEVREV and a
        change of the calibration words, or the status of an 'id' that fails."""
        b = self.bench
        ids = b.knobs.get("ids", [(0x3046, 0)])
        found = ids[min(b.runs["id"], len(ids)) - 1]
        if isinstance(found, str):
            self.say("ERROR: %s\n", found)
            return found
        devrev, change = found
        self.say("Target: DEVID=0x%04X DEVREV=0x%04X", 0x0447, devrev)
        self.say(" = PIC24FJ64GA002 rev %s\n", self.literal({0x3042: "B4", 0x3046: "B8"}[devrev]))
        self.say("Config: CW2=0x%04lX CW1=0x%04lX\n", b.chip[CW2_INDEX] & 0xFFFF, b.chip[CW1_INDEX] & 0xFFFF)
        self.say("Diagnostic/calibration words @0x8007F0:")
        for w in CALIBRATION:
            self.say(" %06lX", w ^ change)
        self.say("\n")
        b.starts("running")
        return "OK"

    def blank_check(self):
        programmed = sum(1 for i in range(FLASH_WORDS) if (self.bench.chip[i] ^ BLANK) & word_mask(i))
        self.bench.starts("running")
        if programmed:
            self.say("NOT BLANK: %lu words programmed\n", programmed)
            return "chip is not blank"
        self.say("BLANK: all %u words erased (CW1 reads 0x7FFF: reserved bit 15)\n", FLASH_WORDS)
        return "OK"

    def erase(self):
        self.erase_lines()
        self.bench.chip = self.bench.stored(Bench.ERASED)
        self.bench.starts("running")
        self.say("ERASED: the Bus Pirate has no firmware now (run 'flash')\n")
        return "OK"

    def dump(self):
        """ops_dump(): the records as print_hex_record() writes them (hex_record() of this program, which test-hex
        checks). The knob dump damages the read: a 'record', a 'missing' one, the 'crc', the 'endline'."""
        b = self.bench
        records, crc = [base_record(0)], 0
        for first in range(0, FLASH_WORDS, 4):
            words = [w & 0xFFFF if i in (CW1_INDEX, CW2_INDEX) else w for i, w in enumerate(b.chip[first:first + 4],
                                                                                         first)]
            data = b"".join(w.to_bytes(3, "little") for w in words)
            crc = zlib.crc32(data, crc)
            if first and not (first * 4) & 0xFFFF:
                records.append(base_record(first * 4 >> 16))
            records.append(hex_record(first * 4 & 0xFFFF, b"".join(data[k:k + 3] + b"\0" for k in range(0, 12, 3))))
        records.append(EOF_RECORD)
        damage = b.knobs.get("dump")
        if damage == "record":
            records[100] = records[100][:-2] + ("00" if records[100][-2:] != "00" else "01")
        elif damage == "missing":
            del records[200]
        self.say("BEGIN HEX\n")
        self.emit("\n".join(records) + "\n")
        if damage == "endline":
            self.emit("END HEX words=%u\n" % FLASH_WORDS)  # a fault: no firmware prints it
        else:
            self.say("END HEX words=%u crc32=%08lX (embedded image crc32=%08lX)\n", FLASH_WORDS,
                     crc ^ (damage == "crc"), b.embedded.crc32)
        b.starts("running")
        return "OK"

    def selftest(self, seed):
        self.say("Full-flash self test, random seed 0x%08lX\n", seed)
        for n, name in ((1, "random pattern"), (2, "all-zeros pattern")):
            self.say("--- pass %u: %s over 0x000000..0x00ABFE ---\n", n, self.literal(name))
            self.erase_lines()
        self.say("SELFTEST PASSED in %lu ms: every word erased, written and read back twice.\n", 1234)
        self.say("--- restoring bootloader + firmware ---\n")
        return self.flash(self.bench.embedded)

    def jumper(self):
        self.bench.starts("bootloader")
        self.say("PGC held low like the PGC-PGD jumper, PIC restarted: its bootloader waits in\n")
        return "OK"

    def release(self):
        """ops_release(): the PIC runs if the board pulls MCLR up (powered)."""
        if self.bench.knobs.get("unpowered"):
            self.say("Lines released, but %s\n", self.literal(NO_TARGET))
        else:
            self.bench.starts("running")
            self.say("Lines released, Bus Pirate running\n")
        return "OK"

    # ------------------------------------------------------------------ 'load' (section 8)

    def start_upload(self, name):
        """The knob refuse ({row: n}) answers 'ERR rrr crc' n times to that row."""
        b = self.bench
        b.runs["load"] += 1
        b.upload = [self.kept_name(name, b.facts), bytearray(b"\xff" * FLASH_WORDS * 3),
                    dict(b.knobs.get("refuse", {})), b.clock.monotonic()]
        b.loaded = None
        self.say("LOAD READY %u %u\n", ROWS, ROW_WORDS)

    def upload_line(self, line):
        """loader_line(): one reply per line, for the lines this program sends (a row whose number is no number, or a
        malformed end line, raises). The knob odd_reply answers that row with a line no firmware sends; drop
        (('load', n)) loses the USB link with the first line of the n-th upload, which the Pico goes on waiting for."""
        def is_hex(s):
            return bool(s) and all(c in string.hexdigits for c in s)

        b = self.bench
        if b.knobs.get("drop") == ("load", b.runs["load"]):
            self.lost = True
            return
        name, data, refuse, _ = b.upload
        b.upload[3] = b.clock.monotonic()
        if line[:1] == "R":
            row = int(line[2:5], 16)
            if row >= ROWS:
                return self.reply("ERR %03lX row out of range", row)
            if len(line) != 399 or line[5] != " " or line[390] != " " or not is_hex(line[6:390] + line[391:]):
                return self.reply("ERR %03lX format", row)
            if b.knobs.get("odd_reply") == row:
                return self.emit("HUH?\n")  # a fault: no firmware says it
            row_bytes = b"".join(int(line[k:k + 6], 16).to_bytes(3, "little") for k in range(6, 390, 6))
            if refuse.get(row):
                refuse[row] -= 1
                return self.reply("ERR %03lX crc", row)
            if zlib.crc32(row_bytes) != int(line[391:], 16):
                return self.reply("ERR %03lX crc", row)
            data[row * 192:row * 192 + 192] = row_bytes
            return self.reply("OK %03lX", row)
        if line[:1] == "E":
            b.upload = None
            crc, expected = zlib.crc32(data), int(line[2:], 16)
            if crc != expected:
                self.reply("LOAD ERR crc32 %08lX, expected %08lX", crc, expected)
            else:
                words = [int.from_bytes(data[k:k + 3], "little") for k in range(0, len(data), 3)]
                b.loaded = Image(name, words)
                self.reply("LOAD OK %s crc32 %08lX, %lu rows used", name, crc, b.loaded.used_rows)
            return self.say("> ")
        if line == "X":
            b.upload = None
            self.reply("LOAD ABORTED")
            return self.say("> ")
        return self.reply("ERR ??? format")


class EmulatedBusPirate(EmulatedPort):
    """The Bus Pirate's own port: its firmware's prompt and 'i' when the PIC runs a firmware, the ds30
    bootloader's HELLO reply while 'jumper' holds it in bootloader mode, nothing otherwise."""

    INFO = (b"i\r\nBus Pirate v3.5\r\nCommunity Firmware v7.1 - buzzpirat.com by Dreg BPCOMP TWELVE [HiZ 1-WIRE "
            b"UART I2C SPI 2WIRE 3WIRE KEYB LCD PIC DIO] Bootloader v4.5\r\nDEVID:0x0447 REVID:0x3046 (24FJ64GA002 "
            b"B8)\r\nhttp://dangerousprototypes.com\r\nHiZ>")

    def receive(self, data):
        b = self.bench
        if b.pic == "bootloader" and b.bootloader_present():
            if not b.bootloader_gets(data):
                return  # part of the command a HELLO started: no answer
            if data == b"\xC1":  # HELLO: device id 0xD4, version 4.5, 'K'
                self.out += bytes([0xD4, 0x04, 0x45, ord("K")])
            elif data == b"\xE4":  # rx - 0xC1 - 0x23 == 0: the bootloader starts the firmware
                b.starts("running")
                if b.firmware_runs():
                    self.out += b"\r\nHiZ>"
        elif b.pic == "running" and b.firmware_runs():
            if data == b"\n":
                self.out += b"\r\nHiZ>"
            elif data == b"i\n":
                self.out += self.INFO
            else:
                self.out += data.rstrip(b"\n") + b"\r\nSyntax error at char 1\r\nHiZ>"


class Bench:
    """The emulated hardware of one scenario, what was done to it, and the knobs that make it misbehave (each one
    is described where it acts; chip: 'image', 'bootloader' or 'erased' at the start, pic: 'running' or
    'bootloader', ports: the Picos' serial ports, before: console commands run before this program opens it,
    hello_sent: a HELLO just reached the bootloader). facts: what the emulated console takes from the firmware's
    code (c_facts())."""
    ERASED = [BLANK] * FLASH_WORDS

    def __init__(self, knobs, embedded, bootloader, facts):
        self.knobs, self.facts = knobs, facts
        self.clock = VirtualClock()
        self.embedded, self.bootloader = embedded, bootloader
        start = {"image": embedded.words, "bootloader": bootloader.words, "erased": self.ERASED}
        self.chip = self.stored(start[knobs.get("chip", "image")])  # what the PIC holds
        self.pic = knobs.get("pic", "running")  # 'bootloader': its bootloader waits ('jumper')
        # when the bootloader last got a byte of a command that a HELLO started
        self.command_at = self.clock.monotonic() if knobs.get("hello_sent") else float("-inf")
        self.upload = None  # 'load' going on: [its name, the image's bytes, rows to refuse, when its last line ended]
        self.pending = ""   # an upload line not complete yet
        self.loaded = None  # the image the last 'load' left on the Pico
        self.speed_ns = 500
        self.last_op = None  # (name, what it printed, status): what 'log' replays
        self.erases = 0
        self.runs = collections.Counter()  # of each operation, and of flashes
        self.commands, self.written, self.opened, self.buzzloader, self.logic2 = [], [], [], [], []
        self.formats = set()  # every format the console printed
        self.console, self.shown = [], {}  # all the console said; what each command printed
        self.running, self.stuck = 0, []  # see run_emulated()
        self.ports = knobs.get("ports", ["COM7"])
        self.serial = types.SimpleNamespace(Serial=self.open, SerialException=EmulatedPortError)
        self.list_ports = types.SimpleNamespace(comports=self.comports)
        for line in knobs.get("before", []):
            EmulatedPico(self, 0.05).receive(line.encode() + b"\r")
        del self.commands[:], self.console[:]
        self.runs.clear()

    @staticmethod
    def stored(words):
        """What the PIC holds after words are written: the bits that read back (word_mask())."""
        return [w & word_mask(i) for i, w in enumerate(words)]

    def knob_of_flash(self, name, default):
        """A knob for every flash (a number), or for the n-th flash of the bench ({n: value})."""
        value = self.knobs.get(name, default)
        return value.get(self.runs["flashes"], default) if isinstance(value, dict) else value

    def starts(self, mode):
        """The PIC (re)starts: it runs ('running'), or its bootloader waits for buzzloader ('bootloader')."""
        self.pic, self.command_at = mode, float("-inf")

    def bootloader_gets(self, data):
        """A byte for the bootloader in bootloader mode: within HELLO_S of the byte before, from a HELLO on, it is part
        of the command that the HELLO started, and the command goes on; else a HELLO starts one. True if it counts
        on its own: a HELLO that it answers, or 0xE4."""
        now = self.clock.monotonic()
        alone = now - self.command_at >= HELLO_S
        if not alone or data == b"\xC1":
            self.command_at = now
        return alone

    def bootloader_present(self):
        return self.chip[:RESET_WORDS] == GOTO_BOOTLOADER and self.chip[BL_PAGE_INDEX] != BLANK

    def firmware_runs(self):
        """The bootloader starts a firmware: it is there, and so is its GOTO at 0x00A7FC."""
        return self.bootloader_present() and self.chip[USRAPP_INDEX] >> 16 == 0x04

    def comports(self):
        return [types.SimpleNamespace(device=port, vid=RPI_VID) for port in self.ports] + \
            [types.SimpleNamespace(device="COM3", vid=0x0403)]  # an FTDI port: the Bus Pirate's

    def open(self, port, baudrate=9600, timeout=None, **_):
        self.opened.append(port)
        if port == "BP":
            return EmulatedBusPirate(self, timeout)
        if port not in self.ports:
            raise EmulatedPortError(f"could not open port '{port}': FileNotFoundError(2, 'not found')")
        return EmulatedPico(self, timeout)

    def run(self, cmd, **kwargs):
        """subprocess.run with buzzloader (test-bootloader) and cmake (build) emulated; nothing else may run."""
        program = os.path.basename(cmd[0])
        if program.startswith("buzzloader"):
            return self.run_buzzloader(cmd, kwargs.get("timeout"))
        if program == "fake-cmake":
            return self.run_cmake(cmd)
        raise AssertionError(f"test-cli does not run {cmd}")

    def run_buzzloader(self, cmd, timeout):
        """buzzloader 3.0 talking to the emulated bootloader: a HELLO, then nothing more (--hello) or FILE written page
        by page from the lowest (--hex=FILE, buzzloader_parse() says which), the bootloader's page refused (ERROR
        [50]). The knobs: hello, what --hello prints; buzzloader_hangs, it never ends."""
        option = cmd[2]
        self.buzzloader.append(option if option == "--hello" else "--hex=" + os.path.basename(option[6:]))
        ready = self.pic == "bootloader" and self.bootloader_present() and self.bootloader_gets(b"\xC1")
        if option == "--hello":
            hello = self.knobs.get("hello", BUZZLOADER_HELLO) + "\n" if ready else ""
            return subprocess.CompletedProcess(cmd, 0 if ready else 1, hello, "")
        if self.knobs.get("buzzloader_hangs"):
            raise subprocess.TimeoutExpired(cmd, timeout)
        sent, pages = buzzloader_parse(option[6:])
        lines, status = [], 0
        for page in pages if ready else []:
            if page >= BL_PAGE:
                lines.append(f"Erasing page {page}, {page * PAGE_WORDS * 2:x}...ERROR [50]")
                status = 1
                break
            first = page * PAGE_WORDS
            self.chip[first:first + PAGE_WORDS] = self.stored(sent)[first:first + PAGE_WORDS]
            lines += [f"Writing page {page} row {r}, {(first + r * ROW_WORDS) * 2:x}...OK" for r in range(8)]
        else:
            lines.append("Firmware updated successfully" if ready else "no answer from the bootloader")
            status = 0 if ready else 1
        return subprocess.CompletedProcess(cmd, status, "\n".join(lines) + "\n", "")

    def run_cmake(self, cmd):
        """cmake of the Pico SDK: configuring makes the build folder; building writes a UF2 of two blocks of the
        board's family (BUILD_BOARDS). The knob build_fails makes configuring that board fail."""
        configure = cmd[1] == "-G"
        board = cmd[-1].split("=")[1] if configure else os.path.basename(cmd[-1])[len("build-"):]
        self.commands.append(("configure " if configure else "build ") + board)
        if configure and board == self.knobs.get("build_fails"):
            raise subprocess.CalledProcessError(1, cmd)
        if configure:
            os.makedirs(cmd[-2], exist_ok=True)
        else:
            block = struct.pack("<8I", *UF2_MAGIC, UF2_FLAG_FAMILY_ID, 0, 256, 0, 2, BUILD_BOARDS[board])
            with open(os.path.join(cmd[-1], f"bp3unbricker-{board}.uf2"), "wb") as f:
                f.write(block.ljust(512, b"\0") * 2)
        return subprocess.CompletedProcess(cmd, 0)

    def logic2_modules(self):
        """saleae, saleae.automation, its errors and grpc as Logic 2 behaves on the bench (a Logic Pro 8, automation
        API 1.0.11): a capture that ends at once; mixed sample rates other than the Logic Pro 8's refused; a
        triggered capture armed ARM_S after it starts, so a command started before misses the trigger. The knob
        logic2_error raises an error at a step ('connect', 'start', 'stop', 'wait'): an API error ('Name' or 'Name:
        message'), or a gRPC status and its details as grpc raises them ((code, details))."""
        bench = self
        errors = types.ModuleType("saleae.automation.errors")
        errors.SaleaeError = type("SaleaeError", (Exception,), {})
        for name, base in (("InvalidRequestError", "SaleaeError"), ("IncompatibleApiVersionError", "SaleaeError"),
                           ("CaptureError", "SaleaeError"), ("DeviceError", "CaptureError"),
                           ("OutOfMemoryError", "CaptureError")):
            setattr(errors, name, type(name, (getattr(errors, base),), {}))
        grpc = types.ModuleType("grpc")
        grpc.RpcError = type("RpcError", (Exception,), {})

        def step(name):
            bench.logic2.append(name)
            error = bench.knobs.get("logic2_error", {}).get(name.split()[0])
            if isinstance(error, tuple):
                raise type("_InactiveRpcError", (grpc.RpcError,), {
                    "code": lambda self: types.SimpleNamespace(name=error[0]), "details": lambda self: error[1]})()
            if error:
                raise getattr(errors, error.split(":")[0])(*error.split(": ", 1)[1:])

        class Capture:
            def __init__(self):
                self.armed = bench.clock.monotonic() + ARM_S

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                bench.logic2.append("close")

            def stop(self):
                running = RecordedPopen.started and RecordedPopen.started[-1].poll() is None
                step("stop" + (" (command running)" if running else ""))

            def wait(self):
                early = RecordedPopen.started and RecordedPopen.started[0].started_at < self.armed
                step("wait" + (" (MCLR rose before the capture was armed)" if early else ""))

            def export_raw_data_csv(self, directory, digital_channels, analog_channels=None):
                bench.logic2.append(f"export csv {digital_channels} {analog_channels}")

            def export_raw_data_binary(self, directory, digital_channels, analog_channels=None):
                bench.logic2.append(f"export binary {digital_channels} {analog_channels}")

            def save_capture(self, filepath):
                bench.logic2.append("save " + os.path.basename(filepath))

        def start_capture(device_configuration, capture_configuration):
            device, mode = device_configuration, capture_configuration["capture_mode"]
            trigger = f" trigger {mode['trigger_channel_index']}" if "trigger_channel_index" in mode else ""
            step(f"start digital {device['enabled_digital_channels']} analog {device['enabled_analog_channels']}"
                 + trigger)
            rates = device["digital_sample_rate"], device["analog_sample_rate"]
            if device["enabled_analog_channels"] and rates not in ((125_000_000, 50_000_000),
                                                                   (250_000_000, 50_000_000)):
                raise errors.InvalidRequestError("Invalid supplied sample rate. Allowed sample rates with channel "
                                                 "configuration: {" + "digital 125000000 analog 50000000, " * 99)
            return Capture()

        def connect(port):
            step(f"connect {port}")
            return contextlib.nullcontext(types.SimpleNamespace(start_capture=start_capture))

        automation = types.ModuleType("saleae.automation")
        automation.Manager, automation.errors = types.SimpleNamespace(connect=connect), errors
        for name in ("LogicDeviceConfiguration", "CaptureConfiguration", "ManualCaptureMode",
                     "DigitalTriggerCaptureMode"):
            setattr(automation, name, lambda **kw: kw)
        automation.DigitalTriggerType = types.SimpleNamespace(RISING="rising")
        saleae = types.ModuleType("saleae")
        saleae.automation = automation
        return {"saleae": saleae, "saleae.automation": automation, "saleae.automation.errors": errors, "grpc": grpc}


FIRMWARE_FACTS = ("NAME_MAX_LEN", "LOADER_LINE_MAX", "LOAD_LINE_TIMEOUT_MS", "FLASH_ATTEMPTS")  # c_facts()


def firmware_code(path):
    """Sections 1-10 of bp3unbricker.c, the firmware: not the host tests, whose copies of its lines (what they expect
    it to print) would hide a change of them. HexError when the file has not one 'host tests' section banner,
    whatever its number: then the two cannot be told apart."""
    with open(path, encoding="utf-8") as f:
        parts = re.split(r"/\* =+ \d+\. host tests \*/", f.read())
    if len(parts) != 2:
        raise HexError(f"{path}: not one 'host tests' section banner: its firmware cannot be told from its tests")
    return parts[0]


def c_facts(code, path):
    """What the emulated console takes from the firmware's code (firmware_code() of path) besides its formats: the
    numbers FIRMWARE_FACTS (#define NAME N), and NAME_CHARS, what name_char_ok() keeps in an upload's name besides
    letters and digits. HexError for one that is not there."""
    defines = dict(re.findall(r"^#define (\w+) (\d+)u?\b", code, re.M))
    body = re.search(r"\bname_char_ok\(char c\) \{(.*?)\n\}", code, re.S)
    missing = [name for name in FIRMWARE_FACTS if name not in defines] + ([] if body else ["name_char_ok()"])
    if missing:
        raise HexError(f"{path}: its firmware has no {missing[0]}, which the emulated console takes from it")
    facts = {name: int(defines[name]) for name in FIRMWARE_FACTS}
    facts["NAME_CHARS"] = "".join(re.findall(r"c == '(.)'", body.group(1)))
    return facts


def c_strings(code):
    """The string literals of C code as the compiler sees them: adjacent ones joined, escapes decoded,
    comments and character constants skipped."""
    tokens = re.findall(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|\s+|.', code, re.S)
    strings, joined = [], None
    for tok in tokens:
        if tok.startswith('"'):
            text = re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t", "r": "\r"}.get(m.group(1), m.group(1)), tok[1:-1])
            joined = text if joined is None else joined + text
        elif not (tok.isspace() or tok.startswith(("/*", "//"))):
            if joined is not None:
                strings.append(joined)
            joined = None
    return strings


@contextlib.contextmanager
def modules_replaced(modules):
    """sys.modules with these entries (None: the import fails), and afterwards as it was, entry by entry."""
    saved = {name: sys.modules[name] for name in modules if name in sys.modules}
    sys.modules.update(modules)
    try:
        yield
    finally:
        for name in modules:
            sys.modules.pop(name, None)
        sys.modules.update(saved)


class RecordedPopen(subprocess.Popen):
    """subprocess.Popen that keeps every process it starts (started), so that test-cli can wait for it, and when
    (started_at, on clock: the bench's)."""
    started, clock = [], time

    def __init__(self, *args, **kwargs):
        self.started_at = RecordedPopen.clock.monotonic()
        super().__init__(*args, **kwargs)
        RecordedPopen.started.append(self)


def run_emulated(bench, argv, tmp, knobs):
    """main(argv) as run_main() runs it, against bench: this module's serial, list_ports, time and LOGS (ROOT and
    DIST for build), the Logic 2 modules, the current folder, PATH, $BUZZPIRAT, stdin, subprocess.run and Popen and
    the random numbers are the scenario's, and as they were again afterwards. The knobs: no_serial, no pyserial;
    no_saleae, no Logic 2 API; stdin, cwd, path; build_fails, a build in tmp/root. Before the patches go, the
    threads and processes the command started are waited for: bench.running counts the commands still running when
    main() returned, bench.stuck names what did not end. Returns run_main()'s result."""
    from unittest import mock

    fakes = {"serial": None if knobs.get("no_serial") else bench.serial, "list_ports": bench.list_ports,
             "time": bench.clock, "LOGS": os.path.join(tmp, "logs")}
    if "build_fails" in knobs:  # build in tmp/root: the CMakeLists.txt of this program, a dist/ of an older build
        root = os.path.join(tmp, "root")
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(os.path.join(root, "dist"))
        shutil.copyfile(os.path.join(ROOT, "CMakeLists.txt"), os.path.join(root, "CMakeLists.txt"))
        for name in ("bp3unbricker-pico.uf2", "bp3unbricker-pico2.uf2", "SHA256SUMS.txt"):
            write_lines(os.path.join(root, "dist", name), ["from the last build"])
        fakes.update(ROOT=root, DIST=os.path.join(root, "dist"))
    modules = {"saleae": None, "saleae.automation": None} if knobs.get("no_saleae") else bench.logic2_modules()
    threads, rng = set(threading.enumerate()), random.getstate()
    RecordedPopen.started, RecordedPopen.clock = [], bench.clock
    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.dict(globals(), fakes))
        stack.enter_context(modules_replaced(modules))
        stack.enter_context(mock.patch.dict(os.environ, {"PATH": knobs["path"]} if "path" in knobs else {}))
        os.environ.pop("BUZZPIRAT", None)
        stack.enter_context(mock.patch.object(sys, "stdin", io.StringIO(knobs.get("stdin", ""))))
        stack.enter_context(mock.patch.object(subprocess, "Popen", RecordedPopen))
        stack.enter_context(mock.patch.object(subprocess, "run", bench.run))
        if "build_fails" in knobs:
            stack.enter_context(mock.patch.object(shutil, "which", lambda name, path=None: "fake-" + name))
        stack.callback(os.chdir, os.getcwd())
        stack.callback(random.setstate, rng)
        os.chdir(knobs.get("cwd", tmp))
        random.seed(12345)
        r = run_main(*argv)
        bench.running = sum(proc.poll() is None for proc in RecordedPopen.started)
        for thread in set(threading.enumerate()) - threads:  # term's reader ends on the emulated port's error
            if thread.is_alive():  # one that another thread is still starting is that thread's to join
                thread.join(30)
        bench.stuck = [thread.name for thread in set(threading.enumerate()) - threads if thread.is_alive()]
        for proc in RecordedPopen.started:
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:  # stopped here, so that tmp can go (Windows keeps a folder in use)
                proc.kill()
                proc.wait()
                bench.stuck.append(f"process {proc.args[0]}")
    return r


# ------------------------------------------------------------------ the scenarios

SLOW_S = 1  # how long the slow command of the sniff scenarios runs: Logic 2 fails or stops while it does
SNIFF_CODE = {  # what the commands that sniff and sniff-analog run do (Python), before they write <sniff>/done
    "prints": "import sys\nprint('to stdout')\nprint('to stderr', file=sys.stderr)",
    "fills": "print('\\n'.join(f'line {i}' for i in range(30000)))",  # far more than a pipe holds
    # the same, but it exits with 3 after 10 s if it is still blocked: a sniff that never read it would hang
    "fills-watchdog": "import os, threading\nt = threading.Timer(10, os._exit, [3])\nt.daemon = True\nt.start()\n"
                      "print('\\n'.join(f'line {i}' for i in range(30000)))",
    "slow": f"import time\ntime.sleep({SLOW_S})\nprint('after {SLOW_S} s')",
}
SNIFF, ANALOG = "sniff --out <sniff> --pre 0 ", "sniff-analog --out <sniff> "  # their command lines start so
SOAK = "--port COM7 soak --bp-port BP --buzzpirat <bz> --out <logs> --max-erases "
BOOT = "--port COM7 test-bootloader --bp-port BP --rounds 1 --buzzpirat <bz> --out <logs>"
CAPTURE = ["connect 10430", "start digital [0, 1, 2, 3] analog []"]  # sniff's capture of the default --map
SAVED = ["export csv [0, 1, 2, 3] None", "save capture.sal", "close"]
TRIGGERED = ["connect 10430", "start digital [0, 1, 2, 3] analog [0, 2, 3] trigger 0"]  # sniff-analog's
SNIFF_FILES = ["cmd_stderr.txt", "cmd_stdout.txt", "done", "run.json"]
SMALL_SENT = ["load small_cfg.hex", "flash loaded", "dump"]  # flash small_cfg.hex
NOT_FOUND = "ERROR: the command after -- cannot run: 'no-such-program' not found\n"
FILE_ERROR = "<file>: [Errno 2] "  # err1: one line '<FILE>: problem', FILE the last argument, as cmd_info() says it
ASCII_ONLY = "ERROR: the Pico's console takes ASCII only: 'speed r\u00e1pido'\n"
BROKEN = "<broken>: line 1: wrong record length\n"
NO_COMMAND = "ERROR: missing command after --\n"
REFUSED_3 = "ERROR: row 0 was refused 3 times (last answer: ERR 000 crc)\n"
RATE_REFUSED = "ERROR: Logic 2: Invalid supplied sample rate.\n"
NO_ANSWER = "ERROR: Logic 2: no answer on port {} (is it open, with its automation server on?)\n"
ID_FAILED = "ERROR: 'id' failed before the test, nothing was written (python bp3unbricker.py id shows why)\n"
BOOT_PAGES, BOOT_RUNS = "buzzloader wrote the 2 pages below the bootloader", "the firmware runs: "  # test-bootloader's
BOOT_HELLO, BOOT_SAME = "buzzloader --hello: the bootloader answers", "DEVID/DEVREV and calibration words unchanged"


def soak_records(p):
    """rounds.jsonl of the soak that just ran ([] without one)."""
    logs = glob.glob(os.path.join(p["logs"], "*", "rounds.jsonl"))
    if not logs:
        return []
    with open(logs[0]) as f:
        return [json.loads(line) for line in f]


def term_shown(r, bench, p):
    """term: each line of stdin went out with CR; stdout is the prompt, then everything the console said."""
    said = "> " + "".join(bench.console[2:]).replace("\n", "\r\n")  # after the banner and its prompt
    return [] if r.stdout == said and bench.written == [b"status\r", b"id\r"] else [f"stdout {r.stdout[-200:]!r}"]


def built(r, bench, p):
    """build: dist/ holds the UF2 of each board, described as build prints it, and SHA256SUMS.txt their hashes."""
    shown, sums = [], []
    for board in BUILD_BOARDS:
        path = os.path.join(p["root"], "dist", f"bp3unbricker-{board}.uf2")
        shown.append(f"{path}: 1024 bytes, 2 blocks, sha256 {file_sha256(path)}")
        sums.append(f"{file_sha256(path)}  {os.path.basename(path)}")
    with open(os.path.join(p["root"], "dist", "SHA256SUMS.txt")) as f:
        written = f.read().splitlines()
    return [] if r.stdout.splitlines() == shown and written == sums else [f"stdout {r.stdout!r}, sums {written}"]


def dist_kept(r, bench, p):
    """A build that fails: its ERROR line ends with cmake's exit status, and dist/ is as it was."""
    dist, kept = os.path.join(p["root"], "dist"), []
    for name in os.listdir(dist):
        with open(os.path.join(dist, name)) as f:
            kept.append(f.read() == "from the last build\n")
    ending = "-DPICO_BOARD=pico2']' returned non-zero exit status 1.\n"
    return [] if r.stderr.endswith(ending) and kept == [True] * 3 else ["dist/ changed"]


def reads_kept_retry(r, bench, p):
    """soak: round 4, 'reads', keeps the retry of the flash that put the image back first."""
    records = soak_records(p)
    kept = len(records) > 3 and "Attempt 1 failed: verify failed" in records[3]["anomalies"]
    return [] if kept else ["round 4 lost its flash's retry"]


CLI_SCENARIOS = [
    # name, what it shows, the command line (a list, or a string of the arguments), the bench's knobs, what must
    # come out:
    #   exit (0 when not given; "pyserial": the SystemExit of need_serial()); err: stderr exactly (empty when not
    #   given), err1: its one line starts so, err_end: it ends so; stdout: stdout exactly, start: it starts so, out:
    #   these are in it,
    #   not_out: these are not, shown: it is what the console printed for that command; cmds: the console commands
    #   the Pico got, speed: the first is 'speed X', the last 'speed normal'; opened: the ports opened; chip, pic:
    #   what the PIC holds at the end, its state; rows, aborts: upload lines R and X sent; loaded: the image the Pico
    #   holds; logic2, started, files, run_json: what sniff did with Logic 2, the commands it started, what is left
    #   in --out (None: no folder), what run.json says; logs: the files of the log folder; fails, count: the checks
    #   of a test that fail, their number; summary: lines of summary.txt (printed too); rounds: soak's verdict per
    #   round (+/-) and the erases kept under --max-erases; buzzloader: its runs; check: a function for the rest.
    # On every scenario: no command sniff started still runs when main() returns, and every thread and process the
    # command started ends. <name> in a string is p[name] of cli_inputs(): a file of the scenarios, or a fact of the
    # built-in image.
    # ------------------------------------------------------------------ the console's own commands, run and term
    ("status", "the console's lines, without its echo and prompt", "status", {},
     {"cmds": ["status"], "shown": "status"}),
    ("id", "the target and its calibration words", "id", {}, {"cmds": ["id"], "shown": "id"}),
    ("blank-not-blank", "blank on a programmed PIC: exit 1", "blank", {},
     {"exit": 1, "cmds": ["blank"], "shown": "blank"}),
    ("erase", "erase leaves the PIC erased", "erase", {}, {"cmds": ["erase"], "shown": "erase", "chip": "erased"}),
    ("jumper", "jumper: the bootloader waits", "jumper", {},
     {"cmds": ["jumper"], "shown": "jumper", "pic": "bootloader"}),
    ("release", "release: the PIC runs", "release", {"pic": "bootloader"},
     {"cmds": ["release"], "shown": "release", "pic": "running"}),
    ("release-unpowered", "release on a board without power: the console says so, exit 0", "release",  # P#24
     {"unpowered": True}, {"cmds": ["release"], "shown": "release"}),
    ("status-last-op", "status after an operation that failed: its summary, exit 0", "status",
     {"before": ["flash"], "fail": {"flash": "verify failed"}}, {"cmds": ["status"], "shown": "status"}),
    ("status-last-ok", "status after an operation that worked: its summary, exit 0", "status", {"before": ["id"]},
     {"cmds": ["status"], "shown": "status"}),
    ("status-unpowered", "status on a board without power: NOT detected, exit 0", "status", {"unpowered": True},
     {"cmds": ["status"], "shown": "status"}),
    ("blank", "blank on an erased PIC: exit 0", "blank", {"chip": "erased"}, {"cmds": ["blank"], "shown": "blank"}),
    ("id-fails", "an id that fails: exit 1", "id", {"ids": ["wrong or missing device ID (check wiring)"]},
     {"exit": 1, "cmds": ["id"], "shown": "id"}),
    ("log-none", "log before any operation: exit 0", "log", {}, {"cmds": ["log"], "shown": "log"}),
    ("log-last-failed", "log: the exit status of the operation it shows (failed: 1)", "log",
     {"before": ["flash"], "fail": {"flash": "verify failed"}}, {"exit": 1, "cmds": ["log"], "shown": "log"}),
    ("log-last-ok", "log: the exit status of the operation it shows (OK: 0)", "log", {"before": ["id"]},
     {"cmds": ["log"], "shown": "log"}),
    ("run-help", "run: a console command, its lines shown", "run help", {}, {"cmds": ["help"], "shown": "help"}),
    ("run-flash", "run flash: exit 0 on its OK result line", "run flash", {"chip": "erased"},
     {"cmds": ["flash"], "shown": "flash", "chip": "image"}),
    ("run-unknown", "run with a command the console does not know: its answer, exit 0", "run bogus", {},
     {"cmds": ["bogus"], "shown": "bogus"}),
    ("run-usage", "run with an argument the command does not take: its usage line, exit 0", ["run", "flash bogus"],
     {}, {"cmds": ["flash bogus"], "shown": "flash bogus"}),
    ("run-speed-usage", "run speed with another speed: the usage line and the speed, exit 0",
     ["run", "speed medium"], {}, {"cmds": ["speed medium"], "shown": "speed medium"}),
    ("run-non-ascii", "run refuses a non-ASCII command before the port is opened (exit 2)",  # G5 P#4
     ["run", "speed r\u00e1pido"], {}, {"exit": 2, "err": ASCII_ONLY, "cmds": [], "opened": []}),
    ("term", "term: each stdin line goes out with CR, all the console says is shown", "term",
     {"stdin": "status\nid\n"}, {"cmds": ["status", "id"], "check": term_shown}),
    # ------------------------------------------------------------------ finding the Pico
    ("no-pico", "no Pico: one ERROR line (exit 1)", "status", {"ports": []},
     {"exit": 1, "err": "ERROR: no Raspberry Pi Pico found (is bp3unbricker running on it? try --port)\n",
      "opened": []}),
    ("two-picos", "two Picos: --port is needed (exit 1)", "status", {"ports": ["COM7", "COM8"]},
     {"exit": 1, "err": "ERROR: several Raspberry Pi serial ports (COM7, COM8): choose one with --port\n",
      "opened": []}),
    ("port-option-missing", "--port that cannot be opened: one ERROR line (exit 1)", "--port COM9 status", {},
     {"exit": 1, "err1": "ERROR: cannot open COM9: could not open port 'COM9'", "cmds": []}),  # Linux adds a hint
    ("port-option", "--port picks one of two Picos", "--port COM8 status", {"ports": ["COM7", "COM8"]},
     {"cmds": ["status"], "opened": ["COM8"]}),
    # ------------------------------------------------------------------ flash
    ("flash-builtin", "flash: the built-in image", "flash", {"chip": "erased"},
     {"cmds": ["flash"], "shown": "flash", "chip": "image"}),
    ("flash-builtin-speed", "--speed for the run, normal again after it", "--speed fast flash", {"chip": "erased"},
     {"cmds": ["speed fast", "flash", "speed normal"], "shown": "flash", "chip": "image"}),
    ("flash-builtin-fails", "a flash that fails: exit 1", "flash", {"fail": {"flash": NO_RESPONSE}},
     {"exit": 1, "cmds": ["flash"], "shown": "flash"}),
    ("flash-file", "flash FILE: described, sent, written, read back", "flash <hex>", {"chip": "erased"},
     {"cmds": ["load <upload>", "flash loaded", "dump"], "rows": "<image_rows>", "chip": "<hex>", "loaded": "<upload>",
      "out": ["Config     : <image_cw>\n", "Sending <image> to the Pico...\n", "  LOAD OK <upload> crc32 <image_crc>, "
              "<image_rows> rows used (", "READBACK OK: all 22016 words of the PIC match <image>\n"]}),
    ("flash-file-speed-slow", "--speed with flash FILE", "--speed slow flash <hex>", {"chip": "erased"},
     {"cmds": ["speed slow", "load <upload>", "flash loaded", "dump", "speed normal"], "chip": "<hex>"}),
    ("flash-file-noreadback", "--no-readback: no dump", "flash <hex> --no-readback", {"chip": "erased"},
     {"cmds": ["load <upload>", "flash loaded"], "chip": "<hex>", "not_out": ["READBACK"]}),
    ("flash-small-noconfig", "a FILE without configuration words is refused (exit 2)", "flash <nocfg>", {},
     {"exit": 2, "err": "<nocfg>: the file has no configuration words (CW1/CW2 at PC 0x00ABFE/0x00ABFC).\n  Erased, "
      "they leave the watchdog on and the clock without PLL: a Bus Pirate\n  firmware will not run. Use --add-config "
      "to add the Bus Pirate v3 values\n  (CW2=0xF9DF CW1=0x3F7F), or --no-config to program the file as it is.\n",
      "cmds": [], "opened": []}),
    ("flash-small-addconfig", "--add-config: the Bus Pirate v3 words added", "flash <nocfg> --add-config", {},
     {"cmds": ["load small_nocfg.hex", "flash loaded", "dump"], "rows": 3, "chip": ("<nocfg>", "add"),
      "out": ["Note       : configuration words added (Bus Pirate v3): CW2=0xF9DF CW1=0x3F7F\n",
              "READBACK OK: all 22016 words of the PIC match small_nocfg.hex\n"]}),
    ("flash-small-noconfig-opt", "--no-config: left erased", "flash <nocfg> --no-config", {},
     {"cmds": ["load small_nocfg.hex", "flash loaded", "dump"], "rows": 2, "chip": ("<nocfg>", "none"),
      "out": ["Config     : CW2=0xFFFF CW1=0xFFFF (erased)\n"]}),
    ("flash-both-config-opts", "--add-config with --no-config (exit 2)", "flash <nocfg> --add-config --no-config", {},
     {"exit": 2, "err": "<nocfg>: --add-config and --no-config exclude each other\n", "cmds": [], "opened": []}),
    ("flash-risky", "risky configuration words: described, refused without --force (exit 2)", "flash <risky>", {},
     {"exit": 2, "err": "<risky>: the configuration words above would leave the Bus Pirate unusable; check the file, "
      "or pass --force to write them anyway\n", "cmds": [], "opened": [],
      "out": ["WARNING    : CW1 GCP=0: code protection, the PIC cannot be read or verified after a reset\n"]}),
    ("flash-risky-force", "--force writes them", "flash <risky> --force", {},
     {"cmds": ["load risky_gcp0.hex", "flash loaded", "dump"], "chip": "<risky>"}),
    ("flash-odd-name", "a name the Pico changes: '_' for the rest", "flash <odd> --add-config", {},
     {"cmds": ["load my_fw_(_)_v1+beta.hex", "flash loaded", "dump"], "loaded": "my_fw_(_)_v1+beta.hex",
      "chip": ("<odd>", "add")}),
    ("flash-long-name", "a name the Pico cuts at 47 characters", "flash <long> --add-config", {},
     {"cmds": ["load a-very-long-firmware-file-name-that-goes-past-f", "flash loaded", "dump"],
      "loaded": "a-very-long-firmware-file-name-that-goes-past-f", "chip": ("<long>", "add")}),
    ("flash-missing-file", "a FILE that does not exist (exit 2)", "flash <missing>", {},
     {"exit": 2, "err1": FILE_ERROR, "cmds": [], "opened": []}),
    ("flash-row-resent", "a row refused once is sent again", "flash <small>", {"refuse": {0: 1}},
     {"cmds": SMALL_SENT, "rows": 4, "chip": "<small>", "out": ["  row 0: ERR 000 crc, sending it again (1)\n"]}),
    ("flash-row-resent-twice", "twice", "flash <small>", {"refuse": {0: 2}},
     {"cmds": SMALL_SENT, "rows": 5, "chip": "<small>", "out": ["sending it again (2)\n"]}),
    ("flash-row-refused", "refused 3 times: X, one ERROR line with the last answer", "flash <small>",  # G5 P#2
     {"refuse": {0: 3}}, {"exit": 1, "err": REFUSED_3, "cmds": ["load small_cfg.hex"], "rows": 3, "aborts": 1,
                          "chip": "image", "out": ["sending it again (2)\n"], "not_out": ["sending it again (3)"]}),
    ("flash-row-weird", "an answer that is no reply: X, one ERROR line", "flash <small>", {"odd_reply": 0},
     {"exit": 1, "err": "ERROR: row 0: unexpected answer 'HUH?'\n", "cmds": ["load small_cfg.hex"], "rows": 1,
      "aborts": 1, "chip": "image"}),
    ("flash-image-crc-mismatch", "the Pico writes another image: exit 1, no read-back", "flash <small>",
     {"image_crc": 0x12345678}, {"exit": 1, "cmds": SMALL_SENT[:2],
                                 "out": ["ERROR: the Pico wrote an image with crc32 12345678, not B514D3CE\n"]}),
    ("flash-loaded-fails", "flash loaded fails: its lines shown, exit 1, no read-back", "flash <small>",
     {"fail": {"flash loaded": "verify failed"}}, {"exit": 1, "cmds": SMALL_SENT[:2], "chip": "image",
                                                   "out": ["[flash loaded: verify failed, 1234 ms]\n"]}),
    ("flash-readback-damaged-record", "a damaged record in the read (exit 1)", "flash <small>", {"dump": "record"},
     {"exit": 1, "err": "ERROR: damaged record in the read: :10063000FFFFFF00FFFFFF00FFFFFF00FFFFFF0000\n",
      "chip": "<small>", "cmds": SMALL_SENT}),
    ("flash-readback-missing-record", "a record missing from the read (exit 1)", "flash <small>", {"dump": "missing"},
     {"exit": 1, "err": "ERROR: the read does not cover every word exactly once\n", "cmds": SMALL_SENT,
      "chip": "<small>"}),
    ("flash-readback-bad-crc", "a read whose crc32 does not match (exit 1)", "flash <small>", {"dump": "crc"},
     {"exit": 1, "err": "ERROR: the read was damaged on the way (crc32 B514D3CE, the Pico sent B514D3CF)\n",
      "cmds": SMALL_SENT, "chip": "<small>"}),
    ("flash-readback-bad-endline", "an END line without crc32: a read error, no traceback", "flash <small>",  # G5 P#3
     {"dump": "endline"}, {"exit": 1, "err": "ERROR: the read failed: no crc32 in 'END HEX words=22016'\n",
                           "cmds": SMALL_SENT, "chip": "<small>"}),
    ("flash-readback-mismatch", "a word the PIC does not keep: shown, exit 1", "flash <small>", {"flip": 0x100},
     {"exit": 1, "cmds": SMALL_SENT, "out": ["  PC 0x000200: small_cfg.hex has 37FFFF, the PIC has 37FEFF\n",
                                             "READBACK FAILED: 1 words differ from small_cfg.hex\n"]}),
    # ------------------------------------------------------------------ verify and read
    ("verify-builtin", "verify: the Pico's own compare", "verify", {}, {"cmds": ["verify"], "shown": "verify"}),
    ("verify-builtin-differs", "verify with another image on the PIC: exit 1", "verify", {"chip": "bootloader"},
     {"exit": 1, "cmds": ["verify"], "shown": "verify"}),
    ("verify-file-same", "verify FILE: a full read compared here", "verify <hex>", {},
     {"cmds": ["dump"], "out": ["READBACK OK: all 22016 words of the PIC match <image>\n"]}),
    ("verify-file-differs", "the first 10 differences, the count, exit 1", "verify <small>", {},
     {"exit": 1, "cmds": ["dump"], "not_out": ["<small_next>"],
      "out": ["Reading the whole PIC...\n<small_shown>READBACK FAILED: <small_differ> words differ from "
              "small_cfg.hex\n"]}),
    ("verify-risky-no-force-needed", "verify takes risky configuration words", "verify <risky>", {},
     {"exit": 1, "cmds": ["dump"], "out": ["READBACK FAILED: <risky_differ> words differ from risky_gcp0.hex\n"]}),
    ("read", "read OUT: the PIC as Intel HEX, checked", "read <read>", {"chip": "bootloader"},
     {"cmds": ["dump"], "out": ["saved <read>: 22016 words, crc32 <bl_crc> (checked)\n"],
      "check": lambda r, bench, p: [] if Bench.stored(load_image(p["read"], "file")[0].words) == bench.chip
      else ["OUT is not the PIC"]}),
    ("read-damaged", "a damaged read is not saved (exit 1)", "read <read>", {"dump": "crc"},
     {"exit": 1, "err": "ERROR: the read was damaged on the way (crc32 <image_crc>, the Pico sent <image_crc1>)\n",
      "check": lambda r, bench, p: ["OUT was written"] if os.path.exists(p["read"]) else []}),
    # ------------------------------------------------------------------ bootloader
    ("bootloader", "bootloader: the Pico's CRCs checked, written, read back", "bootloader", {},
     {"cmds": ["status", "bootloader", "dump"], "chip": "bootloader",
      "out": ["READBACK OK: all 22016 words of the PIC match <bl>\n",
              "Only the bootloader is on the PIC: <bl_words> words, the other <bl_erased> erased.\n"]}),
    ("bootloader-noreadback", "--no-readback: no dump", "bootloader --no-readback", {},
     {"cmds": ["status", "bootloader"], "chip": "bootloader", "not_out": ["READBACK"]}),
    ("bootloader-speed", "--speed with bootloader", "--speed fast bootloader", {},
     {"cmds": ["speed fast", "status", "bootloader", "dump", "speed normal"], "chip": "bootloader"}),
    ("bootloader-old-firmware", "a firmware without 'bootloader': install first (exit 1)", "bootloader",
     {"old_firmware": True}, {"exit": 1, "cmds": ["status"], "chip": "image", "out": [
         "ERROR: the Pico's firmware has no usable 'bootloader' command: update it (install)\n"]}),
    ("bootloader-name-with-space", "an image name with a space", "bootloader", {"status_name": "Bus Pirate fw.hex"},
     {"cmds": ["status", "bootloader", "dump"], "chip": "bootloader", "out": ["READBACK OK: all 22016 words"]}),  # G5
    ("bootloader-other-image", "a Pico built from another bp3unbricker.c (exit 1)", "bootloader",
     {"status_crc": 0x0BADF00D}, {"exit": 1, "cmds": ["status"], "chip": "image", "out": [
         f"ERROR: {SOURCE} does not hold the image built into the Pico (crc32 <image_crc>, the Pico's is "
         "0BADF00D)\n"]}),
    ("bootloader-wrong-crc-written", "the Pico writes another image (exit 1)", "bootloader", {"image_crc": 0x12345678},
     {"exit": 1, "cmds": ["status", "bootloader"],
      "out": ["ERROR: the Pico did not write the bootloader image (crc32 <bl_crc>)\n"]}),
    ("bootloader-fails", "bootloader fails: its lines shown, exit 1, no read-back", "bootloader",
     {"fail": {"bootloader": NO_TARGET}}, {"exit": 1, "cmds": ["status", "bootloader"], "chip": "image",
                                           "out": [f"[bootloader: {NO_TARGET}, 1234 ms]\n"]}),
    ("bootloader-readback-mismatch", "a word of the bootloader the PIC does not keep (exit 1)", "bootloader",
     {"flip": 0xA900 // 2}, {"exit": 1, "out": ["  PC 0x00A900: <bl> has <bl_word>, the PIC has <bl_flipped>\n",
                                                "READBACK FAILED: 1 words differ from <bl>\n"]}),
    # ------------------------------------------------------------------ the Bus Pirate's own port
    ("bp-info", "bp info: the firmware's 'i'", "bp BP info", {},
     {"stdout": EmulatedBusPirate.INFO.decode(), "opened": ["BP"]}),
    ("bp-info-silent", "bp info, no firmware: <nothing>, exit 1", "bp BP info", {"chip": "bootloader"},
     {"exit": 1, "stdout": "<nothing>\n"}),
    ("bp-hello-firmware", "bp hello to a firmware: its reply, exit 1", "bp BP hello", {},
     {"exit": 1, "stdout": "reply: " + b"\xc1\r\nSyntax error at char 1\r\nHiZ>".hex(" ") + "\n"}),
    ("bp-hello-bootloader", "bp hello to the bootloader: id and version", "bp BP hello", {"pic": "bootloader"},
     {"stdout": "reply: d4 04 45 4b\nbootloader: device id 0xD4, version 4.4.5\n"}),
    ("bp-raw", "bp raw TEXT", "bp BP raw v", {}, {"stdout": "v\r\nSyntax error at char 1\r\nHiZ>"}),
    ("bp-raw-no-text", "bp raw without TEXT: a newline", "bp BP raw", {}, {"stdout": "\r\nHiZ>"}),
    ("bp-boot", "bp boot: the bootloader starts the firmware", "bp BP boot", {"pic": "bootloader"},
     {"stdout": EmulatedBusPirate.INFO.decode(), "pic": "running"}),
    ("bp-boot-after-hello", "bp boot right after a HELLO: it waits until the bootloader is back at its HELLO stage",
     "bp BP boot", {"pic": "bootloader", "hello_sent": True}, {"stdout": EmulatedBusPirate.INFO.decode(),
                                                               "pic": "running"}),
    # ------------------------------------------------------------------ test-loader (G7 README#2, P#20)
    ("test-loader", "test-loader: its 22 checks pass", "test-loader", {},
     {"fails": [], "count": 22, "out": ["  ok    every row -> OK\n", "ALL UPLOAD TESTS PASSED\n"], "aborts": 1,
      "loaded": "<upload>", "cmds": ["load faults", "status", "flash loaded", "load abort", "status", "load silent",
                                     "status", "load <upload>", "status", "verify loaded"]}),
    ("test-loader-file", "test-loader FILE", "test-loader <hex>", {},
     {"fails": [], "count": 22, "out": ["  ok    every row -> OK\n"]}),
    ("test-loader-speed", "test-loader takes --speed and sets it back", "--speed slow test-loader", {},
     {"fails": [], "speed": "slow", "out": ["ALL UPLOAD TESTS PASSED\n"]}),
    ("test-loader-speed-error", "...also when it fails: exit 1, the ERROR line", "--speed slow test-loader",
     {"refuse": {0: 3}}, {"exit": 1, "speed": "slow", "err": REFUSED_3,
                          "fails": ["then every real row -> OK", "every row -> OK"]}),
    ("test-loader-missing-file", "test-loader refuses a FILE that does not exist (exit 2)",  # G5 P#13
     "test-loader <missing>", {}, {"exit": 2, "err1": FILE_ERROR, "cmds": [], "opened": []}),
    ("test-loader-bad-file", "a damaged FILE (exit 2)", "test-loader <broken>", {},
     {"exit": 2, "err": BROKEN, "cmds": [], "opened": []}),
    # ------------------------------------------------------------------ bad files and usage before pyserial (G5, FU-P8)
    ("test-loader-bad-file-no-pyserial", "test-loader: FILE checked before pyserial", "test-loader <broken>",
     {"no_serial": True}, {"exit": 2, "err": BROKEN, "cmds": []}),
    ("flash-bad-file-no-pyserial", "flash: FILE first", "flash <missing>", {"no_serial": True},
     {"exit": 2, "err1": FILE_ERROR, "cmds": []}),
    ("verify-bad-file-no-pyserial", "verify: FILE first", "verify <broken>", {"no_serial": True},
     {"exit": 2, "err": BROKEN, "cmds": []}),
    ("flash-file-no-pyserial", "a good FILE is described, then pyserial is missing", "flash <small>",
     {"no_serial": True}, {"exit": "pyserial", "out": ["Config     : CW2=0xF9DF CW1=0x3F7F\n"], "cmds": []}),
    ("run-non-ascii-no-pyserial", "run: COMMAND first", ["run", "speed r\u00e1pido"], {"no_serial": True},
     {"exit": 2, "err": ASCII_ONLY, "cmds": []}),
    ("status-no-pyserial", "status: pyserial is missing, nothing else said", "status", {"no_serial": True},
     {"exit": "pyserial", "stdout": ""}),
    ("soak-speed-no-pyserial", "soak refuses --speed first (exit 2)", "--speed fast soak --bp-port BP",
     {"no_serial": True}, {"exit": 2, "err": "ERROR: soak runs every speed in turn: --speed does not apply\n",
                           "cmds": []}),
    ("soak-no-hex-no-pyserial", "soak without a usable .hex, before pyserial (exit 2)",
     "soak --bp-port BP --buzzpirat <nohex>", {"no_serial": True},
     {"exit": 2, "err": f"ERROR: no usable .hex file under {os.path.join('<nohex>', 'bin')} for the hexfile rounds\n",
      "cmds": []}),
    ("test-bootloader-no-pyserial", "test-bootloader without a buzzpirat clone, before pyserial (exit 2)",
     "test-bootloader --bp-port BP --buzzpirat <nobz>", {"no_serial": True},
     {"exit": 2, "err": "ERROR: where is buzzpirat (https://github.com/therealdreg/buzzpirat)? --buzzpirat DIR or the "
      "BUZZPIRAT environment variable\n", "cmds": []}),
    # ------------------------------------------------------------------ sniff and sniff-analog against Logic 2
    ("sniff", "sniff: the capture around the command, what it printed saved", SNIFF + "--post 0 -- <py:prints>", {},
     {"err": "to stderr\n\n", "start": "to stdout\n", "started": 1, "logic2": CAPTURE + ["stop"] + SAVED,
      "files": SNIFF_FILES, "run_json": {"map": SALEAE_CHANNEL_MAP, "returncode": 0}}),
    ("sniff-map", "--map with PGD and PGC swapped: the map in run.json, for decode",  # P#26
     SNIFF + "--post 0 --map 0=MCLR,1=VDD,2=PGC,3=PGD -- <py:prints>", {},
     {"err": "to stderr\n\n", "logic2": CAPTURE + ["stop"] + SAVED, "files": SNIFF_FILES,
      "run_json": {"map": "0=MCLR,1=VDD,2=PGC,3=PGD"}}),
    ("sniff-map-elsewhere", "--map on other channels, without VDD: those channels captured",
     SNIFF + "--post 0 --map 5=MCLR,7=PGD,6=PGC -- <py:prints>", {},
     {"err": "to stderr\n\n", "logic2": ["connect 10430", "start digital [5, 6, 7] analog []", "stop",
                                         "export csv [5, 6, 7] None", "save capture.sal", "close"]}),
    ("sniff-rate-map", "--analog-rate with a --map: the analog channels are the map's",
     SNIFF + "--post 0 --map 5=MCLR,7=PGD,6=PGC --rate 125000000 --analog-rate 50000000 -- <py:prints>", {},
     {"err": "to stderr\n\n", "logic2": ["connect 10430", "start digital [5, 6, 7] analog [5, 6, 7]", "stop",
                                         "export csv [5, 6, 7] [5, 6, 7]", "save capture.sal", "close"],
      "files": SNIFF_FILES, "run_json": {"map": "5=MCLR,7=PGD,6=PGC", "analog_rate": 50_000_000}}),
    ("sniff-port-0", "--saleae-port 0: not a TCP port, a usage error before anything (exit 2)",
     SNIFF + "--post 0 --saleae-port 0 -- <py:prints>", {},
     {"exit": "SystemExit: 2", "err_end": "argument --saleae-port: 0 is not a TCP port (1 to 65535)\n", "logic2": [],
      "files": None, "started": 0}),
    ("sniff-no-command", "sniff without a command (exit 2)", "sniff --out <sniff>", {},
     {"exit": 2, "err": NO_COMMAND, "logic2": [], "started": 0}),
    ("sniff-fills-pipe", "a command that prints more than a pipe holds, --max-seconds",  # G6 P#5
     SNIFF + "--max-seconds 3 -- <py:fills>", {},
     {"err": "\n", "out": ["line 29999\n"], "started": 1, "logic2": CAPTURE + ["stop"] + SAVED}),
    ("sniff-fills-pipe-no-max-seconds", "the same without --max-seconds", SNIFF + "--post 0 -- <py:fills-watchdog>",
     {}, {"err": "\n", "out": ["line 29999\n", '"returncode": 0}'], "logic2": CAPTURE + ["stop"] + SAVED}),
    ("sniff-max-seconds", "--max-seconds stops the capture, the command runs on",
     SNIFF + "--max-seconds 0.3 -- <py:slow>", {},
     {"err": "\n", "out": [f"after {SLOW_S} s\n"], "logic2": CAPTURE + ["stop (command running)"] + SAVED,
      "files": SNIFF_FILES}),
    ("sniff-analog", "sniff-analog: its default rates a pair the Logic Pro 8 takes, MCLR, PGD and PGC analog",
     ANALOG + "-- <py:prints>", {},
     {"err": "to stderr\n\n", "out": ['{"returncode": 0, "files": ' + json.dumps(SNIFF_FILES) + "}\n"],
      "logic2": TRIGGERED + ["wait", "export binary [0, 1, 2, 3] [0, 2, 3]", "save capture.sal", "close"],
      "run_json": {"map": SALEAE_CHANNEL_MAP, "analog": [0, 2, 3]}}),
    ("sniff-analog-map", "--map without VDD: those channels, MCLR's the trigger, MCLR's, PGD's, PGC's analog",  # P#26
     ANALOG + "--map 6=MCLR,4=PGD,5=PGC -- <py:prints>", {},
     {"err": "to stderr\n\n", "logic2": ["connect 10430", "start digital [4, 5, 6] analog [4, 5, 6] trigger 6", "wait",
                                         "export binary [4, 5, 6] [4, 5, 6]", "save capture.sal", "close"],
      "files": SNIFF_FILES, "run_json": {"map": "6=MCLR,4=PGD,5=PGC", "analog": [4, 5, 6]}}),
    ("sniff-analog-channels", "--analog with a --map: those analog channels, MCLR's the trigger",
     ANALOG + "--map 6=MCLR,4=PGD,5=PGC --analog 4 -- <py:prints>", {},
     {"err": "to stderr\n\n", "logic2": ["connect 10430", "start digital [4, 5, 6] analog [4] trigger 6", "wait",
                                         "export binary [4, 5, 6] [4]", "save capture.sal", "close"],
      "files": SNIFF_FILES, "run_json": {"map": "6=MCLR,4=PGD,5=PGC", "analog": [4]}}),
    ("sniff-analog-port-65536", "--saleae-port 65536: not a TCP port, a usage error before anything (exit 2)",
     ANALOG + "--saleae-port 65536 -- <py:prints>", {},
     {"exit": "SystemExit: 2", "err_end": "argument --saleae-port: 65536 is not a TCP port (1 to 65535)\n",
      "logic2": [], "files": None, "started": 0}),
    ("sniff-analog-no-command", "sniff-analog without a command (exit 2)", ANALOG, {},  # G6 P#6
     {"exit": 2, "err": NO_COMMAND, "logic2": [], "started": 0}),
    ("sniff-relative-path", "the command runs in the caller's folder", SNIFF + "--post 0 -- <python> sniff_here.py",
     {"cwd": "<here>"}, {"err": "\n", "start": "found by its relative path\n", "files": SNIFF_FILES}),  # FU-P2
    ("sniff-analog-relative-path", "the same for sniff-analog", ANALOG + "-- <python> sniff_here.py",
     {"cwd": "<here>"}, {"err": "\n", "start": "found by its relative path\n", "files": SNIFF_FILES}),
    ("sniff-program-not-found", "a program that cannot be found: refused before the capture (exit 2)",  # FU-P3
     SNIFF + "-- no-such-program id", {}, {"exit": 2, "err": NOT_FOUND, "logic2": [], "files": None, "started": 0}),
    ("sniff-analog-program-not-found", "the same for sniff-analog", ANALOG + "-- no-such-program id", {},
     {"exit": 2, "err": NOT_FOUND, "logic2": [], "files": None, "started": 0}),
    # REV-1: Windows also looks for 'python' next to this Python; elsewhere only PATH counts
    ("sniff-python-off-path", "'python' outside PATH, next to this Python: Windows runs it, others refuse it",
     SNIFF + "--post 0 -- python -c <code:prints>", {"path": "<empty>"},
     {"err": "to stderr\n\n", "files": SNIFF_FILES} if os.name == "nt" else
     {"exit": 2, "err": "ERROR: the command after -- cannot run: 'python' not found\n", "files": None}),
    ("sniff-analog-program-cannot-start", "found but cannot start: the capture ends at once (exit 1)",  # REV-2
     ANALOG + "-- batch_only", {"path": "<batch>"},
     {"exit": 1, "err1": "ERROR: ", "logic2": TRIGGERED + ["close"], "started": 0}),
    ("sniff-no-command-no-saleae", "the command is checked before the Saleae API is needed",  # REV-10
     "sniff --out <sniff>", {"no_saleae": True}, {"exit": 2, "err": NO_COMMAND, "logic2": []}),
    ("sniff-analog-program-not-found-no-saleae", "the same for sniff-analog", ANALOG + "-- no-such-program id",
     {"no_saleae": True}, {"exit": 2, "err": NOT_FOUND}),
    # P#21, P#22: Logic 2 refuses the options or fails, or gRPC does: one ERROR line, the first sentence of the
    # message; the command never started, or it had and was waited for (<sniff>/done)
    ("sniff-analog-rate-refused", "a rate pair Logic 2 refuses (exit 2)", ANALOG + "--rate 100000000 -- <py:prints>",
     {}, {"exit": 2, "err": RATE_REFUSED, "logic2": TRIGGERED, "started": 0, "files": []}),
    ("sniff-rate-refused", "the same for sniff --analog-rate", SNIFF + "--post 0 --analog-rate 50000000 -- <py:prints>",
     {}, {"exit": 2, "err": RATE_REFUSED, "started": 0, "files": [],
          "logic2": ["connect 10430", "start digital [0, 1, 2, 3] analog [0, 1, 2, 3]"]}),
    ("sniff-incompatible-logic2", "an API error without a message: its name (exit 1)",
     SNIFF + "--post 0 -- <py:prints>", {"logic2_error": {"connect": "IncompatibleApiVersionError"}},
     {"exit": 1, "err": "ERROR: Logic 2: IncompatibleApiVersionError\n", "logic2": ["connect 10430"], "started": 0,
      "files": []}),
    ("sniff-analog-device-error", "the device fails during the capture: the command is waited for (exit 1)",
     ANALOG + "--rate 125000000 -- <py:slow>",
     {"logic2_error": {"wait": "DeviceError: USB read timeout. The device was disconnected."}},
     {"exit": 1, "err": "ERROR: Logic 2: USB read timeout.\n", "logic2": TRIGGERED + ["wait", "close"], "started": 1,
      "files": ["done"]}),
    ("sniff-buffer-full", "the buffer fills when the capture stops (exit 1)", SNIFF + "--max-seconds 0.3 -- <py:slow>",
     {"logic2_error": {"stop": "OutOfMemoryError: Capture buffer full. The capture was stopped."}},
     {"exit": 1, "err": "ERROR: Logic 2: Capture buffer full.\n", "started": 1, "files": ["done"],
      "logic2": CAPTURE + ["stop (command running)", "close"]}),
    ("sniff-no-logic2", "nothing answers on --saleae-port (exit 1)",
     SNIFF + "--post 0 --saleae-port 10431 -- <py:prints>",
     {"logic2_error": {"connect": ("UNAVAILABLE", "failed to connect to all addresses")}},
     {"exit": 1, "err": NO_ANSWER.format(10431), "logic2": ["connect 10431"], "started": 0, "files": []}),
    ("sniff-analog-logic2-closed", "Logic 2 closed during the capture (exit 1)", ANALOG + "-- <py:slow>",
     {"logic2_error": {"wait": ("UNAVAILABLE", "Socket closed")}},
     {"exit": 1, "err": NO_ANSWER.format(10430), "logic2": TRIGGERED + ["wait", "close"], "started": 1,
      "files": ["done"]}),
    ("sniff-grpc-error", "another gRPC status: its name and the first sentence of its details (exit 1)",
     SNIFF + "--max-seconds 0.3 -- <py:slow>",
     {"logic2_error": {"stop": ("CANCELLED", "Received RST_STREAM with error code 8. The stream was reset.")}},
     {"exit": 1, "err": "ERROR: Logic 2: CANCELLED: Received RST_STREAM with error code 8.\n", "started": 1,
      "logic2": CAPTURE + ["stop (command running)", "close"], "files": ["done"]}),
    ("sniff-analog-grpc-no-details", "a gRPC status without details: its name alone (exit 1)",
     ANALOG + "-- <py:slow>", {"logic2_error": {"wait": ("INTERNAL", "")}},
     {"exit": 1, "err": "ERROR: Logic 2: INTERNAL\n", "logic2": TRIGGERED + ["wait", "close"], "started": 1,
      "files": ["done"]}),
    # ------------------------------------------------------------------ build with cmake
    ("build", "build: both boards, then dist/ and SHA256SUMS.txt", "build", {"build_fails": None},
     {"cmds": ["configure pico", "build pico", "configure pico2", "build pico2"], "check": built}),
    ("build-pico2-fails", "a board that fails: one ERROR line, dist/ as it was (exit 1)", "build",
     {"build_fails": "pico2"}, {"exit": 1, "err1": "ERROR: Command '['fake-cmake', '-G', 'Ninja', '-S'",
                                "cmds": ["configure pico", "build pico", "configure pico2"], "check": dist_kept}),
    # ------------------------------------------------------------------ soak (G7 P#15-P#20)
    ("soak", "soak: every operation at every speed, a firmware that answers", SOAK + "21", {},
     {"rounds": "++++++++++++++", "summary": ["rounds: 14, failed: 0, chip erases: 15"], "out": ["bus_pirate="],
      "chip": "image"}),
    ("soak-retry", "a retry is an anomaly: that round fails", SOAK + "14", {"attempts": {1: 2}},
     {"exit": 1, "rounds": "-+++++"}),
    ("soak-flip", "a word wrong after every flash, the restoring flash of 'reads' too", SOAK + "14", {"flip": 0x100},
     {"exit": 1, "rounds": "------", "summary": ["rounds: 6, failed: 6"]}),
    ("soak-first-id-fails", "the first id fails: stop before writing anything (exit 1)", SOAK + "9",
     {"ids": [NO_RESPONSE, (0x3046, 0)]}, {"exit": 1, "err": ID_FAILED, "cmds": ["status", "id"], "logs": []}),
    ("soak-other-devrev", "another silicon revision is fine", SOAK + "9", {"ids": [(0x3042, 0)]},
     {"rounds": "++", "summary": ["rounds: 2, failed: 0, chip erases: 4"]}),
    ("soak-calibration-changes", "calibration words that change fail every round", SOAK + "9",
     {"ids": [(0x3046, 0), (0x3046, 1)]},
     {"exit": 1, "rounds": "--", "summary": ["DEVID/DEVREV and calibration words unchanged after every round: False"]}),
    ("soak-no-hex", "no usable .hex in the clone (exit 2)", SOAK.replace("<bz>", "<bzbad>") + "9", {},
     {"exit": 2, "err": f"ERROR: no usable .hex file under {os.path.join('<bzbad>', 'bin')} for the hexfile rounds\n",
      "cmds": [], "opened": [], "logs": []}),
    ("soak-max-erases", "every flash takes 3 attempts: never past --max-erases 9", SOAK + "9", {"attempts": 3},
     {"exit": 1, "rounds": "--"}),
    ("soak-max-erases-7", "--max-erases 7: only the final round", SOAK + "7", {"attempts": 3},
     {"exit": 1, "rounds": "-"}),
    ("soak-max-erases-3", "--max-erases 3 is enough for the final round", SOAK + "3", {},
     {"rounds": "+", "summary": ["rounds: 1, failed: 0, chip erases: 1"]}),
    ("soak-max-erases-too-small", "--max-erases 2 (exit 2)", SOAK + "2", {},
     {"exit": 2, "err": "ERROR: --max-erases must be at least 3: the final round alone may erase that often\n",
      "cmds": [], "opened": [], "logs": []}),
    ("soak-link-lost", "the USB link lost during an operation: its erases count", SOAK + "13",
     {"drop": ("test", 1), "attempts": 3},
     {"exit": 1, "rounds": "---", "out": ["ANOMALIES: ['EXCEPTION: the USB link was lost']"]}),
    ("soak-link-lost-upload", "the USB link lost during an upload: the Pico waits for it, the port opened after that",
     SOAK + "13", {"drop": ("load", 1)},
     {"exit": 1, "rounds": "+-+++++", "out": ["ANOMALIES: ['EXCEPTION: the USB link was lost']"]}),
    ("soak-speed", "soak refuses --speed (exit 2)", "--speed fast " + SOAK + "9", {},
     {"exit": 2, "err": "ERROR: soak runs every speed in turn: --speed does not apply\n", "cmds": [], "logs": []}),
    ("soak-restoring-flash", "a reads round that flashes first keeps that flash's retry", SOAK + "13",  # P#15
     {"flip": {3: 0x100}, "attempts": {4: 2}}, {"exit": 1, "rounds": "++--+", "check": reads_kept_retry}),
    # ------------------------------------------------------------------ test-bootloader (G7 P#18-P#20)
    # A coherent bench: the Bus Pirate answers when the bootloader starts a firmware, buzzloader writes the pages
    # it would write, so every check passes, and each fault fails exactly the checks it targets.
    ("test-bootloader", "bootloader, jumper, buzzloader, release: every check passes", BOOT, {},
     {"fails": [], "summary": ["cycles: 2, failed: 0, checks failed: 0"], "chip": "image",
      "buzzloader": ["--hello", "--hello", "--hex=fw-small.hex", "--hello", "--hex=fw-old.hex"]}),
    ("test-bootloader-buzzloader-hangs", "buzzloader that never ends: failed checks, the summary still written",
     BOOT, {"buzzloader_hangs": True},  # G6 P#10
     {"exit": 1, "fails": [BOOT_PAGES, "page 42 (configuration words in the file) refused by the bootloader: ERROR "
                           "[50]", BOOT_RUNS, "read-back: exactly the bootloader + fw-small.hex (", BOOT_PAGES,
                           "buzzloader: 'Firmware updated successfully'", BOOT_RUNS,
                           "read-back: exactly the bootloader + fw-old.hex ("],
      "summary": ["cycles: 2, failed: 2, checks failed: 8"]}),
    ("test-bootloader-first-id-fails", "the first id fails: stop before writing (exit 1)", BOOT,
     {"ids": [NO_RESPONSE, (0x3046, 0)]}, {"exit": 1, "err": ID_FAILED, "cmds": ["status", "id"]}),
    ("test-bootloader-devrev-changes", "a DEVREV that changes fails the id check of each cycle", BOOT,
     {"ids": [(0x3042, 0), (0x3046, 0)]}, {"exit": 1, "fails": [BOOT_SAME, BOOT_SAME], "out": [
         "  ok    id before the test\n"], "summary": ["cycles: 2, failed: 2, checks failed: 2"]}),
    ("test-bootloader-calibration-changes", "so do calibration words that change", BOOT,
     {"ids": [(0x3046, 0), (0x3046, 1)]}, {"exit": 1, "fails": [BOOT_SAME, BOOT_SAME], "out": [
         "  ok    id before the test\n"], "summary": ["cycles: 2, failed: 2, checks failed: 2"]}),
    ("test-bootloader-hello-other", "the hellos need 'Device ID: PIC24FJ64GA002 [d4]'", BOOT,
     {"hello": "Device ID: unknown [d4]"}, {"exit": 1, "fails": ["with a firmware too, 'jumper' gives the bootloader",
                                                                 BOOT_HELLO, BOOT_HELLO],
                                            "summary": ["cycles: 2, failed: 2, checks failed: 3"]}),
    ("test-bootloader-speed", "test-bootloader takes --speed and sets it back", "--speed slow " + BOOT, {},
     {"fails": [], "speed": "slow", "summary": ["cycles: 2, failed: 0, checks failed: 0"]}),
]


def cli_inputs(tmp, embedded, bootloader, facts):
    """The files of the scenarios, written in tmp, and the facts of the built-in image and the firmware (facts:
    c_facts()) they check: <name> in a scenario is p[name]. p["skip"]: the scenarios that cannot run here, and
    why."""
    p = {name: os.path.join(tmp, path) for name, path in (
        ("small", "small_cfg.hex"), ("nocfg", "small_nocfg.hex"), ("risky", "risky_gcp0.hex"),
        ("odd", "my fw (\u00f1) v1+beta.hex"),
        ("long", "a-very-long-firmware-file-name-that-goes-past-forty-seven-chars.hex"),
        ("missing", "does_not_exist.hex"), ("broken", "broken.hex"), ("read", "read_out.hex"), ("sniff", "sniff_out"),
        ("here", "here"), ("empty", "empty"), ("batch", "batch"), ("bz", "buzzpirat"), ("bzbad", "buzzpirat-bad"),
        ("nohex", "buzzpirat-nohex"), ("nobz", "no-buzzpirat"), ("logs", "logs"), ("root", "root"))}
    p["hex"], p["python"] = os.path.join(ROOT, "stuff", embedded.name), sys.executable
    start = {0: 0x040200, 1: 0x000000, 0x100: 0x37FFFF, 0x101: 0x000000}  # GOTO 0x000200, there BRA $
    config = {CW2_INDEX: BP3_CW2, CW1_INDEX: BP3_CW1}
    for name, words in (("small", {**start, **config}), ("nocfg", start),
                        ("risky", {**start, **config, CW1_INDEX: 0x1F7F}), ("odd", start), ("long", start)):
        write_hex(p[name], words)
    write_lines(p["broken"], [":0200000400"])  # shorter than its length byte says
    for folder in (p["here"], p["empty"], p["batch"], os.path.join(p["nohex"], "bin")):
        os.makedirs(folder)
    write_lines(os.path.join(p["here"], "sniff_here.py"),
                ["print('found by its relative path')", f"open({os.path.join(p['sniff'], 'done')!r}, 'w').close()"])
    # a program that which() finds but that cannot start: a .bat on Windows, a text file without #! elsewhere
    batch = os.path.join(p["batch"], "batch_only" + (".bat" if os.name == "nt" else ""))
    write_lines(batch, ["@echo this never runs"])
    os.chmod(batch, 0o755)
    p["skip"] = {}  # which() finds no file that the system will not run: a temporary folder mounted noexec (Linux)
    if not os.access(batch, os.X_OK):
        p["skip"]["sniff-analog-program-cannot-start"] = f"{tmp} is mounted noexec: no file there can run"
    for clone, usable in (("bz", True), ("bzbad", False)):  # buzzpirat's bin/: firmware-only files, and others
        files = {"risky.hex": p["risky"], "broken.hex": p["broken"], "bootloader-upgrade-v4.hex": p["small"]}
        if usable:
            files.update({os.path.join("stablefirm", "fw-small.hex"): p["small"],
                          os.path.join("oldfirm", "fw-old.hex"): p["nocfg"], "full-pickit.hex": p["hex"]})
        for name, source in files.items():
            os.makedirs(os.path.dirname(os.path.join(p[clone], "bin", name)), exist_ok=True)
            shutil.copyfile(source, os.path.join(p[clone], "bin", name))
    # what 'verify FILE' shows with the built-in image on the PIC: the first 10 words that differ, and the count
    chip = Bench.stored(embedded.words)
    for name in ("small", "risky"):
        words = load_image(p[name], "file")[0].words
        lines = [f"  PC 0x{2 * i:06X}: {os.path.basename(p[name])} has {words[i]:06X}, the PIC has {chip[i]:06X}\n"
                 for i in range(FLASH_WORDS) if (words[i] ^ chip[i]) & word_mask(i)]
        p.update({f"{name}_shown": "".join(lines[:10]), f"{name}_next": lines[10], f"{name}_differ": len(lines)})
    word = bootloader.words[0xA900 // 2]  # the one bootloader-readback-mismatch flips
    p.update(image=embedded.name, upload=EmulatedPico.kept_name(embedded.name, facts),
             image_crc=f"{embedded.crc32:08X}",
             image_crc1=f"{embedded.crc32 ^ 1:08X}", image_rows=embedded.used_rows,
             image_cw=f"CW2=0x{embedded.words[CW2_INDEX] & 0xFFFF:04X} CW1=0x{embedded.words[CW1_INDEX] & 0xFFFF:04X}",
             bl=bootloader.name, bl_crc=f"{bootloader.crc32:08X}", bl_words=bootloader.used_words,
             bl_erased=FLASH_WORDS - bootloader.used_words, bl_word=f"{word:06X}", bl_flipped=f"{word ^ 0x100:06X}")
    return p


def fill(value, p):
    """value with each <name> replaced by p[name] (cli_inputs()), also in the lists, tuples and dicts it holds; a
    string that is only <name> becomes p[name] itself."""
    if isinstance(value, dict):
        return {key: fill(v, p) for key, v in value.items()}
    if isinstance(value, (list, tuple)):
        return type(value)(fill(v, p) for v in value)
    if not isinstance(value, str):
        return value
    if re.fullmatch(r"<\w+>", value) and value[1:-1] in p:
        return p[value[1:-1]]
    return re.sub(r"<(\w+)>", lambda m: str(p.get(m.group(1), m.group(0))), value)


def cli_argv(argv, p):
    """A scenario's command line, its arguments split before <name> becomes p[name]: <py:CODE> is this Python
    running SNIFF_CODE[CODE] and <code:CODE> that code, each followed by writing <sniff>/done."""
    done = f"\nopen({os.path.join(p['sniff'], 'done')!r}, 'w').close()"
    out = []
    for arg in argv.split() if isinstance(argv, str) else argv:
        m = re.fullmatch(r"<(py|code):([\w-]+)>", arg)
        if not m:
            out.append(fill(arg, p))
        elif m.group(1) == "py":
            out += [sys.executable, "-c", SNIFF_CODE[m.group(2)] + done]
        else:
            out.append(SNIFF_CODE[m.group(2)] + done)
    return out


def cli_problems(want, r, bench, p):
    """What differs from want (see CLI_SCENARIOS) in the result r of a scenario and on its bench."""
    want = fill(want, p)
    problems = []
    status = want.get("exit", 0)
    if r.returncode != ("SystemExit: pyserial is missing: pip install pyserial" if status == "pyserial" else status):
        problems.append(f"exit {r.returncode!r}")
    lines = r.stderr.splitlines()
    if "err1" in want:
        if not (len(lines) == 1 and lines[0].startswith(want["err1"].replace("<file>", r.args[-1]))):
            problems.append(f"stderr {r.stderr!r}")
    elif "err_end" in want:
        if not r.stderr.endswith(want["err_end"]):
            problems.append(f"stderr {r.stderr[-200:]!r}")
    elif r.stderr != want.get("err", ""):
        problems.append(f"stderr {r.stderr!r}")
    if r.stdout != want.get("stdout", r.stdout) or not r.stdout.startswith(want.get("start", "")):
        problems.append(f"stdout {r.stdout[:200]!r}")
    problems += [f"stdout lacks {t!r}" for t in want.get("out", []) if t not in r.stdout]
    problems += [f"stdout has {t!r}" for t in want.get("not_out", []) if t in r.stdout]
    if "shown" in want and r.stdout != bench.shown.get(want["shown"]):
        problems.append(f"stdout is not what the console printed: {r.stdout[-200:]!r}")
    if "speed" in want and bench.commands[:1] + bench.commands[-1:] != [f"speed {want['speed']}", "speed normal"]:
        problems.append(f"commands {bench.commands[:2]} ... {bench.commands[-1:]}")
    if "chip" in want:  # 'image', 'bootloader', 'erased', a file, or (a file, how load_image() takes it)
        spec = want["chip"]
        if spec in ("image", "bootloader", "erased"):
            words = {"image": bench.embedded.words, "bootloader": bench.bootloader.words}.get(spec, Bench.ERASED)
        else:
            words = load_image(*(spec if isinstance(spec, tuple) else (spec, "file")))[0].words
        if bench.chip != Bench.stored(words):
            problems.append(f"the PIC holds crc32 {zlib.crc32(pack_words(bench.chip)):08X}")
    sent = collections.Counter(data[:2] for data in bench.written)
    left = sorted(os.listdir(p["sniff"])) if os.path.isdir(p["sniff"]) else None  # in --out (None: no folder)
    got = {"cmds": bench.commands, "opened": bench.opened, "pic": bench.pic, "rows": sent[b"R "],
           "aborts": sent[b"X\n"], "loaded": bench.loaded and bench.loaded.name, "logic2": bench.logic2,
           "started": len(RecordedPopen.started), "files": left, "buzzloader": bench.buzzloader,
           "logs": sorted(os.path.basename(f) for f in glob.glob(os.path.join(p["logs"], "*", "*")))}
    problems += [f"{key}: {value}" for key, value in got.items() if key in want and value != want[key]]
    if "run_json" in want:
        recorded = {}
        if left and "run.json" in left:
            with open(os.path.join(p["sniff"], "run.json"), encoding="utf-8") as f:
                recorded = json.load(f)
        problems += [f"run.json: {key} {recorded.get(key)!r}" for key, value in want["run_json"].items()
                     if recorded.get(key) != value]
    if "fails" in want:  # each FAIL line starts with one of them, and each of them starts one FAIL line
        rest, extra = list(want["fails"]), []
        for line in (ln[8:] for ln in r.stdout.splitlines() if ln.startswith("  FAIL  ")):
            match = next((w for w in rest if line.startswith(w)), None)
            rest.remove(match) if match else extra.append(line)
        if rest or extra:
            problems.append(f"checks that should fail and did not: {rest}; failed: {extra}")
    if "count" in want:
        n = sum(1 for ln in r.stdout.splitlines() if ln.startswith(("  ok    ", "  FAIL  ")))
        if n != want["count"]:
            problems.append(f"{n} checks")
    if "summary" in want or "rounds" in want:
        logs = glob.glob(os.path.join(p["logs"], "*", "summary.txt"))
        summary = ""
        if logs:
            with open(logs[0]) as f:
                summary = f.read()
        problems += [f"summary.txt lacks {t!r}" for t in want.get("summary", []) if t not in summary]
        if not summary or summary not in r.stdout.replace("\r", ""):
            problems.append("the summary printed is not summary.txt")
    if "rounds" in want:  # soak: the verdict of each round; never past --max-erases, every erase counted (P#17)
        verdicts = "".join("+" if rec["ok"] and rec["id_ok"] and not rec["anomalies"] else "-"
                           for rec in soak_records(p))
        limit = int(r.args[r.args.index("--max-erases") + 1])
        if verdicts != want["rounds"]:
            problems.append(f"rounds {verdicts}")
        if not (bench.erases <= limit and f"chip erases: {bench.erases}\n" in r.stdout):
            problems.append(f"the Pico erased {bench.erases} times (--max-erases {limit})")
    if bench.running:  # sniff and sniff-analog wait for their command, whatever Logic 2 does (P#21)
        problems.append(f"{bench.running} commands still running when it returned")
    if bench.stuck:
        problems.append(f"still running after 30 s: {bench.stuck}")
    if "check" in want:
        problems += want["check"](r, bench, p)
    return problems


def cmd_test_cli(args):
    """Every command that talks to the hardware but install, and sniff, sniff-analog and build, run in this process
    against emulated hardware (CLI_SCENARIOS, the section above): exit status, output, and what the Pico, the PIC,
    Logic 2 and the files got. Then the emulated console against bp3unbricker.c: it printed only the firmware's own
    formats, and soak counts the firmware's flash attempts. While a scenario runs, this module's globals,
    sys.modules, the folder and the environment are the scenario's: test-cli is a command, not a function to call
    from a program with threads of its own."""
    unknown = [name for name in args.names if not any(s[0].startswith(name) for s in CLI_SCENARIOS)]
    if unknown:
        print(f"ERROR: no scenario starts with {unknown[0]!r}", file=sys.stderr)
        return 2
    check = Checks()
    embedded, bootloader = embedded_from_source()
    code = firmware_code(SOURCE)  # HexError (exit 1) when its tests cannot be told from it
    facts = c_facts(code, SOURCE)
    formats = set()
    tmp = tempfile.mkdtemp(prefix="bp3cli-")  # not TemporaryDirectory(ignore_cleanup_errors=): Python 3.10+
    try:
        p = cli_inputs(tmp, embedded, bootloader, facts)
        for name, what, argv, knobs, want in CLI_SCENARIOS:
            if args.names and not name.startswith(tuple(args.names)):
                continue
            if name in p["skip"]:
                print(f"  skip  {name}: {p['skip'][name]}")
                continue
            shutil.rmtree(p["sniff"], ignore_errors=True)  # what the scenario before left
            shutil.rmtree(p["logs"], ignore_errors=True)
            if os.path.exists(p["read"]):
                os.remove(p["read"])
            knobs = fill(knobs, p)
            bench = Bench(knobs, embedded, bootloader, facts)
            try:
                r = run_emulated(bench, cli_argv(argv, p), tmp, knobs)
                problems = cli_problems(want, r, bench, p)
            except Exception as e:  # a FAIL line, not the end of test-cli
                problems = [f"the scenario raised {type(e).__name__}: {e}"]
            check(not problems, f"{name}: {what}")
            for problem in problems:
                print(f"        {problem}"[:300])
            formats |= bench.formats
    finally:
        shutil.rmtree(tmp, ignore_errors=True)  # Windows keeps a folder that a stuck command still uses
    strings = c_strings(code)
    missing = sorted(fmt for fmt in formats if not any(s.startswith(fmt) for s in strings))
    check(not missing, f"the emulated console printed {len(formats)} formats, each one a literal of sections 1-10 of "
                       "bp3unbricker.c")
    for fmt in missing:
        print(f"        not in sections 1-10 of {SOURCE}: {fmt!r}")
    check(facts["FLASH_ATTEMPTS"] == FLASH_ATTEMPTS, f"soak counts {FLASH_ATTEMPTS} erases a flash at most: "
          f"FLASH_ATTEMPTS of bp3unbricker.c is {facts['FLASH_ATTEMPTS']}")
    return check.summary("CLI TESTS")


# ======================================================================= command line

# The project's banner, the same as the firmware's (print_banner() in bp3unbricker.c) but for its second line
BANNER = "\n".join(["-" * 78,
                    f" bp3unbricker.py {VERSION}",
                    " Bus Pirate v3 unbricker: programs its PIC24FJ64GA002 over ICSP",
                    " https://github.com/therealdreg/bp3unbricker",
                    " MIT License - Copyright (c) 2026 David Reguera Garcia aka Dreg",
                    " X @therealdreg - https://www.rootkit.es - dreg@rootkit.es",
                    "-" * 78])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
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
    p = sub.add_parser("bootloader", help="only the bootloader of the built-in image, then a full read-back")
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
    p.add_argument("uf2", nargs="?", default=DIST, help="a .uf2 file, or a folder with one per chip (default: dist/)")
    p.set_defaults(func=cmd_install)
    p = sub.add_parser("bp", help="talk to the Bus Pirate's own serial port")
    p.add_argument("dev", help="the Bus Pirate's serial port")
    p.add_argument("what", choices=("info", "hello", "boot", "raw"))
    p.add_argument("text", nargs="?", help="for raw: what to send")
    p.set_defaults(func=cmd_bp)

    sub.add_parser("build", help="build both UF2 files into dist/ (Pico SDK)").set_defaults(func=cmd_build)
    p = sub.add_parser("hex2c", help="make FILE.hex the image embedded in bp3unbricker.c")
    p.add_argument("file")
    p.add_argument("--source", default=SOURCE)
    p.set_defaults(func=cmd_hex2c)
    p = sub.add_parser("make-test-image", help="full-flash test image")
    p.add_argument("out")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--pattern", default="random", choices=("random", "zeros", "ones"))
    p.set_defaults(func=cmd_make_test_image)
    lines_help = "the channel of each line, as CHANNEL=NAME,... (VDD may be left out)"
    sniff_map_help = f"{lines_help}; run.json records it (default: {SALEAE_CHANNEL_MAP})"
    map_help = f"{lines_help} (default: the capture's, from its run.json; else {SALEAE_CHANNEL_MAP})"
    saleae_port_help = f"the Logic 2 automation server's TCP port, 1 to 65535 (default: {SALEAE_PORT})"
    p = sub.add_parser("decode", help="decode a Saleae digital CSV capture of the ICSP lines")
    p.add_argument("csv")
    p.add_argument("--map", help=map_help)
    p.add_argument("--list", type=int, default=0, help="print the first N commands of each session")
    p.add_argument("--tail", type=int, default=0, help="print the last N commands of each session")
    p.add_argument("--timing", action="store_true", help="PGC/PGD timing statistics")
    p.add_argument("--dump-reads", help="reconstructed reads (PC address -> bytes) as JSON")
    p.add_argument("--dump-writes", help="reconstructed flash writes as JSON")
    p.set_defaults(func=cmd_decode)
    p = sub.add_parser("sniff", help="capture the ICSP lines with a Saleae Logic 2 while a command runs")
    p.add_argument("--out", required=True)
    p.add_argument("--rate", type=int, default=100_000_000, help="digital sample rate")
    p.add_argument("--analog-rate", type=int, default=0, help="analog sample rate (0: digital only)")
    p.add_argument("--threshold", type=float, default=SALEAE_THRESHOLD_V, help="digital threshold setting in volts")
    p.add_argument("--pre", type=float, default=1.0, help="seconds before the command")
    p.add_argument("--post", type=float, default=1.0, help="seconds after the command")
    p.add_argument("--max-seconds", type=float, default=0,
                   help="stop the capture after this long even if the command still runs")
    p.add_argument("--buffer-mb", type=int, default=SALEAE_BUFFER_MB)
    p.add_argument("--map", default=SALEAE_CHANNEL_MAP, help=sniff_map_help)
    p.add_argument("--saleae-port", type=tcp_port, default=SALEAE_PORT, help=saleae_port_help)
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    p.set_defaults(func=cmd_sniff)
    p = sub.add_parser("sniff-analog", help="triggered analog + digital capture (Saleae Logic 2)")
    p.add_argument("--out", required=True)
    p.add_argument("--map", default=SALEAE_CHANNEL_MAP, help=sniff_map_help)
    p.add_argument("--analog", type=int, nargs="+", help="analog channels (default: MCLR's, PGD's and PGC's)")
    p.add_argument("--analog-rate", type=int, default=50_000_000, help="analog sample rate (default: 50 MS/s)")
    p.add_argument("--rate", type=int, default=125_000_000,
                   help="digital sample rate (default: 125 MS/s; a Logic Pro 8 takes 125 or 250 MS/s with analog "
                        "at 50 MS/s)")
    p.add_argument("--after", type=float, default=0.8, help="seconds kept after the MCLR trigger")
    p.add_argument("--saleae-port", type=tcp_port, default=SALEAE_PORT, help=saleae_port_help)
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    p.set_defaults(func=cmd_sniff_analog)
    p = sub.add_parser("pgd-levels", help="PGD at every latched bit of a sniff-analog capture")
    p.add_argument("dir")
    p.add_argument("--map", help=map_help)
    p.set_defaults(func=cmd_pgd_levels)

    p = sub.add_parser("test-sim", help="the firmware against a PIC24 simulator (host gcc)")
    p.add_argument("--cc", help="host C compiler (default: $CC, MSYS2 UCRT64 gcc on Windows, gcc)")
    p.add_argument("--keep", help="build and run in this folder")
    p.set_defaults(func=cmd_test_sim)
    sub.add_parser("test-hex", help="the checks of this program that need no hardware").set_defaults(func=cmd_test_hex)
    p = sub.add_parser("test-cli", help="every command against emulated hardware (no hardware)")
    p.add_argument("names", nargs="*", metavar="NAME", help="only the scenarios whose names start so")
    p.set_defaults(func=cmd_test_cli)
    p = sub.add_parser("test-loader", help="upload protocol fault injection against a real Pico")
    p.add_argument("file", nargs="?", help="what the PIC holds now (default: the built-in image)")
    p.set_defaults(func=cmd_test_loader)
    for name, text, func in (("test-bootloader", "'bootloader' + buzzloader with every firmware .hex",
                              cmd_test_bootloader),
                             ("soak", "long hardware run of everything", cmd_soak)):
        p = sub.add_parser(name, help=text)
        p.add_argument("--bp-port", required=True, help="the Bus Pirate's own serial port")
        p.add_argument("--buzzpirat", help="a clone of github.com/therealdreg/buzzpirat (or $BUZZPIRAT)")
        p.add_argument("--out", help="log folder")
        if name == "test-bootloader":
            p.add_argument("--rounds", type=int, default=2)
            p.add_argument("--buzzloader", help="the buzzloader program (default: buzzpirat's bin/buzzloader)")
        else:
            p.add_argument("--minutes", type=float, default=90)
            p.add_argument("--max-erases", type=int, default=250,
                           help="at most this many chip erases (10000 rated)")
        p.set_defaults(func=func)

    args = ap.parse_args(argv)
    if argv is None and sys.stderr and sys.stderr.isatty():  # a person at a terminal (pythonw has no stderr)
        print(BANNER, file=sys.stderr)
    # install, bp: pyserial is checked before anything else. soak checks it after its own checks, and the other
    # commands that open the Pico in with_pico(): a bad FILE or usage is exit 2 even without pyserial.
    if args.func in (cmd_install, cmd_bp):
        need_serial()
    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 1
    except (OSError, HexError, PicoError, subprocess.SubprocessError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
