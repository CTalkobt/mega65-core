#!/bin/bash
#
# test_hardware.sh — Interactive hardware test suite for etherdbg
#
# Runs each test, captures -vv output, prompts the user to confirm
# pass/fail. Results and logs are saved to a timestamped directory.
#
# Usage: ./test_hardware.sh [etherdbg_path]
#
# Prerequisites:
#   - MEGA65 connected via Ethernet
#   - ETHLOAD active (Shift+Pound pressed, LED blinking)
#

set -euo pipefail

ETHERDBG="${1:-./etherdbg}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="test_results_${TIMESTAMP}"
mkdir -p "$RESULTS_DIR"

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
TOTAL_COUNT=0

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log_file="$RESULTS_DIR/summary.log"

print_summary() {
    echo ""
    echo -e "${BOLD}${CYAN}========================================${NC}"
    echo -e "${BOLD}${CYAN}  Test Results Summary${NC}"
    echo -e "${BOLD}${CYAN}========================================${NC}"
    echo ""
    echo -e "  Total:   ${TOTAL_COUNT}"
    echo -e "  ${GREEN}Passed:  ${PASS_COUNT}${NC}"
    echo -e "  ${RED}Failed:  ${FAIL_COUNT}${NC}"
    echo -e "  ${YELLOW}Skipped: ${SKIP_COUNT}${NC}"
    echo ""
    echo -e "  Results saved to: ${BOLD}${RESULTS_DIR}/${NC}"
    echo -e "  Summary: ${BOLD}${RESULTS_DIR}/summary.log${NC}"
    echo ""

    echo "" >> "$log_file"
    echo "========================================" >> "$log_file"
    echo "Total: $TOTAL_COUNT  Passed: $PASS_COUNT  Failed: $FAIL_COUNT  Skipped: $SKIP_COUNT" >> "$log_file"
    echo "========================================" >> "$log_file"

    cat "$log_file"
}

banner() {
    echo ""
    echo -e "${BOLD}${CYAN}========================================${NC}"
    echo -e "${BOLD}${CYAN}  etherdbg Hardware Test Suite${NC}"
    echo -e "${BOLD}${CYAN}  $(date)${NC}"
    echo -e "${BOLD}${CYAN}========================================${NC}"
    echo ""
}

run_test() {
    local test_name="$1"
    local description="$2"
    local command="$3"
    local expect="$4"

    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    local test_num=$(printf "%02d" $TOTAL_COUNT)
    local log="$RESULTS_DIR/${test_num}_${test_name}.log"

    echo ""
    echo -e "${BOLD}--- Test ${test_num}: ${test_name} ---${NC}"
    echo -e "  ${description}"
    echo -e "  Command: ${CYAN}${command}${NC}"
    echo -e "  Expected: ${expect}"
    echo ""

    # Prompt to continue
    echo -e "${YELLOW}Press Enter to run this test, or 's' to skip:${NC}"
    read -r skip_input
    if [[ "$skip_input" == "s" || "$skip_input" == "S" ]]; then
        echo -e "${YELLOW}  SKIPPED${NC}"
        SKIP_COUNT=$((SKIP_COUNT + 1))
        echo "[$test_num] $test_name: SKIPPED" >> "$log_file"
        return
    fi

    # Run the command, capture output
    echo -e "  Running..."
    echo "Command: $command" > "$log"
    echo "Timestamp: $(date -Iseconds)" >> "$log"
    echo "---" >> "$log"

    # Run with timeout
    set +e
    eval "$command" >> "$log" 2>&1
    local exit_code=$?
    set -e

    echo "---" >> "$log"
    echo "Exit code: $exit_code" >> "$log"

    # Show last few lines of output
    echo -e "  Exit code: $exit_code"
    echo -e "  Output (last 10 lines):"
    tail -10 "$log" | sed 's/^/    /'
    echo ""

    # Prompt for result
    echo -e "${YELLOW}Did this test pass? ${NC}"
    echo -e "  ${GREEN}[Enter]${NC} = PASS"
    echo -e "  ${RED}Type reason${NC} = FAIL (describe what went wrong)"
    echo -e "  ${YELLOW}q${NC} = Quit testing"
    read -r user_input

    if [[ "$user_input" == "q" || "$user_input" == "Q" ]]; then
        echo -e "  ${YELLOW}QUIT — stopping tests${NC}"
        echo "[$test_num] $test_name: QUIT" >> "$log_file"
        echo "Result: QUIT" >> "$log"
        print_summary
        exit $FAIL_COUNT
    elif [[ -z "$user_input" ]]; then
        echo -e "  ${GREEN}PASS${NC}"
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "[$test_num] $test_name: PASS" >> "$log_file"
        echo "Result: PASS" >> "$log"
    else
        echo -e "  ${RED}FAIL: ${user_input}${NC}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[$test_num] $test_name: FAIL - $user_input" >> "$log_file"
        echo "Result: FAIL - $user_input" >> "$log"
    fi
}

pause_for_setup() {
    local msg="$1"
    echo ""
    echo -e "${YELLOW}${msg}${NC}"
    echo -e "${YELLOW}Press Enter when ready...${NC}"
    read -r
}

