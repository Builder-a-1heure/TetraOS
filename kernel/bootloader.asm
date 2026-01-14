[BITS 16]
[ORG 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; Affiche un message d'amorçage
    mov si, boot_msg
.print_char:
    lodsb
    or al, al
    jz .after_msg
    mov ah, 0x0E
    int 0x10
    jmp .print_char
.after_msg:


    ; Charger le kernel à 0x100000 → segment:offset = 0x1000:0x0000
    mov ax, 0x1000
    mov es, ax
    xor bx, bx               ; offset 0x0000
    mov ah, 0x02             ; Lire secteur(s)
    mov al, 80               ; Nombre de secteurs à lire (ajuster si besoin)
    mov ch, 0                ; Cylindre 0
    mov cl, 2                ; Secteur 2 (secteur 1 = bootloader)
    mov dh, 0                ; Tête 0
    mov dl, 0x80             ; Disque dur principal
    int 0x13
    jc disk_error

    ; Passer en mode protégé
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:init_pm    ; Saut far pour vider le pipeline


; ---------------- GDT ----------------

gdt_start:
    dq 0x0000000000000000          ; Descripteur nul
    dq 0x00CF9A000000FFFF          ; Code segment 32-bit, base=0, limite=4GiB
    dq 0x00CF92000000FFFF          ; Data segment 32-bit, base=0, limite=4GiB
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ 0x08
DATA_SEG equ 0x10

; ----------- Mode protégé -----------
[BITS 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000               ; Pile dans la RAM

    ; Sauter à l'adresse exacte du kernel
    jmp CODE_SEG:0x00010000              ; Le kernel commence à 0x1000

; ------------- Gestion des erreurs -------------
disk_error:
    mov si, disk_msg
.print_err:
    lodsb
    or al, al
    jz halt
    mov ah, 0x0E
    int 0x10
    jmp .print_err

halt:
    cli
    hlt
    jmp halt

; --------- Messages et signature -----------

boot_msg db "Chargement du kernel...", 0
disk_msg db "Erreur de lecture disque", 0

times 510 - ($ - $$) db 0
dw 0xAA55
