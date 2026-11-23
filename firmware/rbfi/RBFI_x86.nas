;RawBootFirmwareInterface RBFI v0.4.19
;Copiright (c) 2026 $yscall-(Syscall1dev)
;More info : By default, RBFI looks for the kernel at address 0x00100000,
;exactly one megabyte of memory in 64-bit mode.
;+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
RBFI:
[bits 16]
[org 0xFFFE0000]
startcli:
    cli
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    jmp lol
    align 4
    gdt:
        dq 0x0000000000000000
        dq 0x00CF9A000000FFFF
        dq 0x00CF92000000FFFF
    gdt_end:
    gdt_pointer:
    dw gdt_end - gdt - 1
    dd 0x000E000C
    lol:
;======PROTECTED_MODE???=======
    lgdt [cs:gdt_pointer] 
     mov eax,cr0
     or eax,1
     mov cr0,eax
     jmp dword 0x08:0x00000000000e0042
;=============================
;=============================
;=============================
[bits 32]
start32: 
;======CONF======
   mov ax,0x10
   mov esp,0xFEF0FFFC
   mov ds,ax
   mov ss,ax
   mov es,ax 
;======CPU_ID======
        mov eax,1
        cpuid
        mov edx,eax
        shr edx,4 
        and edx,0x0000000F
        mov ebx,eax
        shr ebx,16
        and ebx,0x0000000F
        shl ebx,4
        add ebx,edx
cmp ebx,0x2A
je sandy_bri
cmp ebx,0x3A
je ivy_bri
cmp ebx,0x3C
je haswell
cmp ebx,0x3D
je broadwell
cmp ebx,0x37
je baytrail

cli
no_found_proccesor:
hlt
jmp no_found_proccesor

sandy_bri:
;======CAR======
  mov ecx,0x2FF
  rdmsr
  and eax,0xFFFFF3FF
  mov ecx,0x200
  mov eax,0xFEF00006
  mov edx,0x00000000
  wrmsr
  mov ecx,0x201
  mov eax,0xFFE00800
  mov edx,0x0000000F
  wrmsr
  mov ecx,0x2FF
  rdmsr
  or eax,0xC00
  wrmsr
  mov edi,0xFEF00000
  mov ecx,0x000C0000
  xor eax,eax
  rep stosd
  mov esp,0xFEF0FFFC
  mov ebp,esp  
;======PCI======
   mov dx,0x0CF8
   mov eax,0x8000083E
   out dx,eax
   mov dx,0x0CFC
   in eax,dx
   or eax,0x0008
   out dx,eax

   mov dx,0x0CF8
   mov eax,0x8000F880
   out dx,eax
   mov dx,0x0CFC
   mov eax,0x00700010
   out dx,eax

   mov al,0x80
   out 0x70,al
   mov dx,0x0CF8
   mov eax,0x8000F804
   out dx,eax
   mov dx,0x0CFC
   in eax,dx
   or eax,0x07
   out dx,eax
;======MMIO======
mov eax,0x80000060
mov dx,0x0CF8
out dx,eax
mov eax,0xE0000001
mov dx,0x0CFC
out dx,eax
;======DDR-3======
mov edi,0xE00FB000
mov byte [edi+4],0xA0
mov [edi+8],byte 0x12
mov [edi+2],byte 0x48
loops:
    test dword [edi],2
    je loops
mov ebx,[edi+5]
push ebx
mov esi,0xE0000000
mov dword [esi+0x48],0xFED10001
mov ecx,0xFED10000
mov [ecx+0x2810],dword 2 
pop ebx
add ebx,1 
mov [ecx+0x4000],ebx
mov [ecx+0x5000],ebx
mov edx,[ecx+0x4010]
or edx,1
mov [ecx+0x4010],edx
jmp loff
;======================
;======================
;======================
ivy_bri:
 ;======CAR======
  mov ecx,0x2FF
 rdmsr
 and eax,0xFFFFF3FF
mov ecx,0x200
mov eax,0xFEF00006
mov edx,0x00000000
wrmsr
 mov ecx,0x201
 mov eax,0xFFE00800
 mov edx,0x0000000F
