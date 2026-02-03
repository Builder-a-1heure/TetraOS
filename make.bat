@echo off
setlocal enabledelayedexpansion

REM === CONFIGURATION ===
set GCC="R:\Project - TetraOS\TetraOS v2.0\i686\bin\i686-elf-gcc"
set LD="R:\Project - TetraOS\TetraOS v2.0\i686\bin\i686-elf-ld"
set OBJCOPY="R:\Project - TetraOS\TetraOS v2.0\i686\bin\i686-elf-objcopy"
set NASM=nasm
set QEMU="qemu\qemu-system-i386.exe"

REM === NETTOYAGE ===
echo Nettoyage...
del /q kernel\*.o >nul 2>&1
del /q kernel\src\mem\*.o >nul 2>&1
del /q *.bin >nul 2>&1
del /q *.elf >nul 2>&1
del /q os.img >nul 2>&1

REM === BOOTLOADER ===
echo Assemblage du bootloader (MBR)...
%NASM% -f bin kernel\bootloader.asm -o bootloader.bin
if errorlevel 1 goto error

REM === COMPILATION DU KERNEL ===
echo Compilation des fichiers du kernel...
set FILES=main input fs screen utils ata boot_info mem_boot tex session

for %%f in (%FILES%) do (
    echo Compilation de kernel\%%f.c...
    %GCC% -ffreestanding -Wall -Wextra -nostdlib -Ikernel -c kernel\%%f.c -o kernel\%%f.o -g
    if errorlevel 1 goto error
)

REM Compilation des fichiers mémoire dans kernel\src\mem
echo Compilation du code mémoire...
for %%f in (pfa paging heap) do (
    if exist kernel\src\mem\%%f.c (
        echo Compilation de kernel\src\mem\%%f.c...
        %GCC% -ffreestanding -Wall -Wextra -nostdlib -Ikernel -c kernel\src\mem\%%f.c -o kernel\src\mem\%%f.o -g
        if errorlevel 1 goto error
    )
)

REM === LINKAGE DU KERNEL ===
echo Linking du kernel...
%LD% -T kernel\linker.ld -o kernel.elf -Map kernel.map ^
kernel\main.o ^
kernel\input.o ^
kernel\fs.o ^
kernel\screen.o ^
kernel\utils.o ^
kernel\ata.o ^
kernel\boot_info.o ^
kernel\mem_boot.o ^
kernel\tex.o ^
kernel\session.o ^
kernel\src\mem\pfa.o


REM === EXTRACTION DU BINAIRE PUR DU KERNEL ===
echo Extraction du binaire pur...
%OBJCOPY% -O binary kernel.elf kernel.bin
if errorlevel 1 goto error

REM === CREATION DU DISQUE RAW ===
echo Création de os.img vide de 16 Mo...
fsutil file createnew os.img 16777216 >nul

REM === ECRITURE DU BOOTLOADER ===
echo Copie du bootloader dans os.img secteur 0...
python write_lba.py os.img bootloader.bin 0
if errorlevel 1 goto error

REM === ECRITURE DU KERNEL A PARTIR DU SECTEUR 1 ===
echo Insertion du kernel.bin à partir du secteur 1 (offset 512)...
python write_lba.py os.img kernel.bin 1 40
if errorlevel 1 goto error

REM === DEMARRAGE QEMU ===
echo Lancement de QEMU...
%QEMU% ^
    -drive format=raw,file=os.img ^
    -m 64M ^
    -serial stdio ^
    -display sdl ^
    -d guest_errors
goto end

:error
echo ERREUR durant la compilation ou le build.
pause
exit /b 1

:end
echo Compilation et build OK !
pause
