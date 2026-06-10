#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/../.."
QEMU="qemu-system-i386"

$QEMU \
    -drive format=raw,file=os.img \
    -m 64M \
    -display cocoa,zoom-to-fit=on \
    -vga std \
    -global VGA.vgamem_mb=32 \
    -d guest_errors