wrmsr
 mov ecx,0x2FF
 rdmsr
 or eax,0xC00
 wrmsr
 mov edi,0xFEF00000
 mov ecx,0x000C0000
 xor eax,eax
 rep stosd
 mov esp,0xFEF0FFFC
 mov ebp,esp   
;======PCI======
    mov dx,0x0CF8
    mov eax,0x8000083C
    out dx,eax
    mov dx,0x0CFC
    in eax,dx
    or eax,0x000000008
    out dx,eax

    mov eax,0x8000F880
    mov dx,0x0CF8
    out dx,eax
    mov eax,0x00700010
    mov dx,0x0CFC
    out dx,eax

    mov eax,0x8000F804
    mov dx,0x0CF8
    out dx,eax
    mov dx,0x0CFC
    in eax,dx
    or eax,0x00000007
    mov dx,0x0CFC
    out dx,eax

    mov al,0x80
    mov dx,0x70
    out dx,al
;======MMIO======
mov eax,0x80000060
mov dx,0x0CF8
out dx,eax
mov eax,0xE0000001
mov dx,0x0CFC
out dx,eax
;======DDR-3======
mov edi,0xE00FB000
mov byte [edi+4],0xA0
mov [edi+8],byte 0x12
mov [edi+2],byte 0x48
loope:
    test dword [edi],2
    je loope
mov ebx,[edi+5]
push ebx
mov esi,0xE0000000
mov dword [esi+0x48],0xFED10001
mov ecx,0xFED10000
mov [ecx+0x2810],dword 2 
pop ebx
add ebx,1 
mov [ecx+0x4000],ebx
mov [ecx+0x5000],ebx
mov edx,[ecx+0x4010]
or edx,1
mov [ecx+0x4010],edx
jmp loff
;======================
;======================
;======================
haswell:
;======MMIO======
mov eax,0x80000060
mov dx,0x0CF8
out dx,eax

mov eax,0xE0000001
mov dx,0x0CFC
out dx,eax
;======PCIe======
mov eax,[0xE000803C]
or eax,0x00000008
mov [0xE000803C],eax
;======LPC======
mov eax,0x00700010
mov [0xE00F8080],eax
;======PMBASE======
mov eax,0x00000501
mov [0xE00F80AC],eax
;======PCI_command======
mov eax,[0xE00F8004]
or eax,0x00000007
mov [0xE00F8004],eax
;======CAR======
mov ecx,0x000002FF
rdmsr
and eax,0xFFFFF3FF
wrmsr 
mov ecx,0x00000200
mov eax,0xFEF00006
xor edx,edx
wrmsr
mov ecx,0x00000201
mov eax,0xFFFF0800
mov edx,0x0000000F
wrmsr 
mov ecx,0x000002FF
rdmsr
or eax,0x00000C00
wrmsr 
mov edi,0xFEF00000
mov ecx,16386
xor eax,eax
rep stosd
mov esp,0xFEF0FFFC
mov ebp,esp
;======DDR-4======
mov esi,0xE0000000
mov dword [esi+0x48],0xFED10001
mov ecx,0xFED10000
mov dword [ecx+0x5004],0x000100A1
mov dword [ecx+0x3E00],0x00000000
mov edx,0x2710
.loop:
    dec edx
    cmp edx,0
    jne .loop
mov dword [ecx+0x3E00],0x00000003
mov dword [ecx+0x4000],0x10101010
mov dword [ecx+0x4008],0x00000020
mov dword [ecx+0x3F04],0x86001C00
.loopz:
    mov ebx,[ecx+0x3F04]
    test ebx,0x80000000
    jne .loopz
mov dword [ecx+0x3F04],0x85000000
.loope:
    mov ebx,[ecx+0x3F04]
    test ebx,0x80000000
    jne .loope
mov dword [ecx+0x3F04],0x81000001
.loopw:
    mov ebx,[ecx+0x3F04]
    test ebx,0x80000000
    jne .loopw
mov dword [ecx+0x3F04],0x80000510
.loopd:
    mov ebx,[ecx+0x3F04]
    test ebx,0x80000000
    jne .loopd
