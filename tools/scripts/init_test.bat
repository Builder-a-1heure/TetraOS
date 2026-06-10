set QEMU="qemu\qemu-system-i386.exe"
echo Lancement de QEMU...
%QEMU% ^
    -drive format=raw,file="os.img" ^
    -m 64M ^
    -serial stdio ^
    -display sdl ^
    -d guest_errors