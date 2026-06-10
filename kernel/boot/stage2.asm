[BITS 16]
[ORG 0x7E00]

; =============================================================
; STAGE 2 - TetraOS
; Basé sur la version originale qui fonctionnait.
; Corrections uniquement :
;   1. A20 activé au démarrage
;   2. Buffers VESA déplacés à 0x70000/0x71000
;      (l'original utilisait 0x8000/0x9000 qui écrasaient
;       la structure kernel)
;   3. Scanner DEADBEEF en 32 bits pour trouver start()
; =============================================================

start2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    ; --- A20 via port 0x92 ---
    in  al, 0x92
    or  al, 0x02
    and al, 0xFE
    out 0x92, al

    mov si, msg_load
    call print16

    ; ---------------------------------------------------------
    ; Chargement kernel LBA (code original intact)
    ; ---------------------------------------------------------
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc  no_ext
    cmp bx, 0xAA55
    jne no_ext

    mov ax, KERNEL_SECTORS
    mov [sects_left], ax
    mov ax, LBA_START
    mov [cur_lba], ax
    mov ax, 0x1000
    mov [cur_seg], ax

.loop:
    mov ax, [sects_left]
    test ax, ax
    jz  .load_done
    cmp ax, CHUNK
    jbe .small
    mov ax, CHUNK
.small:
    mov [dap_buf + 2], ax
    mov word [dap_buf + 4], 0x0000
    mov bx, [cur_seg]
    mov [dap_buf + 6], bx
    xor bx, bx
    mov bx, [cur_lba]
    mov [dap_buf + 8], bx
    mov word [dap_buf + 10], 0
    mov dword [dap_buf + 12], 0
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap_buf
    int 0x13
    jc  disk_err
    mov al, '.'
    mov ah, 0x0E
    int 0x10
    mov bx, [dap_buf + 2]
    add [cur_lba], bx
    mov ax, [sects_left]
    sub ax, bx
    mov [sects_left], ax
    mov ax, bx
    shl ax, 5
    add [cur_seg], ax
    jmp .loop