mov eax,[ecx+0x5000]
or eax,0x00000001
mov [ecx+0x5000],eax
jmp loff
;======================
;======================
;======================
broadwell:
;======MMIO======
mov eax,0x80000060
mov dx,0x0CF8
out dx,eax

mov eax,0xE0000001
mov dx,0x0CFC
out dx,eax
;======PCIe======
mov esi,0xE0000004
mov eax,[esi]
or eax,0x00000007
mov [esi],eax
;======LPC======
mov eax,0x00700010
mov esi,0xE00F8080
mov [esi],eax
;======PMBASE======
mov eax,0x00000501
mov esi,0xE00F8040
mov [esi],eax
;======PCI_command======
mov eax,[0xE00F8004]
or eax,0x00000007
mov [0xE00F8004],eax
;======DDR-4======
mov eax,0xFED10001
mov esi,0xE0000048
mov [esi],eax
mov ecx,0xFED10000
mov edx,[ecx+0x5008]
or edx,0x05
mov [ecx+0x5008],edx
mov dword [ecx+0x4000],0x00000000
mov dword [ecx+0x4008],0x83000000
.loopb:
    mov eax,[ecx+0x4008]
    test eax,0x80000000
    jne .loopb
mov dword [ecx+0x4008],0x82000000
.loopg:
    mov eax,[ecx+0x4008]
    test eax,0x80000000
    jne .loopg
mov dword [ecx+0x4008],0x81000000
.loopy:
    mov eax,[ecx+0x4008]
    test eax,0x80000000
    jne .loopy
mov dword [ecx+0x4008],0x80000100
.loopr:
    mov eax,[ecx+0x4008]
    test eax,0x80000000
    jne .loopr

mov eax,[ecx+0x5000]
or eax,0x00000001
mov [ecx+0x5000],eax
jmp loff
;======================
;======================
;======================
baytrail:
;======MMIO======
mov eax,0x80000060
mov dx,0x0CF8
out dx,eax

mov eax,0xE0000001
mov dx,0x0CFC
out dx,eax
;======PCIE======
mov eax,0xE0000004
mov ebx,dword [eax]
or ebx,0x07
mov [eax],ebx
;======LPC======
mov eax,0xE00F8040
mov dword [eax],0x00700010
mov dword [eax+4],0x00700010
;======PMBASE======
mov eax,0xE00F8060
mov dword [eax],0x00000501
;======PCI_COMM======
mov eax,0xE00F8004
mov ebx,[eax]
or ebx,0x00000007
mov [eax],ebx
;======DDR3L======
mov edi,0xE00FB000
mov byte [edi+4],0xA0
mov [edi+8],byte 0x12
mov [edi+2],byte 0x48
loopc:
    test dword [edi],2
    je loopc
mov ebx,[edi+5]
push ebx
mov esi,0xE0000000
pop ebx
add ebx,1
mov ecx,0xFED10000
mov [ecx+0x70],ebx
mov [ecx+0x74],ebx
mov edx,[ecx+0x70]
or edx,1
mov [ecx+0x70],edx
;======LONG_MODE======
loff:
;======SATA======
mov esi,0xE00FA000
mov [esi+0x24],dword 0xFE044000
mov edx,[esi+0x06]
or edx,0x06
mov [esi+0x04],edx
mov edx,0xFE044000
mov dword [edx+0x100],0x00200000
mov dword [edx+0x108],0x00100000
mov [edx+0x138],dword 1
;======SPEAKER======
mov esi,0xFED08400
mov bl,[esi]
or bl,0x03
mov [esi],bl
;======DEVISE_SEARCHING======
mov esi,0xE0000000
mov edi,0xE00FFFFF
loopm:
    cmp esi,edi
    jge devise_not_found
    mov ax,[esi]
    cmp ax,0xFFFF
    je next_devise
    mov bl,[esi+0x0B]
    cmp bl,0x03
    je enablegpu
   mov bl,[esi+0x0B]
   cmp bl,0x02
   je enablenetwork
next_devise:
    add esi,0x1000
    jmp loopm
devise_not_found:
   jmp conf
enablegpu:
    mov dword [esi+0x10],0xD0000000
    mov ax,[esi+0x04]
    or ax,0x0006
    mov [esi+0x04],ax
