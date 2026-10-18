#!/bin/bash
# Read the two things about the unit's running kernel that an add-on kernel module needs, from the unit itself:
#
#   unit.config      the kernel configuration (/proc/config.gz)
#   Module.symvers   the checksum of every symbol the kernel exports
#
# The kernel of the unit is built with symbol versioning, so a module only loads when it was built against the same
# symbol checksums. The unit's kernel source is not public (the maker added code to NXP's tree), so the checksums are
# read out of the running kernel: its tables of exported symbols are in memory, and the script copies that part of the
# memory through /dev/mem and turns it into a Module.symvers file (mksymvers.py).
#
#   scripts/kernelsyms.sh root@ADDRESS-OF-THE-UNIT
#   X45_UNIT=root@ADDRESS scripts/kernelsyms.sh
#
# Needs an SSH login as root on the unit (the package does not switch SSH on; if yours has it off, this cannot run).
# It only reads: nothing on the unit is changed. Output goes to $X45_KERNEL_DATA (default $X45_BUILD/kernel-data).
# Both files come from your own unit and stay on your computer; do not publish them.
#
# Assumption: the unit's RAM starts at physical address 0x10000000 and the kernel is mapped at 0x80000000, which holds
# for the i.MX6 board of the X45. The result is checked at the end (a module-layout entry must be there and the table
# must be large); the real test is scripts/wifidriver.sh, which checks every checksum of the module it builds.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools ssh python3

UNIT="${1:-${X45_UNIT:-}}"
[ -n "$UNIT" ] || die "usage: scripts/kernelsyms.sh root@ADDRESS   (the unit, reachable by SSH)"
KDATA="${X45_KERNEL_DATA:-$X45_BUILD/kernel-data}"
SSHCMD=(ssh -o BatchMode=yes -o ConnectTimeout=10 ${X45_SSH_OPTS:-} "$UNIT")
mkdir -p "$KDATA"

log "kernel configuration"
"${SSHCMD[@]}" 'gunzip -c /proc/config.gz' >"$KDATA/unit.config" || die "cannot read /proc/config.gz on the unit"
grep -q '^CONFIG_MODVERSIONS=y' "$KDATA/unit.config" || die "this kernel has no symbol versioning: nothing to extract"

log "markers of the export tables"
"${SSHCMD[@]}" "grep -E ' (__start|__stop)___(ksymtab|kcrctab)(_gpl)?\$' /proc/kallsyms" >"$KDATA/ksymtab.txt" \
	|| die "cannot read /proc/kallsyms on the unit"
[ "$(wc -l <"$KDATA/ksymtab.txt")" -eq 8 ] || die "expected 8 table markers, got: $(cat "$KDATA/ksymtab.txt")"
first=$(awk '{print $1}' "$KDATA/ksymtab.txt" | sort | head -1)
VIRT=$(( 0x$first & ~0xFFFFF ))      # start of the 1 MiB block that holds the tables
PHYS=$(( VIRT - 0x70000000 ))        # kernel virtual 0x80000000 = RAM at 0x10000000
SIZE=$(( 2 * 1024 * 1024 ))

log "copying the tables out of memory (2 MiB at physical $(printf '0x%x' $PHYS))"
"${SSHCMD[@]}" "dd if=/dev/mem bs=4096 skip=$(( PHYS / 4096 )) count=$(( SIZE / 4096 )) 2>/dev/null" >"$KDATA/rodata.bin" \
	|| die "cannot read /dev/mem on the unit"
[ "$(stat -c %s "$KDATA/rodata.bin")" -eq $SIZE ] || die "the memory copy has the wrong size"

python3 "$SCRIPTS/mksymvers.py" "$KDATA/rodata.bin" "$(printf '0x%x' $VIRT)" "$KDATA/ksymtab.txt" >"$KDATA/Module.symvers"
n=$(wc -l <"$KDATA/Module.symvers")
grep -q 'module_layout' "$KDATA/Module.symvers" && [ "$n" -gt 2000 ] || die "the symbol table looks wrong ($n entries)"
echo "kernel data: $KDATA ($n exported symbols)"
