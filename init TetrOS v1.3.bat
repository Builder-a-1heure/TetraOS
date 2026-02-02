set QEMU="qemu\qemu-system-i386.exe"
echo Lancement de QEMU...
%QEMU% ^
    -drive format=raw,file="TetraOS v1.3.img" ^
    -m 64M ^
    -serial stdio ^
    -display sdl ^
    -d guest_errors