jmp next_devise
enablenetwork:
    mov dword [esi+0x10],0xD1000000
    mov ax,[esi+0x04]
    or ax,0x0006
    mov [esi+0x04],ax
    jmp next_devise
;======CONF======
conf:
   mov eax,cr4
   or eax,0x30
   mov cr4,eax
;======PAGING======
   mov edi, 0x00010000
xor eax,eax
mov ecx, 0x6000
rep stosd
mov dword [0x00010000], 0x00011003
mov dword [0x00011000], 0x00012003
mov dword [0x00011008], 0x00013003
mov dword [0x00011010], 0x00014003
mov dword [0x00011018], 0x00015003
jmp paging_setting
mmio_sector:
mov ebx,eax
or ebx,0x18
mov eax,ebx
jmp loopcf
paging_setting:
mov edi,0x00012000
mov eax,0x00000083
mov ecx,2048
loopag:
cmp eax,0xD0000000
jae mmio_sector
loopcf:
 mov [edi],eax
 add eax,0x00200000
 add edi,8
dec ecx
cmp ecx,0
jne loopag
;======CONF======
   mov eax,0x00010000
   mov cr3,eax
   mov ecx,0xC0000080
   rdmsr
   or eax,0x100
   wrmsr
   lgdt[0x000E0000+(gdt64_ptr-startcli)]
   mov eax,cr0
   or eax,0x80000000
   mov cr0,eax
   jmp dword 0x08:long_mode
;=============================
;=============================
;=============================
[bits 64]
long_mode:
;======LONG_MODE======
    mov ax,0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    jmp RBFI_MAIN
;======CONF======
align 8

gdt64:
dq 0x0000000000000000

dq 0x00AF9A000000FFFF

dq 0x00AF92000000FFFF

gdt64_end:

gdt64_ptr:
dw gdt64_end - gdt64 - 1
dq (0x000e0000+(gdt64-startcli))

align 16
stack: 
times 8192 db 0
gdt32:
dq 0
dq 0x00CF9A000000FFFF   
dq 0x00CF92000000FFFF   

gdt32_ptr:
dw gdt32_end - gdt32 - 1
dd gdt32
gdt32_end:

stack_top:
RBFI_MAIN:
;+=+=+=+=+=+=+=+=+=+=+=+=+=+=++=+=+=+=+=+=+=+=+=+=+=+=+=+=+
;=+=+=+=+=+=+=+=+=+=+=+=+=+=+==+=+=+=+=+=+=+=+=+=+=+=+=+=+=
section .text
[bits 64]
;======IDT======
jmp lool2
align 16
idt_table:
times 256 dq 0, 0
idt_end:

align 16
    align 16
idt_ptr:
dw idt_end - idt_table - 1
dq (0xFFFE0000+(idt_table-startcli))
lool2:
xor rcx, rcx
mov rdi,0x000E2210
add rdi,(idt_table-startcli)
.fill_idt:
    lea rax,[rel fault]
    mov rbx, rdi
    mov r8, rcx
    shl r8, 4
    add rbx, r8
    mov [rbx],ax
    
    mov word [rbx+2], 0x08
    mov byte [rbx+5], 0x8E
    mov rdx, rax
    shr rdx, 16
    mov [rbx+6], dx
mov [rbx+4],byte 0
    mov rdx,rax
    shr rdx,32
    mov dword[rbx+8],edx
    mov dword [rbx+12], 0
    inc rcx
    cmp rcx, 256
    jne .fill_idt
    lidt [rel idt_ptr]
    lol2:
;======CULLERS======
    mov dx,0x002E
    mov al,0x87
    out dx,al
    out dx,al

    mov dx,0x002E
    mov al,0x07
    out dx,al
    
    mov dx,0x002F
    mov al,0x0B
    out dx,al
;======COM======
jmp HDMI
engine:
push rax
push rsi
mov rsi,0xE0018014
.loopw:
    mov al,[rsi]
    test al,0x20
    jz .loopw
    mov rsi,0xE0018000
    mov [rsi],al
    pop rsi
    pop rax
    ret