# =========================================================================
# Test Suite
# =========================================================================

banner

echo "Results will be saved to: $RESULTS_DIR/"
echo ""

# Check etherdbg exists
if [[ ! -x "$ETHERDBG" ]]; then
    echo -e "${RED}Error: $ETHERDBG not found or not executable${NC}"
    echo "Build first: make"
    exit 1
fi

echo "Using: $ETHERDBG"
echo ""

pause_for_setup "Ensure MEGA65 is connected via Ethernet and ETHLOAD is active (Shift+Pound)."

# --- Discovery ---
run_test "discover" \
    "Auto-detect MEGA65 on local network" \
    "$ETHERDBG -D" \
    "Should print IPv6 address like fe80::...%interface"

# --- Reset to MEGA65 mode ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "reset65" \
    "Reset MEGA65 to MEGA65 BASIC 65 mode" \
    "$ETHERDBG -vv -5 2>&1" \
    "MEGA65 should show BASIC 65 READY prompt"

# --- Reset to C64 mode ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "reset64" \
    "Reset MEGA65 to C64 BASIC 2 mode" \
    "$ETHERDBG -vv -4 2>&1" \
    "MEGA65 should show C64 READY. prompt"

# --- Reset back to M65 from C64 ---
run_test "reset65_from_c64" \
    "Reset from C64 mode back to MEGA65 mode (without Shift+Pound)" \
    "$ETHERDBG -vv -5 2>&1" \
    "MEGA65 should reset from C64 to MEGA65 BASIC 65 READY prompt"

# --- PAL mode reset ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "reset65_pal" \
    "Reset to MEGA65 mode with PAL video" \
    "$ETHERDBG -vv --pal -5 2>&1" \
    "MEGA65 should reset in PAL mode (check video output)"

# --- NTSC mode reset ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "reset65_ntsc" \
    "Reset to MEGA65 mode with NTSC video" \
    "$ETHERDBG -vv --ntsc -5 2>&1" \
    "MEGA65 should reset in NTSC mode (check video output)"

# --- Ping ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "ping" \
    "Send INC \$D020 to flash border colour" \
    "$ETHERDBG -vv --ping 2>&1" \
    "MEGA65 border should flash/change colour (screen corruption from hyperrupt is known)"

# --- Echo ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "echo" \
    "Send echo ethlet and receive response" \
    "$ETHERDBG -vv --echo 2>&1" \
    "Should print 'Echo response: 1024 bytes' (screen corruption is expected)"

# --- Jump ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound). This will jump to \$E000 (BASIC warm start)."

run_test "jump" \
    "Jump to address \$E000 (BASIC warm start)" \
    "$ETHERDBG -vv -j E000 2>&1" \
    "MEGA65 should show BASIC prompt (jumped to warm start vector)"

# --- Halt ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "halt" \
    "Halt mode — ETHLOAD stays running" \
    "$ETHERDBG -vv --halt 2>&1" \
    "etherdbg should exit cleanly. MEGA65 stays in ETHLOAD (LED blinking)"

# --- Load PRG (if available) ---
echo ""
echo -e "${YELLOW}Do you have a test PRG file to load? Enter path or press Enter to skip:${NC}"
read -r prg_file

if [[ -n "$prg_file" && -f "$prg_file" ]]; then
    pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

    run_test "load_prg" \
        "Load PRG file and auto-detect C64/M65 mode" \
        "$ETHERDBG -vv -r '$prg_file' 2>&1" \
        "Program should load and RUN on MEGA65"

    pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

    run_test "load_prg_c64" \
        "Load PRG with explicit C64 mode" \
        "$ETHERDBG -vv -4 -r '$prg_file' 2>&1" \
        "Program should load in C64 mode and RUN"
fi

# --- Mount D81 ---
echo ""
echo -e "${YELLOW}Do you have a D81 file on the SD card to test mounting? Enter name or press Enter to skip:${NC}"
read -r d81_file

if [[ -n "$d81_file" ]]; then
    pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

    run_test "mount_d81" \
        "Reset to M65 mode with D81 mounted" \
        "$ETHERDBG -vv -5 -m '$d81_file' 2>&1" \
        "MEGA65 should reset with the D81 image mounted (check with LOAD\"\$\",8)"
fi

# --- Memory read (may not work due to NDP/response issues) ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "read_memory" \
    "Read 16 bytes from \$D020 (VIC border colour register area)" \
    "$ETHERDBG -vv --read D020 10 2>&1" \
    "Should show hex dump of 16 bytes. NOTE: may timeout if response protocol isn't working yet"

# --- Memory write ---
pause_for_setup "Ensure MEGA65 has ETHLOAD active (Shift+Pound)."

run_test "write_memory" \
    "Write byte \$01 to \$D020 (set border to white)" \
    "$ETHERDBG -vv --poke D020 01 2>&1" \
    "MEGA65 border should turn white. NOTE: requires working transport"

# =========================================================================
# Summary
# =========================================================================

print_summary
exit $FAIL_COUNT
