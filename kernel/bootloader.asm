; =============================================================
; BOOTLOADER TetraOS - Stage 1
; Secteur 1 du disque (512 bytes, signature 0xAA55)
;
; Rôle UNIQUE : charger stage2.asm (secteur 2) en 0x7E00
;               puis lui passer la main
;
; Stage 2 s'occupe de tout le reste (chargement kernel, GDT,
; passage en mode protégé).
; =============================================================

[BITS 16]
[ORG 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00


    ; Sauvegarder le numéro de lecteur (dl = 0x80 pour HDD)
    mov [boot_drive], dl

    ; Message de boot
    mov si, boot_msg
.print:
    lodsb
    or  al, al
    jz  .after_msg
    mov ah, 0x0E
    int 0x10
    jmp .print
.after_msg:

    ; =========================================================
    ; Charger stage 2 : 1 secteur, LBA 1 (= secteur 2 du disque)
    ; On utilise INT 13h AH=02h (CHS) ici car stage 2 est
    ; toujours au secteur 2 — on garde CHS pour la compatibilité
    ; maximale avec les vieux BIOS.
    ; Destination : 0x0000:0x7E00 (juste après le bootloader)
    ; =========================================================
    mov ax, 0x0000
    mov es, ax
    mov bx, 0x7E00          ; offset destination

    mov ah, 0x02            ; Lire secteurs (CHS)
    mov al, 2               ; 2 secteurs (stage2 = 1024 bytes)
    mov ch, 0               ; Cylindre 0
    mov cl, 2               ; Secteur 2 (CHS commence à 1)
    mov dh, 0               ; Tête 0
    mov dl, [boot_drive]    ; Numéro de lecteur sauvegardé
    int 0x13
    jc  disk_error

    ; Passer la main à stage 2
    ; On remet dl = boot_drive pour que stage2 l'ait dans dl
    mov dl, [boot_drive]
    jmp 0x0000:0x7E00

disk_error:
    mov si, err_msg
.printerr:
    lodsb
    or  al, al
    jz  .halt
    mov ah, 0x0E
    int 0x10
    jmp .printerr
.halt:
    cli
    hlt
    jmp .halt

; =========================================================
; Données
; =========================================================
boot_drive  db 0x80
boot_msg    db "TetraOS booting...", 13, 10, 0
err_msg     db "ERREUR: impossible de charger stage2", 13, 10, 0

; Remplissage + signature MBR
times 510 - ($ - $$) db 0
dw 0xAA55