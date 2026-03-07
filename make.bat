@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  TetraOS — Script de build
REM  Structure sources :
REM    kernel/boot/        bootloader.asm, stage2.asm, linker.ld
REM    kernel/drivers/     vesa, vga, mouse, ata, input
REM    kernel/gfx/         screen, vesaanim
REM    kernel/mem/         mem_boot, pfa
REM    kernel/fs/          fs
REM    kernel/ui/          desktop, session
REM    kernel/shell/       main, shell, terminal, editor, tex
REM    kernel/lib/         utils, boot_info
REM  Objets intermédiaires : kernel\compilation\
REM ============================================================

REM === CONFIGURATION ===
set GCC="i686\bin\i686-elf-gcc"
set LD="i686\bin\i686-elf-ld"
set OBJCOPY="i686\bin\i686-elf-objcopy"
set NASM=nasm
set QEMU="qemu\qemu-system-i386.exe"

set OBJ=kernel\compilation
set CFLAGS=-ffreestanding -Wall -Wextra -nostdlib -g ^
    -Ikernel\drivers ^
    -Ikernel\gfx ^
    -Ikernel\mem ^
    -Ikernel\fs ^
    -Ikernel\ui ^
    -Ikernel\shell ^
    -Ikernel\lib ^
    -Ikernel

REM === NETTOYAGE ===
echo [1/7] Nettoyage...
if exist %OBJ% rd /s /q %OBJ%
del /q *.bin  >nul 2>&1
del /q *.elf  >nul 2>&1
del /q *.map  >nul 2>&1
del /q os.img >nul 2>&1

REM === CRÉATION DU DOSSIER COMPILATION ===
echo [2/7] Creation du dossier %OBJ%...
mkdir %OBJ%
if errorlevel 1 goto error

REM === ASSEMBLAGE BOOTLOADER (Stage 1 — MBR) ===
echo [3/7] Assemblage bootloader (Stage 1)...
%NASM% -f bin kernel\boot\bootloader.asm -o bootloader.bin
if errorlevel 1 goto error

REM === ASSEMBLAGE STAGE 2 (chargeur LBA + init VESA) ===
echo [3/7] Assemblage stage2...
%NASM% -f bin kernel\boot\stage2.asm -o stage2.bin
if errorlevel 1 goto error

REM === COMPILATION DU KERNEL ===
echo [4/7] Compilation des sources C...

REM -- shell/ --
echo   shell\main.c
%GCC% %CFLAGS% -c kernel\shell\main.c     -o %OBJ%\main.o
if errorlevel 1 goto error

echo   shell\shell.c
%GCC% %CFLAGS% -c kernel\shell\shell.c    -o %OBJ%\shell.o
if errorlevel 1 goto error

echo   shell\terminal.c
%GCC% %CFLAGS% -c kernel\shell\terminal.c -o %OBJ%\terminal.o
if errorlevel 1 goto error

echo   shell\editor.c
%GCC% %CFLAGS% -c kernel\shell\editor.c   -o %OBJ%\editor.o
if errorlevel 1 goto error

echo   shell\tex.c
%GCC% %CFLAGS% -c kernel\shell\tex.c      -o %OBJ%\tex.o
if errorlevel 1 goto error

REM -- ui/ --
echo   ui\desktop.c
%GCC% %CFLAGS% -c kernel\ui\desktop.c     -o %OBJ%\desktop.o
if errorlevel 1 goto error

echo   ui\session.c
%GCC% %CFLAGS% -c kernel\ui\session.c     -o %OBJ%\session.o
if errorlevel 1 goto error

REM -- fs/ --
echo   fs\fs.c
%GCC% %CFLAGS% -c kernel\fs\fs.c          -o %OBJ%\fs.o
if errorlevel 1 goto error

REM -- gfx/ --
echo   gfx\screen.c
%GCC% %CFLAGS% -c kernel\gfx\screen.c     -o %OBJ%\screen.o
if errorlevel 1 goto error

