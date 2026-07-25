# MEGA65 Ethernet Debug Protocol

## Overview

The MEGA65 supports remote program loading and debugging via its built-in
Ethernet controller. The protocol uses IPv6 link-local UDP packets containing
executable 45GS02 machine code ("ethlets") that the MEGA65 receives and
executes directly.

This document describes the protocol as implemented by:
- **ETHLOAD.M65** — the MEGA65-side listener (`mega65-core/src/utilities/etherload.a65`)
- **etherload** — the host-side tool (`mega65-tools/src/tools/etherload/`)
- **etherdbg** — this project's host-side tool

## Protocol Stack

```
┌─────────────────────────────────────────────┐
│  Ethlet (executable 45GS02 code payload)    │
├─────────────────────────────────────────────┤
│  UDP (port 4510)                            │
├─────────────────────────────────────────────┤
│  IPv6 (link-local fe80::/10)                │
├─────────────────────────────────────────────┤
│  Ethernet II (EtherType $86DD)              │
└─────────────────────────────────────────────┘
```

IPv4 is **not supported** — ETHLOAD explicitly checks for EtherType `$86DD`
and rejects all other frame types. This was changed from the original IPv4
protocol in mega65-core commit `88f067d1`.

---

## 1. ETHLOAD Listener

### Activation

ETHLOAD.M65 is activated on the MEGA65 by pressing **Shift+£** (pound key).
DIP switch 2 must be set to ON for Ethernet remote control to be enabled.
When active, the MEGA65's power LED blinks green-yellow.

ETHLOAD can also be activated remotely via the **hyperrupt trigger** (see
Section 5), which causes the hypervisor to load ETHLOAD.M65 from the SD card.

### Source Code

The listener lives at `src/utilities/etherload.a65` in the mega65-core
repository. It assembles to `ETHLOAD.M65` which resides on the SD card.

### Initialization

On activation, ETHLOAD:
1. Enables MEGA65 I/O personality (`$47/$53` → `$D02F`)
2. Switches to 40MHz CPU mode (`LDA #65; STA $00`)
3. Configures Ethernet controller (`$D6E5 = $75`):
   - RXPH 1, MCST on, BCST on, TXPH 1, NOCRC off, NOPROM on
4. Clears pending RX frames
5. Maps Ethernet buffer at `$6000-$7FFF` via MAP instruction:
   - `$DE000-$DFFFF` → `$6000-$7FFF`
6. Builds link-local IPv6 address from MAC address
7. Enters packet polling loop

### Packet Polling Loop

ETHLOAD continuously polls `$D6E1` bit 5 for received frames. For each frame:

1. **EtherType check**: `$680E/$680F` must be `$86DD` (IPv6)
2. **IPv6 version check**: `$6810` top nibble must be `$60`
3. **Source address check**: `$6818/$6819` must be link-local (`$FE80`)
4. **Protocol check**: `$6816` = `$3A` (ICMPv6) → handle ND solicitation
   or `$6816` = `$11` (UDP) → continue to payload check
