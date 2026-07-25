#!/usr/bin/env python3
"""Generate a simple C64 BASIC+ML hello PRG for testing etherload/etherdbg -r"""

import struct

# Load address $0801
load_addr = 0x0801

# BASIC stub: 10 SYS 2062
# This is a standard BASIC line that calls our ML code at $080E
basic = bytearray()
# Next line pointer (will be patched)
basic += struct.pack('<H', 0)  # placeholder
# Line number 10
basic += struct.pack('<H', 10)
# SYS token
basic += bytes([0x9E])
# "2061" in PETSCII
basic += b'2061'
# End of line
basic += bytes([0x00])
# End of program (null pointer)
basic += struct.pack('<H', 0)

# Patch next line pointer: points past this line
next_line = load_addr + len(basic) - 2  # before the final 00 00
basic[0] = (load_addr + len(basic) - 2) & 0xFF
basic[1] = ((load_addr + len(basic) - 2) >> 8) & 0xFF

# ML code starts at $0801 + len(basic) = $080E = 2062
ml_start = load_addr + len(basic)
assert ml_start == 2061, f"ML start is {ml_start}, expected 2061"

# 6502 machine code
code = bytearray()

# Change border to cyan (3)
code += bytes([0xA9, 0x03])       # LDA #3
code += bytes([0x8D, 0x20, 0xD0]) # STA $D020

# Change background to black (0)
code += bytes([0xA9, 0x00])       # LDA #0
code += bytes([0x8D, 0x21, 0xD0]) # STA $D021

# Print message using KERNAL CHROUT ($FFD2)
message = (
    "\r\r"
    "  ****************************\r"
    "  *                          *\r"
    "  *  HELLO FROM ETHERDBG!    *\r"
    "  *                          *\r"
    "  *  IF YOU SEE THIS, THE    *\r"
    "  *  TRANSFER WORKED!        *\r"
    "  *                          *\r"
    "  ****************************\r"
    "\r"
)

# Convert to PETSCII (uppercase)
petscii = bytearray()
for ch in message:
    if ch == '\r':
        petscii.append(0x0D)
    elif ch == '*':
        petscii.append(0x2A)
    elif 'A' <= ch <= 'Z':
        petscii.append(ord(ch))  # uppercase maps directly in PETSCII
    elif 'a' <= ch <= 'z':
        petscii.append(ord(ch) - 0x20)  # lowercase → uppercase PETSCII
    else:
        petscii.append(ord(ch))

# LDX #0; loop: LDA msg,X; BEQ done; JSR $FFD2; INX; BNE loop; done: RTS
msg_offset = len(code) + 9  # offset to message data from start of ML
code += bytes([0xA2, 0x00])       # LDX #0
loop_addr = len(code)
code += bytes([0xBD])             # LDA msg,X (absolute,X)
# Patch address later
msg_addr_pos = len(code)
code += bytes([0x00, 0x00])       # placeholder for msg address
code += bytes([0xF0])             # BEQ done
done_offset_pos = len(code)
code += bytes([0x00])             # placeholder for branch offset
code += bytes([0x20, 0xD2, 0xFF]) # JSR $FFD2 (CHROUT)
code += bytes([0xE8])             # INX
# BNE loop (branch back)
branch_back = loop_addr - (len(code) + 2)
code += bytes([0xD0, branch_back & 0xFF])  # BNE loop
# done:
done_addr = len(code)
code += bytes([0x60])             # RTS

# Patch BEQ done offset
code[done_offset_pos] = done_addr - (done_offset_pos + 1)

# Append message data
msg_actual_addr = ml_start + len(code)
code += petscii
code += bytes([0x00])  # null terminator

# Patch message address
code[msg_addr_pos] = msg_actual_addr & 0xFF
code[msg_addr_pos + 1] = (msg_actual_addr >> 8) & 0xFF

# Build PRG file
prg = struct.pack('<H', load_addr) + basic + code

with open('test_hello.prg', 'wb') as f:
    f.write(prg)

print(f"Generated test_hello.prg")
print(f"  Load address: ${load_addr:04X}")
print(f"  BASIC stub: SYS {ml_start}")
print(f"  ML code: ${ml_start:04X}-${ml_start + len(code) - 1:04X}")
print(f"  Total size: {len(prg)} bytes (including 2-byte header)")