echo   gfx\vesaanim.c
%GCC% %CFLAGS% -c kernel\gfx\vesaanim.c   -o %OBJ%\vesaanim.o
if errorlevel 1 goto error

REM -- drivers/ --
echo   drivers\vesa.c
%GCC% %CFLAGS% -c kernel\drivers\vesa.c   -o %OBJ%\vesa.o
if errorlevel 1 goto error

echo   drivers\mouse.c
%GCC% %CFLAGS% -c kernel\drivers\mouse.c  -o %OBJ%\mouse.o
if errorlevel 1 goto error

echo   drivers\ata.c
%GCC% %CFLAGS% -c kernel\drivers\ata.c    -o %OBJ%\ata.o
if errorlevel 1 goto error

echo   drivers\input.c
%GCC% %CFLAGS% -c kernel\drivers\input.c  -o %OBJ%\input.o
if errorlevel 1 goto error

REM -- mem/ --
echo   mem\mem_boot.c
%GCC% %CFLAGS% -c kernel\mem\mem_boot.c   -o %OBJ%\mem_boot.o
if errorlevel 1 goto error

echo   mem\pfa.c
%GCC% %CFLAGS% -c kernel\mem\pfa.c        -o %OBJ%\pfa.o
if errorlevel 1 goto error

REM -- lib/ --
echo   lib\utils.c
%GCC% %CFLAGS% -c kernel\lib\utils.c      -o %OBJ%\utils.o
if errorlevel 1 goto error

echo   lib\boot_info.c
%GCC% %CFLAGS% -c kernel\lib\boot_info.c  -o %OBJ%\boot_info.o
if errorlevel 1 goto error

REM === LINKAGE ===
echo [5/7] Linkage du kernel...
%LD% -T kernel\boot\linker.ld -o kernel.elf -Map kernel.map ^
    %OBJ%\main.o ^
    %OBJ%\shell.o ^
    %OBJ%\terminal.o ^
    %OBJ%\editor.o ^
    %OBJ%\tex.o ^
    %OBJ%\desktop.o ^
    %OBJ%\session.o ^
    %OBJ%\fs.o ^
    %OBJ%\screen.o ^
    %OBJ%\vesaanim.o ^
    %OBJ%\vesa.o ^
    %OBJ%\mouse.o ^
    %OBJ%\ata.o ^
    %OBJ%\input.o ^
    %OBJ%\mem_boot.o ^
    %OBJ%\pfa.o ^
    %OBJ%\utils.o ^
    %OBJ%\boot_info.o
if errorlevel 1 goto error

REM === EXTRACTION DU BINAIRE PUR ===
echo [5/7] Extraction du binaire kernel...
%OBJCOPY% -O binary kernel.elf kernel.bin
if errorlevel 1 goto error

REM === CRÉATION IMAGE DISQUE ===
echo [6/7] Creation de os.img (16 Mo)...
fsutil file createnew os.img 16777216 >nul
if errorlevel 1 goto error

echo [6/7] Ecriture bootloader  (LBA 0)...
python write_lba.py os.img bootloader.bin 0
if errorlevel 1 goto error

echo [6/7] Ecriture stage2      (LBA 1)...
python write_lba.py os.img stage2.bin 1
if errorlevel 1 goto error

echo [6/7] Ecriture kernel      (LBA 3)...
python write_lba.py os.img kernel.bin 3
if errorlevel 1 goto error

REM === LANCEMENT QEMU ===
echo [7/7] Lancement de QEMU...
%QEMU% ^
    -drive format=raw,file=os.img ^
    -m 64M ^
    -display sdl ^
    -vga std ^
    -global VGA.vgamem_mb=32 ^
    -d guest_errors
goto end

:error
echo.
echo *** ERREUR lors du build — voir message ci-dessus ***
pause
exit /b 1

:end
echo.
echo Build OK !
pause
