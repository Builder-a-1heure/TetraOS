QEMU="qemu-system-i386"

$QEMU \
    -drive format=raw,file=os.img \
    -m 64M \
    -display cocoa,zoom-to-fit=on \
    -vga std \
    -global VGA.vgamem_mb=32 \
    -d guest_errors
