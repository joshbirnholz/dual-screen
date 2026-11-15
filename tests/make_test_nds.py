#!/usr/bin/env python3
"""Writes a tiny DS program that fills the top screen with red and waits.

Not a game: the smallest thing a real DS core will boot, for checking a build
plays DS games at all where no game can be shipped -- CI's iOS simulator run
starts it with --game and looks for red in a screenshot. It boots directly
(no BIOS intro), which is how melonDS DS starts a game with its built-in BIOS.

The ARM9 program, as assembled by clang --target=armv5te-none-eabi:

    ldr  r0, =0x04000304    @ POWCNT1: screens, 2D engine A, A on top
    ldr  r1, =0x8203
    str  r1, [r0]
    ldr  r0, =0x04000240    @ VRAMCNT_A: bank A to the LCD
    mov  r1, #0x80
    strb r1, [r0]
    mov  r0, #0x04000000    @ DISPCNT A: show VRAM bank A
    mov  r1, #0x00020000
    str  r1, [r0]
    mov  r0, #0x06800000    @ bank A
    ldr  r1, =0x801F801F    @ two red pixels
    mov  r2, #0x6000        @ 256 x 192 / 2 words
 1: str  r1, [r0], #4
    subs r2, r2, #1
    bne  1b
 2: b    2b

The ARM7 only waits.

usage: make_test_nds.py <out.nds>
"""

import struct
import sys

ARM9 = [
    0xE59F0038, 0xE59F1038, 0xE5801000, 0xE59F0034,
    0xE3A01080, 0xE5C01000, 0xE3A00301, 0xE3A01802,
    0xE5801000, 0xE3A0051A, 0xE59F101C, 0xE3A02A06,
    0xE4801004, 0xE2522001, 0x1AFFFFFC, 0xEAFFFFFE,
    0x04000304, 0x00008203, 0x04000240, 0x801F801F,
]
ARM7 = [0xEAFFFFFE]  # b .

ARM9_OFFSET = 0x8000
ARM7_OFFSET = 0x9000
ROM_SIZE = 0x20000


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    arm9 = b"".join(struct.pack("<I", word) for word in ARM9)
    arm7 = b"".join(struct.pack("<I", word) for word in ARM7)
    rom = bytearray(ROM_SIZE)
    rom[0x000:0x00C] = b"DUALSCRNTEST"
    rom[0x00C:0x010] = b"DSTS"
    rom[0x010:0x012] = b"00"
    struct.pack_into("<IIII", rom, 0x20, ARM9_OFFSET, 0x02000000, 0x02000000, len(arm9))
    struct.pack_into("<IIII", rom, 0x30, ARM7_OFFSET, 0x02380000, 0x02380000, len(arm7))
    struct.pack_into("<I", rom, 0x80, ARM7_OFFSET + len(arm7))
    struct.pack_into("<I", rom, 0x84, 0x4000)
    rom[ARM9_OFFSET:ARM9_OFFSET + len(arm9)] = arm9
    rom[ARM7_OFFSET:ARM7_OFFSET + len(arm7)] = arm7
    struct.pack_into("<H", rom, 0x15E, crc16(bytes(rom[:0x15E])))
    with open(sys.argv[1], "wb") as out:
        out.write(rom)
    return 0


if __name__ == "__main__":
    sys.exit(main())