5. **UDP dest port check**: `$683A/$683B` must be `$119E` (4510 big-endian)
6. **Payload signature check**: `$6840` must be `$A9` (LDA #imm opcode)
7. If all checks pass: reset basepage to `$00`, JSR to `$6840`
8. On RTS: set basepage back to `$68`, return to polling loop

### Neighbor Discovery (ND)

ETHLOAD implements minimal IPv6 Neighbor Discovery:
- Responds to ND Solicitations (ICMPv6 type 135) for its own link-local address
- Sends ND Advertisement replies with its MAC address
- **Known issue**: ND responses may not always work reliably, causing the
  host's neighbor table entry to remain in FAILED state

---

## 2. Memory Map (ETHLOAD Active)

When ETHLOAD is running, the MAP instruction overlays the Ethernet controller
at `$6000-$7FFF`:

```
$6000-$67FF  TX buffer (physical $FFDE000-$FFDE7FF)
             CPU writes go to TX buffer. Read returns TX buffer contents.

$6800-$6FFF  RX buffer (physical $FFDE800-$FFDEFFF)
             CPU reads come from current RX frame buffer.
             First 2 bytes = frame length (little-endian).
             Ethernet frame starts at $6802.

$7000-$7FFF  I/O space (physical $FFDF000-$FFDF7FF)
             NOT usable as RAM. Contains other I/O devices.
```

### RX Buffer Layout (Received IPv6 UDP Packet)

All offsets are absolute addresses (basepage = $00):

```
Offset  Address  Content
------  -------  -------
$00     $6800    Frame length low byte
$01     $6801    Frame length high byte
$02     $6802    Dest MAC byte 0 (us / multicast)
...     ...      ...
$07     $6807    Dest MAC byte 5
$08     $6808    Source MAC byte 0 (sender)
...     ...      ...
$0D     $680D    Source MAC byte 5
$0E     $680E    EtherType high ($86)
$0F     $680F    EtherType low ($DD)
$10     $6810    IPv6 version/class ($60)
$11-$13 $6811    Traffic class / flow label
$14-$15 $6814    IPv6 payload length (big-endian)
$16     $6816    Next header ($11=UDP, $3A=ICMPv6)
$17     $6817    Hop limit
$18-$27 $6818    IPv6 source address (16 bytes)
$28-$37 $6828    IPv6 dest address (16 bytes)
$38-$39 $6838    UDP source port (big-endian)
$3A-$3B $683A    UDP dest port ($11 $9E = 4510)
$3C-$3D $683C    UDP length (big-endian)
$3E-$3F $683E    UDP checksum
$40+    $6840    UDP payload = ETHLET CODE
```

### Key I/O Registers

```
$D02F       I/O personality (write $47 then $53 for MEGA65 mode)
$D6E1       Ethernet status (bit 5 = RX frame available, bit 4 = TX ready)
$D6E2-$D6E3 TX frame size (little-endian)
$D6E4       TX trigger (write $01 to transmit)
$D6E9-$D6EE MAC address registers (6 bytes)
$D700       DMA trigger / list address low byte
$D701       DMA list address high byte
$D702       DMA list bank
$D703       DMA control (bit 0 = F018B format)
$D704       DMA list MB (megabyte)
$D705       DMA source MB
$D706       DMA dest MB
$D707       Inline DMA trigger (DMA list follows in instruction stream)
```

---

## 3. Ethlet Format

An "ethlet" is a self-contained 45GS02 machine code program sent as the
UDP payload. When ETHLOAD receives it, it JSRs to `$6840` (the start of
the payload in the RX buffer).

### Requirements

1. **First byte must be `$A9`** (LDA #imm). ETHLOAD checks this before JSR.
2. **Must end with `$60`** (RTS) to return to ETHLOAD's polling loop.
3. **Should be 1024 bytes** (padded with zeros). All mega65-tools ethlets
   use this size. The host pads all packets to 1024 bytes before sending.
4. **Must enable MEGA65 I/O** (`$47/$53` → `$D02F`) before accessing any
   VIC, DMA, or Ethernet registers.
5. **Code runs at `$6840`** in the RX buffer with basepage = `$00`.
6. **Non-persistent**: the ethlet is overwritten by the next received packet.

### Standard Ethlet Preamble

All mega65-tools ethlets begin with this echo/ACK preamble:

```asm
entry:
    lda #$00            ; Required $A9 prefix
    nop                 ; 6 NOPs (reserved/padding)
    nop
    nop
    nop
    nop
    nop

    ; Enable MEGA65 I/O
    lda #$47
    sta $d02f
    lda #$53
    sta $d02f

    ; Wait for TX ready
*   lda $d6e1
    and #$10
    beq -

    ; Set F018B DMA format
    lda #$01
    tsb $d703

    ; Trigger inline DMA
    sta $d707

    ; DMA list follows: copy RX→TX, swap MACs, copy our MAC
    ; (this echoes the packet back as an ACK)
    ...
```

The echo preamble copies the received packet to the TX buffer (with swapped
MAC/IP/port) and transmits it back. This serves as an ACK mechanism — the
host knows the ethlet was received and executed when it receives the echo.

### Sequence Numbers

Ethlets have a 16-bit sequence number at offset `$03FE-$03FF` (bytes 1022-1023
of the 1024-byte payload). The host increments this for each packet and
matches it in the echo response for ACK tracking.

---

## 4. Standard Ethlets

### Echo Ethlet (`ethlet_echo`)
- Size: 1024 bytes (117 bytes code + padding)
- Purpose: Connectivity test / ACK mechanism
- Behavior: Copies entire RX frame to TX, swaps MACs/IPs/ports, transmits
- Used by `ethl_ping()` to confirm ETHLOAD is running

### DMA Load Ethlet (`ethlet_dma_load`)
- Size: 1260 bytes (code + 192 bytes data area)
- Purpose: Transfer data to MEGA65 memory
- Patchable fields:
  - `dest_address` (16-bit): destination in MEGA65 address space
  - `dest_bank` (4-bit): destination bank
  - `dest_mb` (8-bit): destination megabyte
  - `byte_count` (16-bit): number of bytes to copy
  - `rom_write_enable` (1 byte): enable writing to ROM area
  - `seq_num` (16-bit): sequence number for ACK
  - `data` (up to ~190 bytes): payload data at offset `$00C0`

### Reset C64 Ethlet (`ethlet_all_done_basic2`)
- Size: 1024 bytes
- Purpose: Reset MEGA65 to C64 BASIC 2 mode
- Includes echo preamble (ACK) then:
  - Disables ROM write protection via hypervisor trap
  - Optionally loads ROM file from SD card
  - Optionally mounts D81 disk image
  - Copies autostart routine to cassette buffer ($0340)
  - Restores MAP, VIC state, banking
  - Resets CPU via `JMP ($FFFC)` (C64 reset vector)
  - Autostart patches IRQ to RUN loaded program
- Patchable fields:
  - `data_end_address`: end of loaded program
  - `do_run`: auto-RUN flag
  - `enable_cart_signature`: cartridge detection
  - `enable_default_rom_load`: load ROM from SD
  - `restore_prg`: restore program to BASIC
  - `set_video_mode`: PAL ($01) / NTSC ($FF) / unchanged ($00)
  - `d81filename`: D81 image to mount (64 bytes)

### Reset MEGA65 Ethlet (`ethlet_all_done_basic65`)
- Size: 1024 bytes
- Purpose: Reset to MEGA65 BASIC 65 mode
- Similar to C64 reset but targets MEGA65 ROM and BASIC 65
- Copies autostart routine to ROM space at `$2C700` (via MAP)
- Autostart patches BASIC 65 IRQ vector at `$32007`
- Same patchable fields as C64 reset

### Jump Ethlet (`ethlet_all_done_jump`)
- Size: 1024 bytes
- Purpose: Jump to a specified address
- Includes echo preamble, then unmaps Ethernet buffer and JMPs
- Patchable fields:
  - `jump_addr`: 16-bit target address
  - `d81filename`: optional D81 mount

---

## 5. Hyperrupt Trigger

The hyperrupt is a special packet that triggers a hypervisor trap via the
MEGA65's Ethernet controller hardware (FPGA level, not software).

### Packet Format

- Size: 128 bytes UDP payload
- Content: zeros except for magic string at offset `$24`:
  ```
  $24: 65 47 53 4B 45 59 43 4F 44 45 00 80
       'e' 'G' 'S' 'K' 'E' 'Y' 'C' 'O' 'D' 'E' $00 $80
  ```
- The `$00 $80` is the keyboard scancode `$8000` which triggers the
  Ethernet hypervisor trap
- Sent to **multicast** `ff02::1` (no NDP resolution needed)

### FPGA Processing

The Ethernet controller VHDL (`ethernet.vhdl`) processes the magic string
at the raw frame level (bytes 100-111 of the Ethernet frame). When matched,
it generates the `eth_keycode` signal with value `$8000`, which triggers
`eth_hyperrupt` in `gs4510.vhdl`. The hypervisor then loads `ETHLOAD.M65`
from the SD card.

### Side Effects

- ETHLOAD.M65 is reloaded from the SD card (even if already running)
- The MEGA65 screen is temporarily disrupted during the reload
- After reload, ETHLOAD starts its polling loop and sends beacons
- The beacon/echo responses resolve the host's NDP neighbor table

---

## 6. Host-Side Communication

### Connection Sequence (mega65-tools etherload)

```
1. Discovery
   - Create UDP socket per IPv6 link-local interface
   - Join multicast group ff02::1 on each interface
   - Bind to port 4510
   - Poll for "mega65" beacon (6-byte UDP: "mega65")
   - Extract MEGA65's IPv6 address and scope ID from beacon source
   - Close discovery sockets

2. Transport Setup
   - Create single UDP socket (AF_INET6, SOCK_DGRAM)
   - Set IPV6_MULTICAST_IF to discovered scope_id
   - Set non-blocking (O_NONBLOCK)
   - NO bind() — send from ephemeral port
   - Set up broadcast address (ff02::1) for hyperrupt

3. Hyperrupt
   - Send 128-byte trigger to ff02::1:4510
   - This resolves NDP and ensures ETHLOAD is running

4. Echo/Ping
   - Send ethlet_echo (1024 bytes) to MEGA65's unicast address
   - Retry with 20ms intervals until echo response received
   - Confirms ETHLOAD is running and NDP is resolved

5. Data Transfer (if loading a file)
   - Send dma_load ethlets with file data
   - Each ethlet echoed back as ACK
   - Retry unacked frames with exponential backoff

6. Completion
   - Send reset ethlet (basic2/basic65/jump) with patched fields
   - Wait for ACK
   - Close socket
```

### NDP (Neighbor Discovery Protocol)

The MEGA65's IPv6 implementation is minimal. Key behaviors:

- **ND Solicitation responses are unreliable**: The host's neighbor table
  entry for the MEGA65 often shows `FAILED`
- **Multicast packets don't need NDP**: The hyperrupt trigger (`ff02::1`)
  always reaches the MEGA65
- **NDP resolves via echo**: When ETHLOAD echoes a packet back, the host
  kernel sees a unicast IPv6 packet from the MEGA65 and populates the
  neighbor table entry (transitions to REACHABLE)
- **NDP decays**: REACHABLE → STALE → FAILED within seconds to minutes.
  Each new tool invocation may need to re-resolve.
- **After machine reset**: ETHLOAD stops running, so subsequent packets
  cannot be delivered even if NDP is still resolved.

### Socket Configuration Notes

- **No bind() to port 4510**: Binding causes Linux to silently drop outgoing
  UDP packets (confirmed via tcpdump). mega65-tools does not bind.
- **No IPV6_JOIN_GROUP on transport socket**: Joining `ff02::1` causes the
  OS to send MLD (Multicast Listener Discovery) reports, which ETHLOAD
  misinterprets and corrupts the MEGA65 screen. Discovery sockets may join
  but must be closed before creating the transport socket.
- **Interface routing**: Without `SO_BINDTODEVICE` (requires root), packets
  may route to the wrong interface (e.g., WiFi instead of Ethernet).
  `IPV6_MULTICAST_IF` helps for multicast but unicast routing depends on
  the kernel's route table and the `sin6_scope_id` in the destination address.

---

## 7. Screen Corruption

### Cause

Screen corruption during Ethernet transfers is inherent to the ETHLOAD
protocol. It occurs because:

1. **ETHLOAD's MAP instruction** overlays `$6000-$7FFF` with the Ethernet
   controller. While active, normal memory access to this range is redirected.

2. **The echo preamble** in every ethlet does a DMA copy of the entire RX
   buffer (~1086 bytes) to the TX buffer. This DMA operation through the
   mapped I/O space can affect the display.

3. **The hyperrupt** causes ETHLOAD to reload from the SD card, which
   disrupts the VIC-IV display during the hypervisor trap and reload process.

### Impact

- **Reset commands** (`-4`, `-5`): Corruption is transient. The reset ethlet
  restores all VIC state, reloads ROM, and resets the CPU. The screen
  shows a clean BASIC prompt after completion.

- **File loading** (`-r`): Same as reset — corruption during transfer,
  clean screen after reset and program execution.

- **Debug commands** (`--ping`, `--poke`, `--read`): Corruption persists
  because these commands don't reset the machine. The ETHLOAD MAP remains
  active and the VIC state is not restored.

### Mitigation (Not Yet Solved)

Potential approaches:
1. **DMA save/restore**: Save screen RAM and colour RAM to upper memory
   before corruption, restore after. Requires correct F018B DMA list format.
2. **JTAG transport**: The serial monitor doesn't use ETHLOAD, avoiding
   all screen corruption issues.
3. **Accept corruption**: For commands that reset the machine, corruption
   is transient and acceptable.

---

## 8. Beacon Protocol

ETHLOAD sends a periodic beacon to announce its presence on the network.

### Beacon Packet

- **Source**: MEGA65's link-local IPv6 address
- **Destination**: `ff02::1` (all-nodes multicast) port 4510
- **Payload**: 6 bytes: `6D 65 67 61 36 35` = ASCII `"mega65"`
- **Interval**: approximately 1 second

### Discovery Process

The host listens on port 4510 for the beacon. When received:
1. The MEGA65's IPv6 address is extracted from the packet's source address
2. The interface scope ID is noted for routing
3. The host creates a transport socket targeted at this address

Discovery does NOT cause any screen corruption on the MEGA65.

---

## 9. Differences from Original Protocol

The original etherload (in `mega65-core/src/tools/etherload/etherload.c`)
used IPv4 (`AF_INET`, `SOCK_DGRAM`). This was changed when ETHLOAD.M65
was updated to require IPv6 (mega65-core commit `88f067d1`).

Key differences:
- Old: IPv4 broadcast to `255.255.255.255:4510`
- New: IPv6 link-local unicast to MEGA65's `fe80::...` address
- Old: No echo/ACK mechanism (fire-and-forget with `usleep(150)`)
- New: Echo-based ACK with retransmission queue
- Old: Simple DMA routine embedded as C byte array
- New: Separate ethlet files (`.a65` source, `.c` compiled data)

---

## 10. References

- **ETHLOAD source**: `mega65-core/src/utilities/etherload.a65`
- **Ethernet VHDL**: `mega65-core/src/vhdl/ethernet.vhdl`
- **CPU VHDL**: `mega65-core/src/vhdl/gs4510.vhdl` (hyperrupt handling)
- **mega65-tools etherload**: `mega65-tools/src/tools/etherload/`
- **Xemu Ethernet**: `/home/duck/src/xemu/targets/mega65/ethernet65.c`
- **Monitor**: `mega65-core/src/monitor/monitor.a65` (JTAG serial protocol)
- **MEGA65 Book**: DMA controller, VIC-IV registers, MAP instruction