.load_done:
    mov si, msg_ok
    call print16

    ; ---------------------------------------------------------
    ; VESA — CORRECTION : buffers à 0x7000:0 et 0x7100:0
    ; (l'original utilisait 0x0800:0 = 0x8000 et 0x0900:0 = 0x9000
    ;  ce qui écrasait la structure kernel qu'on écrit aussi à 0x8000)
    ; ---------------------------------------------------------
    mov si, msg_vesa
    call print16

    ; GET VBE CONTROLLER INFO → buffer à 0x7000:0000
    mov ax, 0x7000          ; ← était 0x0800
    mov es, ax
    xor di, di
    mov cx, 256
    xor ax, ax
    rep stosw
    xor di, di
    mov word [es:0x0000], 0x4256   ; 'VB'
    mov word [es:0x0002], 0x3245   ; 'E2'

    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne vesa_fail

    cmp word [es:0x0000], 0x4556
    jne vesa_fail
    cmp word [es:0x0002], 0x4153
    jne vesa_fail

    mov ax, [es:0x000E]
    mov [mode_list_off], ax
    mov ax, [es:0x0010]
    mov [mode_list_seg], ax

    ; Parcourir les modes — GET MODE INFO → buffer à 0x7100:0000
    mov ax, [mode_list_seg]
    mov fs, ax
    mov bx, [mode_list_off]

.find_mode:
    mov cx, [fs:bx]
    cmp cx, 0xFFFF
    je  vesa_fail

    add bx, 2
    mov [mode_list_off], bx

    push cx
    mov ax, 0x7100          ; ← était 0x0900
    mov es, ax
    xor di, di
    mov ax, 0x4F01
    int 0x10
    pop cx
    cmp ax, 0x004F
    jne .find_mode

    cmp word [es:0x0012], 1920
    jne .find_mode
    cmp word [es:0x0014], 1080
    jne .find_mode
    cmp byte [es:0x0019], 32
    jne .find_mode
    test word [es:0x0000], 0x0080
    jz  .find_mode

    ; Mode trouvé !
    mov [found_mode], cx

    mov ax, [es:0x0028]
    mov [fb_addr_lo], ax
    mov ax, [es:0x002A]
    mov [fb_addr_hi], ax
    mov ax, [es:0x0010]
    mov [fb_pitch], ax
    mov al, [es:0x0019]
    mov [fb_bpp], al

    mov ax, 0x4F02
    mov bx, [found_mode]
    or  bx, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne vesa_fail

    ; Écrire la structure à 0x9000 pour le kernel
    ; (0x8000 est dans la zone stage2 0x7E00-0x8200 !)
    xor ax, ax
    mov es, ax
    mov word [es:0x9000], 0x4556
    mov word [es:0x9002], 0x4153
    mov ax, [fb_addr_lo]
    mov [es:0x9004], ax
    mov ax, [fb_addr_hi]
    mov [es:0x9006], ax
    mov ax, [fb_pitch]
    mov [es:0x9008], ax
    mov word [es:0x900A], 0
    mov word [es:0x900C], 1920
    mov word [es:0x900E], 0
    mov word [es:0x9010], 1080
    mov word [es:0x9012], 0
    mov al, [fb_bpp]
    mov byte [es:0x9014], al

    mov si, msg_vesa_ok
    call print16
    jmp do_pm

vesa_fail:
    mov si, msg_vesa_fail
    call print16
    xor ax, ax
    mov es, ax
    mov dword [es:0x9000], 0

do_pm:
    cli
    lgdt [gdt_descriptor]   ; charge limit(16) + base(32)
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    ; jmp far 32 bits — préfixe 0x66 pour forcer opérande 32 bits en mode 16
    ; sans ça certains BIOS/CPU tronquent l'adresse à 16 bits
    o32 jmp dword CODE_SEG:init_pm

no_ext:
    mov si, msg_noext
    call print16
.hlt: cli
    hlt
    jmp .hlt

disk_err:
    mov si, msg_err
    call print16
.hlt2: cli
    hlt
    jmp .hlt2

print16:
    lodsb
    or  al, al
    jz  .done
    mov ah, 0x0E
    int 0x10
    jmp print16
.done: ret

; ---------------------------------------------------------
; Données
; ---------------------------------------------------------
boot_drive    db 0x80
cur_lba       dw 0
cur_seg       dw 0x1000
sects_left    dw 0
fb_addr_lo    dw 0
fb_addr_hi    dw 0
fb_pitch      dw 0
fb_bpp        db 0
found_mode    dw 0
mode_list_off dw 0
mode_list_seg dw 0

dap_buf:
    db 0x10, 0x00
    dw 0
    dw 0
    dw 0
    dw 0
    dw 0
    dd 0

msg_load      db "Stage2: chargement...", 0
msg_ok        db " OK", 13, 10, 0
msg_vesa      db "VESA 1920x1080x32...", 0
msg_vesa_ok   db " OK", 13, 10, 0
msg_vesa_fail db " FAIL (VGA)", 13, 10, 0
msg_noext     db "ERR: pas d'ext INT13", 13, 10, 0
msg_err       db "ERR: lecture disque", 13, 10, 0

gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ 0x08
DATA_SEG equ 0x10

; ---------------------------------------------------------
; init_pm — 32 bits
; Scanner DEADBEEF puis bande jaune puis saut kernel
; ---------------------------------------------------------
[BITS 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0xA00000

    ; Scanner DEADBEEF de 0x10000 à 0x32000
    mov edi, 0x10000
.scan:
    cmp edi, 0x32000
    jae .not_found
    cmp dword [edi], 0xDEADBEEF
    je  .found
    add edi, 4
    jmp .scan

.found:
    add edi, 4              ; edi = start()

    ; Bande jaune si fb_addr valide
    movzx ebx, word [0x9004]
    movzx ecx, word [0x9006]
    shl  ecx, 16
    or   ebx, ecx           ; ebx = fb_addr
    test ebx, ebx
    jz   .jump

    movzx ecx, word [0x9008] ; pitch
    xor  edx, edx
.yellow:
    cmp  edx, 8
    jge  .jump
    mov  eax, edx
    imul eax, ecx
    add  eax, ebx
    mov  esi, eax
    mov  eax, 1920
.px:
    mov  dword [esi], 0x00FFFF00
    add  esi, 4
    dec  eax
    jnz  .px
    inc  edx
    jmp  .yellow

.jump:
    jmp edi

.not_found:
    ; DEADBEEF absent → shutdown QEMU
    mov dx, 0x604
    mov ax, 0x2000
    out dx, ax
    cli
    hlt

LBA_START      equ 3
; KERNEL_SECTORS peut être surchargé par make.sh via : nasm -D KERNEL_SECTORS=4000
%ifndef KERNEL_SECTORS
KERNEL_SECTORS equ 500
%endif
CHUNK          equ 64

times 1024 - ($ - $$) db 0