;======HDMI======
HDMI:
mov rax,0xD0000000
mov [rax+0x06014],dword 0x80000000
mov [rax+0x6000C],dword 0x800C0000
mov [rax+0x7200C],dword 0x80000000
mov [rax+0x72008],dword 0x80000000
mov [rax+0xE1140],dword 0x84000000
mov [rax+0xC400C],dword 0x00000200
;======DISPLAY_640x480======
mov rsi,rax
mov dword [rsi+0x60000],0x031F027F
mov dword [rsi+0x60010],0x020C01DF
mov dword [rsi+0x70180],0x86100000
mov dword [rsi+0x70184],0x01000000
or ebx,1<<31
mov dword [rsi+0x70008],ebx
;======USB-2======
uiis:
mov dx,0x0CF8
mov eax, 0x80001810
out dx,eax

mov dx,0x0CFC
in eax,dx

and rax,0xFFFFFFFFFFFFFFF0
mov r15,rax

mov rbx,[r15]
mov r14,r15
mov eax,ebx 
add r14,rax

usb_ff:
mov [rax],r15
movzx rbx,byte [rax]
mov r14,r15
add r14,rbx


mov rax,[r14]
or rax,1
mov [r14],rax
mov dword [r14+0x40],0x1
mov rax,[r14+0x40]
cmp rax,0
jne ussb
hlt
;======BSS======
usb_cbw_size equ 0x00020100
usb_cbw equ 31
;======USB_BOOT======
ussb:
mov eax,[r14+0x40]
or eax,1<<8
mov [r14+0x40],eax
usb_boot:
mov eax,[r14+0x44]
test eax,0x05
jne usb_boot

mov [0x20000], dword 0x20002
mov [0x20004], dword 0x00402001
mov [0x20008], dword 0x40000000
mov [0x2000C], dword 0x20040

mov dword [0x20040],0x1
mov dword [0x20044],0x1
mov dword [0x20048],0x001F0080

lea rax,[abs usb_cbw_size]
mov [rax],dword 0x43425355
mov [rax+4],dword 0x01
mov [rax+8],dword 512
mov [rax+12],byte 0x80
mov [rax+13],byte 0
mov [rax+14],byte 10

mov [rax+15],byte 0x28
mov [rax+16],byte 0
mov [rax+17],dword 0
mov [rax+21],byte 0
mov [rax+22],byte 0
mov [rax+23],byte 1 ;fixed usb boot gg
mov [rax+24],byte 0

mov [0x2004C],rax
mov dword [r14+0x18],0x20000
mov r12,[r14+0x00]
or r12,0x21 
mov [r14+0x00],r12
.loop_ff:
    mov r11,[0x20048]
    mov eax,r11d
    test eax,0x80
    je .loop_ff
;======BOOTING======
mov al,'B'
call engine   
mov al,'o'         
call engine      
mov al,'o'         
call engine        
mov al,'t'         
call engine       
mov al,'i'         
call engine       
mov al,'n'        
call engine       
mov al,'g'        
call engine       
mov al,'.'        
call engine       
mov al,'.'
call engine       
mov al,'.'
call engine
mov [0x20040],dword 0x20060
mov [0x20060],dword 0x1
mov [0x20064],dword 0x1
mov dword [0x20068],0x02000180
mov dword [0x2006C],0x00100000

mov r10,[r14+0x00]
or r10,1<<6
mov [r14+0x00],r10
.loopd:
    mov rax,[0x20068]
    test rax,0x80
    je .loopd

mov [0x20060],dword 0x20080
mov [0x20080],dword 0x1
mov [0x20084],dword 0x1
mov [0x20088],dword 0x000D0180
mov [0x2008C],dword 0x20200
mov rax,[r14+0x00]
or rax,1<<6
mov [r14+0x00],rax

.loopz:
    mov rax,[0x20088]
    test rax,0x80
    je .loopz
jmp start
;=+=+=+=+=+=+=+=+=+=+=+=+=+=+==+=+=+=+=+=+=+=+=+=+=+=+=+=+=
;+=+=+=+=+=+=+=+=+=+=+=+=+=+=++=+=+=+=+=+=+=+=+=+=+=+=+=+=+
;=+=+=+=+=+=+=+=+=+=+=+=+=+=+==+=+=+=+=+=+=+=+=+=+=+=+=+=+=
fault:
    hlt 
    jmp fault
;+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
start:
call launch
hlt
align 8 
rbfi_32 db 0x33, 0x32, 0x62, 0x69, 0x74, 0x73, 0x4F, 0x53
rbfi_64 db 0x6C, 0x6F, 0x6E, 0x67, 0x6D, 0x6F, 0x64, 0x65
launch:
    cmp rax, qword [rel rbfi_32]
    je launch32bit
    cmp rax, qword [rel rbfi_64]
    je launch64bit
    cmp rax, byte 0x00
    je none
    ret
;======BOOT======
launch64bit:
lea rsi, [rel acpi_rsdp]
mov rdi, 0x0009E000
mov ecx, 36
cld
rep movsb
lea rsi, [rel acpi_rsdt]
mov rdi, 0x0009E030
mov ecx, 40
cld
rep movsb
lea rsi,[rel acpi_madt]
mov rdi, 0x0009E060
mov ecx, 64
cld
rep movsb
mov rdi,0x0009E000
    mov rax, 0x00100000
    jmp [rax+0x08]
launch32bit:
lea rsi, [rel acpi_rsdp]
mov rdi, 0x0009E000
mov ecx, 36
cld
rep movsb
lea rsi, [rel acpi_rsdt]
mov rdi, 0x0009E030
mov ecx, 40
cld
rep movsb
lea rsi,[rel acpi_madt]
mov rdi, 0x0009E060
mov ecx, 64
cld
rep movsb
mov rdi,0x0009E000
hlt
jmp launch32bit
;This part of code is  doesn`t ended
;compat32:
;[bits 32]
;lgdt [gdt_pointer]
;compat322:
;mov eax,cr0
;and eax,0x7FFFFFFF
;mov cr0,eax
;jmp far dword [gdt_poiii]
;mov ecx,0xC0000080
;rdmsr 
;mov eax,cr4
;and eax,0xFFFFFFDF
;mov cr4,eax
;cli
;xor ax,ax
;mov ds,ax
;mov es,ax
;mov ss,ax
;mov fs,ax
;mov gs,ax
;mov esp,0x8000FFFF
;jmp dword 0x18:0x0100000
    [bits 64]
none:
hlt
jmp none 
    [bits 64]
;======GDTs======
    gdt_gg:
dq 0

dq 0x00209A0000000000

dq 0x0000920000000000

dq 0x00CF9A000000FFFF

dq 0x00CF92000000FFFF

gdt_gg_end:
CODE64 equ 0x08
DATA64 equ 0x10 
CODE32 equ 0x18
gdt_gg_ptr:
    dw gdt_gg_ptr - gdt_gg -1 
    dq gdt_gg
;gdt_poiii:
;    dd compat322
;    dw 0x18
[bits 16]
    align 4
    gdt2:
        dq 0x0000000000000000
        dq 0x00CF9A000000FFFF
        dq 0x00CF92000000FFFF
    gdt_end2:
    gdt_pointer2:
    dw gdt_end2 - gdt2 - 1
    dd gdt2
[bits 64]
acpi_rsdp:
db 'RSD PTR '
db 0x63
db 'RBFI  '
db 2
dd 0x0009E030
dd 36
dq 0x0009E030
db 0xC3
db 0, 0, 0 
acpi_rsdt:
db 'RSDT'
dd 40
db 1
db 0x31
db 'RBFI  '
db 'RBFI org'
dd 1
db 'SYS1'
dd 1
dd 0x0009E060

acpi_madt:
db 'APIC'
dd 64
db 3
db 0xCD
db 'RBFI  '
db 'RBFI org'
dd 1
db 'SYS1'
dd 1
dd 0xFEE00000
dd 1
db 0, 8, 0, 0
dd 1
db 1, 12, 0, 0
dd 0xFEC00000
dd 0 
;=============================
[bits 16]
TIMES 131072-16-($-$$) db 0
starthd:
    db 0xEA
    dw 0x0000
    dw 0xE000
TIMES 131072 - ($-$$) db 0 
;=============================	
