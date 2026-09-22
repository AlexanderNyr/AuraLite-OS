; w32a9_registry.asm — W32APP_PLAN.md phase W32A-9 guest gate.
;
; The registry and security surface, end to end, through the documented
; paths: the W32HIVE1 engine behind the Reg* API (A and W as the ledger
; spells them), the HKLM read-mostly policy, the HKCR merge view, the
; single-user SID model, the descriptor builder set, the hash-only
; CryptoAPI over libatls, RtlGenRandom, and the nine FAIL-CLEAN finals
; with their exact codes.
;
; Sections (each prints A9-<NAME>-OK only after all its checks):
;   CORRUPT   probed FIRST: the only corrupt verdict is
;             ERROR_FILE_CORRUPT (1392) from the very first Reg call --
;             every claim is void, print A9-CORRUPT-OK and exit 78.  The
;             gate corrupts the hive in boot 2 and runs this fixture
;             again to land here.  A fresh hive (FILE_NOT_FOUND for
;             HKCU\Software) is NOT corrupt and falls through.
;   HIVE      create/open (disposition NEW then EXISTING), the six value
;             types round-tripped, size-only + MORE_DATA, case-insensitive
;             reopen, enum A (insertion order) + ExW (FILETIME),
;             QueryInfoKey counts/maxes, delete value, the no-subkeys
;             delete contract, flush
;   PERSIST   the cross-boot marker: absent -> write it (boot 1 prints
;             A9-PERSIST-WRITE-OK); present -> verify the exact bytes
;             (boot 2 prints A9-PERSIST-READ-OK).  Same code path both
;             boots; the hive's fate decides.
;   HKLM      the seed answers (ProductName), \Software is writable,
;             everything outside it is refused with ACCESS_DENIED
;   HKCR      HKCU wins, HKLM answers where HKCU is silent, the merged
;             enumeration counts both sides, writes land in the HKCU half
;   SID       build/copy/equal/length/free + CheckTokenMembership against
;             the documented single-user model
;   SD        InitializeSecurityDescriptor + Dacl + Owner, the control
;             bits and the x64 field offsets (owner@8, dacl@32)
;   CRYPTO    the NTE_BAD_ALGID refusals (SHA1/MD5/SHA384), SHA-256/
;             SHA-512/SHA3-256 against the public "abc" vectors, the
;             ragged-feed == one-shot property, HP_* params, provider
;             lifetime (double release refused)
;   RND       two SystemFunction036 draws differ
;   IDENT     GetUserNameA/W (name, length-including-NUL, the short
;             buffer receipt), IsTextUnicode in both directions + BOM
;   FAILCLEAN the nine documented refusals, exact return + last-error
;
; Strings are UTF-16LE one dw per character (the WSTR macro): NASM's
; dw 'ab' packs pairs into words and du is gone in 2.16, so per-char dw
; is the only honest spelling.
;
; The final W32A9-REGISTRY-OK plus exit 78 is what
; tests/integration/cases/test_w32a9_registry.sh asserts; exit 79 is any
; section failure, exit 1 the loader's refusal.

bits 64
default rel

; ---- kernel32 imports --------------------------------------------------------
extern GetStdHandle
extern WriteFile
extern ExitProcess
extern GetLastError

; ---- advapi32 imports (the ledger surface this fixture drives) ----------------
extern RegOpenKeyExA
extern RegOpenKeyExW
extern RegCreateKeyExW
extern RegCloseKey
extern RegQueryValueExA
extern RegQueryValueExW
extern RegSetValueExW
extern RegDeleteValueW
extern RegDeleteKeyW
extern RegDeleteKeyExW
extern RegEnumKeyA
extern RegEnumKeyExW
extern RegQueryInfoKeyW
extern RegFlushKey
extern GetUserNameA
extern GetUserNameW
extern AllocateAndInitializeSid
extern CopySid
extern EqualSid
extern GetLengthSid
extern FreeSid
extern CheckTokenMembership
extern InitializeSecurityDescriptor
extern SetSecurityDescriptorDacl
extern SetSecurityDescriptorOwner
extern IsTextUnicode
extern CryptAcquireContextW
extern CryptReleaseContext
extern CryptCreateHash
extern CryptHashData
extern CryptGetHashParam
extern CryptDestroyHash
extern SystemFunction036
extern LsaOpenPolicy
extern LsaAddAccountRights
extern LsaClose
extern LookupAccountNameW
extern LookupPrivilegeValueW
extern OpenProcessToken
extern AdjustTokenPrivileges
extern GetFileSecurityW
extern SetFileSecurityW

; ---- constants (mirror w32/include/w32/advapi32.h) ----------------------------
%define STD_OUTPUT_HANDLE  -11
%define HKCR               0x80000000
%define HKCU               0x80000001
%define HKLM               0x80000002

%define REG_SZ             1
%define REG_BINARY         3
%define REG_DWORD          4
%define REG_MULTI_SZ       7
%define REG_QWORD          11

%define E_SUCCESS          0
%define E_FILE_NOT_FOUND   2
%define E_ACCESS_DENIED    5
%define E_INVALID_HANDLE   6
%define E_INSUFFICIENT     122
%define E_MORE_DATA        234
%define E_NO_MORE_ITEMS    259
%define E_NO_TOKEN         1008
%define E_NOT_ALL_ASSIGNED 1300
%define E_NONE_MAPPED      1332
%define E_FILE_CORRUPT     1392
%define E_NO_SUCH_PRIV     1530
%define E_NOT_SUPPORTED    50

%define NTE_BAD_ALGID      0x80090008
%define STATUS_ACCESS_DENIED  0xC0000022
%define STATUS_INVALID_HANDLE 0xC0000008

%define PROV_RSA_FULL      1
%define CRYPT_VERIFYCONTEXT 0xF0000000
%define CALG_MD5           0x8003
%define CALG_SHA1          0x8004
%define CALG_SHA_256       0x800C
%define CALG_SHA_384       0x800D
%define CALG_SHA_512       0x800E
%define CALG_SHA3_256      0x8025
%define HP_ALGID           1
%define HP_HASHVAL         2
%define HP_HASHSIZE        4

; ---- macros -------------------------------------------------------------------
; WSTR: one dw per character -- the ONLY way NASM spells UTF-16LE (dw 'ab'
; packs the pair into one word, which is not UTF-16).
%macro WSTR 1-*
    %rep %0
        dw %1
        %rotate 1
    %endrep
%endmacro

; CHK: eax == expected, else jump to the section's fail label.
%macro CHK 2
    cmp  eax, %2
    jne  %1
%endmacro
; CHKGLE: GetLastError() == expected, else fail.
%macro CHKGLE 2
    call GetLastError
    cmp  eax, %2
    jne  %1
%endmacro

; Call-shape macros (Windows x64: rcx,rdx,r8,r9, then stack at rsp+0x20).
; OPEN_W:  RegOpenKeyExW(HK, name) -> hk
%macro OPEN_W 2
    mov  rcx, %1
    lea  rdx, [%2]
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [hk]
    mov  [rsp+0x20], rax
    call RegOpenKeyExW
%endmacro
; OPEN_A:  RegOpenKeyExA(HK, name) -> hk
%macro OPEN_A 2
    mov  rcx, %1
    lea  rdx, [%2]
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [hk]
    mov  [rsp+0x20], rax
    call RegOpenKeyExA
%endmacro
; CREATE_W: RegCreateKeyExW(HK, name) -> hk, disp
%macro CREATE_W 2
    mov  rcx, %1
    lea  rdx, [%2]
    xor  r8d, r8d
    xor  r9d, r9d
    mov  qword [rsp+0x20], 0        ; options (non-volatile)
    mov  qword [rsp+0x28], 0        ; sam
    mov  qword [rsp+0x30], 0        ; security
    lea  rax, [hk]
    mov  [rsp+0x38], rax
    lea  rax, [disp]
    mov  [rsp+0x40], rax
    call RegCreateKeyExW
%endmacro
; SET_W: RegSetValueExW(hk, name, type, bytes, byte_len)
%macro SET_W 4
    mov  rcx, [hk]
    lea  rdx, [%1]
    xor  r8d, r8d
    mov  r9d, %2
    lea  rax, [%3]
    mov  [rsp+0x20], rax
    mov  dword [rsp+0x28], %4
    call RegSetValueExW
%endmacro
; QUERY_W: RegQueryValueExW(hk, name) -> vtype/data/vlen
%macro QUERY_W 1
    mov  rcx, [hk]
    lea  rdx, [%1]
    xor  r8d, r8d
    lea  r9, [vtype]
    lea  rax, [data]
    mov  [rsp+0x20], rax
    lea  rax, [vlen]
    mov  [rsp+0x28], rax
    mov  dword [vlen], 512
    call RegQueryValueExW
%endmacro
; QUERY_A: RegQueryValueExA(hk, name) -> vtype/data/vlen
%macro QUERY_A 1
    mov  rcx, [hk]
    lea  rdx, [%1]
    xor  r8d, r8d
    lea  r9, [vtype]
    lea  rax, [data]
    mov  [rsp+0x20], rax
    lea  rax, [vlen]
    mov  [rsp+0x28], rax
    mov  dword [vlen], 512
    call RegQueryValueExA
%endmacro

section .text
global mainCRTStartup

; p_ok: rsi = bytes, edx = length (the A-8 helper, unchanged).
p_ok:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x40
    mov  rcx, [g_stdout]
    mov  r8d, edx
    mov  rdx, rsi
    lea  r9, [g_written]
    mov  qword [rsp+0x20], 0
    call WriteFile
    add  rsp, 0x40
    pop  rbp
    ret

p_fail:
    sub  rsp, 8
    call p_ok
    mov  ecx, 79
    call ExitProcess

%macro OK 2
    lea  rsi, [%1]
    mov  edx, %2
    call p_ok
%endmacro

%macro FAIL 2
    lea  rsi, [%1]
    mov  edx, %2
    call p_fail
%endmacro

; m_eq: rsi = a, rdi = b, rcx = n.  eax = 0 when equal.
m_eq:
    xor  eax, eax
    test rcx, rcx
    jz   .done
    repe cmpsb
    je   .done
    mov  eax, 1
.done:
    ret

; strcmp_a: rsi = a, rdi = b (ASCIIZ).  eax = 0 when equal.
strcmp_a:
    xor  eax, eax
.loop:
    mov  al, [rsi]
    cmp  al, [rdi]
    jne  .ne
    test al, al
    jz   .eq
    inc  rsi
    inc  rdi
    jmp  .loop
.ne:
    mov  eax, 1
    ret
.eq:
    xor  eax, eax
    ret

; ---- main --------------------------------------------------------------------
; Frame: 0x78 = 32 shadow + 11 stack-arg slots (QueryInfoKeyW needs 8);
; 16-byte aligned at every call site.
mainCRTStartup:
    sub  rsp, 0x78

    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [g_stdout], rax

; ================= CORRUPT (probed first) =====================================
    mov  rcx, HKCU
    lea  rdx, [k_software]
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [hk]
    mov  [rsp+0x20], rax
    call RegOpenKeyExW
    cmp  eax, E_FILE_CORRUPT
    jne  .corrupt_no
    OK   m_corrupt_ok, m_corrupt_ok_len
    mov  ecx, 78
    call ExitProcess
.corrupt_no:
    ; anything else (a fresh hive answers FILE_NOT_FOUND for HKCU\Software,
    ; a seeded one SUCCESS) means the hive is usable -- and if the probe
    ; did open a handle, give it back before the real sections start.
    cmp  eax, E_SUCCESS
    jne  .corrupt_probe_closed
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS
.corrupt_probe_closed:

; ================= HIVE =======================================================
    ; create -> disposition NEW on a fresh hive, EXISTING after the
    ; cross-boot reload (both are the documented contract; the host
    ; gate pins each shape in isolation)
    CREATE_W HKCU, k_sessions
    CHK  hive_fail, E_SUCCESS
    cmp  dword [disp], 1
    je   .disp_fresh
    cmp  dword [disp], 2
    je   .disp_fresh
    jmp  hive_fail
.disp_fresh:
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS
    ; reopen -> disposition EXISTING (always: it was just opened/created)
    CREATE_W HKCU, k_sessions
    CHK  hive_fail, E_SUCCESS
    cmp  dword [disp], 2
    jne  hive_fail

    ; the value types round-trip
    SET_W v_hostname, REG_SZ, s_myhost, s_myhost_len
    CHK  hive_fail, E_SUCCESS
    SET_W v_port, REG_DWORD, d_22, 4
    CHK  hive_fail, E_SUCCESS
    SET_W v_big, REG_QWORD, q_big, 8
    CHK  hive_fail, E_SUCCESS
    SET_W v_raw, REG_BINARY, bin_raw, bin_raw_len
    CHK  hive_fail, E_SUCCESS
    SET_W v_list, REG_MULTI_SZ, s_multi, s_multi_len
    CHK  hive_fail, E_SUCCESS
    ; the default (unnamed) value
    mov  rcx, [hk]
    xor  edx, edx
    xor  r8d, r8d
    mov  r9d, REG_SZ
    lea  rax, [s_myhost]
    mov  [rsp+0x20], rax
    mov  dword [rsp+0x28], s_myhost_len
    call RegSetValueExW
    CHK  hive_fail, E_SUCCESS

    ; query REG_SZ back: type, length, bytes
    QUERY_W v_hostname
    CHK  hive_fail, E_SUCCESS
    cmp  dword [vtype], REG_SZ
    jne  hive_fail
    cmp  dword [vlen], s_myhost_len
    jne  hive_fail
    lea  rsi, [data]
    lea  rdi, [s_myhost]
    mov  rcx, s_myhost_len
    call m_eq
    test eax, eax
    jnz  hive_fail
    ; query REG_QWORD back (the constant rides a register: x86 has no
    ; cmp r64, imm64, only sign-extended imm32)
    QUERY_W v_big
    CHK  hive_fail, E_SUCCESS
    cmp  dword [vtype], REG_QWORD
    jne  hive_fail
    mov  rax, [data]
    mov  rdx, 0x1122334455667788
    cmp  rax, rdx
    jne  hive_fail
    ; the default value answers by NULL name
    mov  rcx, [hk]
    xor  edx, edx
    xor  r8d, r8d
    lea  r9, [vtype]
    lea  rax, [data]
    mov  [rsp+0x20], rax
    lea  rax, [vlen]
    mov  [rsp+0x28], rax
    mov  dword [vlen], 512
    call RegQueryValueExW
    CHK  hive_fail, E_SUCCESS
    cmp  dword [vtype], REG_SZ
    jne  hive_fail
    ; short buffer -> MORE_DATA and the needed size
    mov  rcx, [hk]
    lea  rdx, [v_port]
    xor  r8d, r8d
    lea  r9, [vtype]
    lea  rax, [data]
    mov  [rsp+0x20], rax
    lea  rax, [vlen]
    mov  [rsp+0x28], rax
    mov  dword [vlen], 2
    call RegQueryValueExW
    CHK  hive_fail, E_MORE_DATA
    cmp  dword [vlen], 4
    jne  hive_fail
    ; missing value
    QUERY_W v_nope
    CHK  hive_fail, E_FILE_NOT_FOUND
    ; delete the BINARY value, gone, re-delete refused
    mov  rcx, [hk]
    lea  rdx, [v_raw]
    call RegDeleteValueW
    CHK  hive_fail, E_SUCCESS
    QUERY_W v_raw
    CHK  hive_fail, E_FILE_NOT_FOUND
    mov  rcx, [hk]
    lea  rdx, [v_raw]
    call RegDeleteValueW
    CHK  hive_fail, E_FILE_NOT_FOUND
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS

    ; case-insensitive reopen (ASCII folding)
    OPEN_W HKCU, k_sessions_lower
    CHK  hive_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS
    ; A-variant open + query (PuTTY's spelling)
    OPEN_A HKCU, k_sessions_a
    CHK  hive_fail, E_SUCCESS
    QUERY_A v_hostname_a
    CHK  hive_fail, E_SUCCESS
    cmp  dword [vtype], REG_SZ
    jne  hive_fail
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS

    ; ---- enumeration ---------------------------------------------------------
    CREATE_W HKCU, k_enumtest
    CHK  hive_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS
    %macro MKSUB 1
    CREATE_W HKCU, k_sub_ %+ %1
    CHK  hive_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS
    %endmacro
    MKSUB gamma
    MKSUB alpha
    MKSUB beta2
    CREATE_W HKCU, k_sub_beta2_child
    CHK  hive_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS
    OPEN_W HKCU, k_enumtest
    CHK  hive_fail, E_SUCCESS
    ; RegEnumKeyA [0..2] in insertion order
    mov  rcx, [hk]
    xor  edx, edx
    lea  r8, [name_a]
    mov  r9d, 64
    call RegEnumKeyA
    CHK  hive_fail, E_SUCCESS
    lea  rsi, [name_a]
    lea  rdi, [n_gamma]
    call strcmp_a
    test eax, eax
    jnz  hive_fail
    mov  rcx, [hk]
    mov  edx, 1
    lea  r8, [name_a]
    mov  r9d, 64
    call RegEnumKeyA
    CHK  hive_fail, E_SUCCESS
    lea  rsi, [name_a]
    lea  rdi, [n_alpha]
    call strcmp_a
    test eax, eax
    jnz  hive_fail
    mov  rcx, [hk]
    mov  edx, 2
    lea  r8, [name_a]
    mov  r9d, 64
    call RegEnumKeyA
    CHK  hive_fail, E_SUCCESS
    lea  rsi, [name_a]
    lea  rdi, [n_beta2]
    call strcmp_a
    test eax, eax
    jnz  hive_fail
    mov  rcx, [hk]
    mov  edx, 3
    lea  r8, [name_a]
    mov  r9d, 64
    call RegEnumKeyA
    CHK  hive_fail, E_NO_MORE_ITEMS
    mov  rcx, [hk]
    xor  edx, edx
    lea  r8, [name_a]
    mov  r9d, 3                    ; "gamma" needs 6
    call RegEnumKeyA
    CHK  hive_fail, E_MORE_DATA
    ; RegEnumKeyExW fills a last-write FILETIME
    mov  rcx, [hk]
    xor  edx, edx
    lea  r8, [name_w]
    lea  r9, [cap_w]
    mov  qword [rsp+0x20], 0       ; reserved
    mov  qword [rsp+0x28], 0       ; class
    mov  qword [rsp+0x30], 0       ; class cap
    lea  rax, [ft]
    mov  [rsp+0x38], rax
    mov  dword [cap_w], 64
    call RegEnumKeyExW
    CHK  hive_fail, E_SUCCESS
    cmp  qword [ft], 0
    je   hive_fail
    ; QueryInfoKeyW: 3 subkeys, max name 5, 0 values
    mov  rcx, [hk]
    xor  edx, edx                   ; class
    xor  r8d, r8d                   ; class cap
    xor  r9d, r9d                   ; reserved
    lea  rax, [nsub]
    mov  [rsp+0x20], rax
    lea  rax, [maxsub]
    mov  [rsp+0x28], rax
    mov  qword [rsp+0x30], 0       ; max class
    lea  rax, [nval]
    mov  [rsp+0x38], rax
    mov  qword [rsp+0x40], 0       ; max value name
    mov  qword [rsp+0x48], 0       ; max value len
    mov  qword [rsp+0x50], 0       ; security desc len
    lea  rax, [ft]
    mov  [rsp+0x58], rax
    call RegQueryInfoKeyW
    CHK  hive_fail, E_SUCCESS
    cmp  dword [nsub], 3
    jne  hive_fail
    cmp  dword [maxsub], 5
    jne  hive_fail
    cmp  dword [nval], 0
    jne  hive_fail

    ; ---- delete contracts ----------------------------------------------------
    ; the subkey argument is RELATIVE TO the parent handle, so the full
    ; HKCU paths ride the predefined root, not the EnumTest handle
    ; leaf deletes fine
    mov  rcx, HKCU
    lea  rdx, [k_sub_alpha]
    call RegDeleteKeyW
    CHK  hive_fail, E_SUCCESS
    ; a key with subkeys is refused (the documented contract)
    mov  rcx, HKCU
    lea  rdx, [k_sub_beta2]
    call RegDeleteKeyW
    CHK  hive_fail, E_ACCESS_DENIED
    ; RegDeleteKeyExW empties the leaf, then beta2 goes too
    mov  rcx, HKCU
    lea  rdx, [k_sub_beta2_child]
    xor  r8d, r8d
    xor  r9d, r9d
    call RegDeleteKeyExW
    CHK  hive_fail, E_SUCCESS
    mov  rcx, HKCU
    lea  rdx, [k_sub_beta2]
    call RegDeleteKeyW
    CHK  hive_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hive_fail, E_SUCCESS
    ; flush is observable success
    mov  rcx, HKCU
    call RegFlushKey
    CHK  hive_fail, E_SUCCESS
    OK   m_hive_ok, m_hive_ok_len
    jmp  persist_section
hive_fail:
    FAIL  m_hive_fail, m_hive_fail_len

; ================= PERSIST ====================================================
persist_section:
    mov  rcx, HKCU
    lea  rdx, [k_persist]
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [hk]
    mov  [rsp+0x20], rax
    call RegOpenKeyExW
    cmp  eax, E_FILE_NOT_FOUND
    je   .write
    CHK  persist_fail, E_SUCCESS
    ; boot 2: the marker must answer with the exact bytes
    QUERY_W v_marker
    CHK  persist_fail, E_SUCCESS
    cmp  dword [vlen], s_persist_len
    jne  persist_fail
    lea  rsi, [data]
    lea  rdi, [s_persist]
    mov  rcx, s_persist_len
    call m_eq
    test eax, eax
    jnz  persist_fail
    mov  rcx, [hk]
    call RegCloseKey
    CHK  persist_fail, E_SUCCESS
    OK   m_persist_read_ok, m_persist_read_ok_len
    jmp  hklm_section
.write:
    CREATE_W HKCU, k_persist
    CHK  persist_fail, E_SUCCESS
    SET_W v_marker, REG_SZ, s_persist, s_persist_len
    CHK  persist_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  persist_fail, E_SUCCESS
    OK   m_persist_write_ok, m_persist_write_ok_len
    jmp  hklm_section
persist_fail:
    FAIL  m_persist_fail, m_persist_fail_len

; ================= HKLM =======================================================
hklm_section:
    ; the seed answers (PuTTY/7-Zip spell it in A)
    OPEN_A HKLM, k_seed
    CHK  hklm_fail, E_SUCCESS
    QUERY_A v_product_a
    CHK  hklm_fail, E_SUCCESS
    cmp  dword [vtype], REG_SZ
    jne  hklm_fail
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hklm_fail, E_SUCCESS
    ; \Software is writable
    CREATE_W HKLM, k_lm_install
    CHK  hklm_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hklm_fail, E_SUCCESS
    ; outside \Software: create refused, set refused
    CREATE_W HKLM, k_lm_system
    CHK  hklm_fail, E_ACCESS_DENIED
    mov  rcx, HKLM
    lea  rdx, [v_x]
    xor  r8d, r8d
    mov  r9d, REG_DWORD
    lea  rax, [d_22]
    mov  [rsp+0x20], rax
    mov  dword [rsp+0x28], 4
    call RegSetValueExW
    CHK  hklm_fail, E_ACCESS_DENIED
    OK   m_hklm_ok, m_hklm_ok_len
    jmp  hkcr_section
hklm_fail:
    FAIL  m_hklm_fail, m_hklm_fail_len

; ================= HKCR =======================================================
hkcr_section:
    ; seed both halves: HKCU shell\open, HKLM icon
    CREATE_W HKCU, k_cu_z9
    CHK  hkcr_fail, E_SUCCESS
    SET_W v_cmd, REG_SZ, s_open, s_open_len
    CHK  hkcr_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hkcr_fail, E_SUCCESS
    CREATE_W HKLM, k_lm_z9
    CHK  hkcr_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hkcr_fail, E_SUCCESS
    ; merged enumeration through HKCR: both sides answer
    mov  rcx, HKCR
    lea  rdx, [k_z9]
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [hk]
    mov  [rsp+0x20], rax
    call RegOpenKeyExW
    CHK  hkcr_fail, E_SUCCESS
    mov  rcx, [hk]
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [nsub]
    mov  [rsp+0x20], rax
    mov  qword [rsp+0x28], 0
    mov  qword [rsp+0x30], 0
    mov  qword [rsp+0x38], 0
    mov  qword [rsp+0x40], 0
    mov  qword [rsp+0x48], 0
    mov  qword [rsp+0x50], 0
    mov  qword [rsp+0x58], 0
    call RegQueryInfoKeyW
    CHK  hkcr_fail, E_SUCCESS
    cmp  dword [nsub], 2
    jne  hkcr_fail
    ; the HKCU half answers through the view
    mov  rcx, [hk]
    lea  rdx, [k_shell_open]
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [hk2]
    mov  [rsp+0x20], rax
    call RegOpenKeyExW
    CHK  hkcr_fail, E_SUCCESS
    mov  rcx, [hk2]
    lea  rdx, [v_cmd]
    xor  r8d, r8d
    lea  r9, [vtype]
    lea  rax, [data]
    mov  [rsp+0x20], rax
    lea  rax, [vlen]
    mov  [rsp+0x28], rax
    mov  dword [vlen], 512
    call RegQueryValueExW
    CHK  hkcr_fail, E_SUCCESS
    cmp  dword [vlen], s_open_len
    jne  hkcr_fail
    mov  rcx, [hk2]
    call RegCloseKey
    CHK  hkcr_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hkcr_fail, E_SUCCESS
    ; write through HKCR -> lands in the HKCU half only
    mov  rcx, HKCR
    lea  rdx, [k_y9]
    xor  r8d, r8d
    xor  r9d, r9d
    mov  qword [rsp+0x20], 0
    mov  qword [rsp+0x28], 0
    mov  qword [rsp+0x30], 0
    lea  rax, [hk]
    mov  [rsp+0x38], rax
    lea  rax, [disp]
    mov  [rsp+0x40], rax
    call RegCreateKeyExW
    CHK  hkcr_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hkcr_fail, E_SUCCESS
    OPEN_W HKCU, k_cu_y9
    CHK  hkcr_fail, E_SUCCESS
    mov  rcx, [hk]
    call RegCloseKey
    CHK  hkcr_fail, E_SUCCESS
    OPEN_W HKLM, k_lm_y9
    CHK  hkcr_fail, E_FILE_NOT_FOUND
    OK   m_hkcr_ok, m_hkcr_ok_len
    jmp  sid_section
hkcr_fail:
    FAIL  m_hkcr_fail, m_hkcr_fail_len

; ================= SID ========================================================
sid_section:
    ; S-1-5-32-544 (BUILTIN\Administrators): 11 args, 7 on the stack
    lea  rcx, [auth_nt]
    mov  edx, 2                     ; subauthority count
    mov  r8d, 32
    mov  r9d, 544
    mov  qword [rsp+0x20], 0        ; dw2
    mov  qword [rsp+0x28], 0        ; dw3
    mov  qword [rsp+0x30], 0        ; dw4
    mov  qword [rsp+0x38], 0        ; dw5
    mov  qword [rsp+0x40], 0        ; dw6
    mov  qword [rsp+0x48], 0        ; dw7
    lea  rax, [g_sid]
    mov  [rsp+0x50], rax            ; out
    call AllocateAndInitializeSid
    test eax, eax
    jz   sid_fail
    cmp  qword [g_sid], 0
    je   sid_fail
    ; GetLengthSid == 16
    mov  rcx, [g_sid]
    call GetLengthSid
    cmp  eax, 16
    jne  sid_fail
    ; CopySid(len, dst, src) + EqualSid both ways
    mov  ecx, 64
    lea  rdx, [sid_copy]
    mov  r8, [g_sid]
    call CopySid
    test eax, eax
    jz   sid_fail
    lea  rcx, [sid_copy]
    mov  rdx, [g_sid]
    call EqualSid
    cmp  eax, 1
    jne  sid_fail
    lea  rcx, [sid_bogus]
    mov  rdx, [g_sid]
    call EqualSid
    test eax, eax
    jnz  sid_fail
    ; CheckTokenMembership: Administrators TRUE (the documented model)
    xor  ecx, ecx
    mov  rdx, [g_sid]
    lea  r8, [member]
    call CheckTokenMembership
    cmp  eax, 1
    jne  sid_fail
    cmp  dword [member], 1
    jne  sid_fail
    ; a SID nobody holds: FALSE, still success
    xor  ecx, ecx
    lea  rdx, [sid_bogus]
    lea  r8, [member]
    call CheckTokenMembership
    cmp  eax, 1
    jne  sid_fail
    cmp  dword [member], 0
    jne  sid_fail
    mov  rcx, [g_sid]
    call FreeSid
    OK   m_sid_ok, m_sid_ok_len
    jmp  sd_section
sid_fail:
    FAIL  m_sid_fail, m_sid_fail_len

; ================= SD =========================================================
sd_section:
    lea  rcx, [sd_buf]
    mov  edx, 1
    call InitializeSecurityDescriptor
    cmp  eax, 1
    jne  sd_fail
    cmp  byte [sd_buf], 1
    jne  sd_fail
    cmp  word [sd_buf+2], 0
    jne  sd_fail
    lea  rcx, [sd_buf]
    mov  edx, 1                     ; dacl present
    mov  r8, 0x1234                 ; the dacl pointer
    xor  r9d, r9d                   ; not defaulted
    call SetSecurityDescriptorDacl
    cmp  eax, 1
    jne  sd_fail
    test word [sd_buf+2], 4         ; SE_DACL_PRESENT
    jz   sd_fail
    mov  rax, [sd_buf+32]           ; dacl @ 32 (x64 natural alignment)
    cmp  rax, 0x1234
    jne  sd_fail
    lea  rcx, [sd_buf]
    mov  rdx, 0x5678                ; the owner sid
    mov  r8d, 1                     ; owner defaulted
    call SetSecurityDescriptorOwner
    cmp  eax, 1
    jne  sd_fail
    mov  rax, [sd_buf+8]            ; owner @ 8
    cmp  rax, 0x5678
    jne  sd_fail
    test word [sd_buf+2], 1         ; SE_OWNER_DEFAULTED
    jz   sd_fail
    OK   m_sd_ok, m_sd_ok_len
    jmp  crypto_section
sd_fail:
    FAIL  m_sd_fail, m_sd_fail_len

; ================= CRYPTO =====================================================
crypto_section:
    lea  rcx, [g_prov]
    xor  edx, edx
    xor  r8d, r8d
    mov  r9d, PROV_RSA_FULL
    mov  rax, CRYPT_VERIFYCONTEXT       ; 0xF0000000 does not sign-extend
    mov  [rsp+0x20], rax
    call CryptAcquireContextW
    cmp  eax, 1
    jne  crypto_fail
    cmp  qword [g_prov], 0
    je   crypto_fail
    ; the NTE_BAD_ALGID refusals
    %macro BADALG 1
    mov  rcx, [g_prov]
    mov  edx, %1
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [g_hash]
    mov  [rsp+0x20], rax
    call CryptCreateHash
    test eax, eax
    jnz  crypto_fail
    CHKGLE crypto_fail, NTE_BAD_ALGID
    %endmacro
    BADALG CALG_SHA1
    BADALG CALG_MD5
    BADALG CALG_SHA_384
    ; SHA-256 "abc" against the public vector
    mov  rcx, [g_prov]
    mov  edx, CALG_SHA_256
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [g_hash]
    mov  [rsp+0x20], rax
    call CryptCreateHash
    cmp  eax, 1
    jne  crypto_fail
    mov  rcx, [g_hash]
    lea  rdx, [s_abc]
    mov  r8d, 3
    xor  r9d, r9d
    call CryptHashData
    cmp  eax, 1
    jne  crypto_fail
    mov  rcx, [g_hash]
    mov  edx, HP_HASHSIZE
    lea  r8, [data]
    lea  r9, [vlen]
    mov  qword [rsp+0x20], 0
    mov  dword [vlen], 512
    call CryptGetHashParam
    cmp  eax, 1
    jne  crypto_fail
    cmp  dword [data], 32
    jne  crypto_fail
    mov  rcx, [g_hash]
    mov  edx, HP_HASHVAL
    lea  r8, [digest]
    lea  r9, [vlen]
    mov  qword [rsp+0x20], 0
    mov  dword [vlen], 64
    call CryptGetHashParam
    cmp  eax, 1
    jne  crypto_fail
    cmp  dword [vlen], 32
    jne  crypto_fail
    lea  rsi, [digest]
    lea  rdi, [vec_sha256_abc]
    mov  rcx, 32
    call m_eq
    test eax, eax
    jnz  crypto_fail
    ; HP_ALGID echoes
    mov  rcx, [g_hash]
    mov  edx, HP_ALGID
    lea  r8, [data]
    lea  r9, [vlen]
    mov  qword [rsp+0x20], 0
    mov  dword [vlen], 512
    call CryptGetHashParam
    cmp  eax, 1
    jne  crypto_fail
    cmp  dword [data], CALG_SHA_256
    jne  crypto_fail
    mov  rcx, [g_hash]
    call CryptDestroyHash
    cmp  eax, 1
    jne  crypto_fail
    ; SHA-512 "abc"
    mov  rcx, [g_prov]
    mov  edx, CALG_SHA_512
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [g_hash]
    mov  [rsp+0x20], rax
    call CryptCreateHash
    cmp  eax, 1
    jne  crypto_fail
    mov  rcx, [g_hash]
    lea  rdx, [s_abc]
    mov  r8d, 3
    xor  r9d, r9d
    call CryptHashData
    cmp  eax, 1
    jne  crypto_fail
    mov  rcx, [g_hash]
    mov  edx, HP_HASHVAL
    lea  r8, [digest]
    lea  r9, [vlen]
    mov  qword [rsp+0x20], 0
    mov  dword [vlen], 64
    call CryptGetHashParam
    cmp  eax, 1
    jne  crypto_fail
    lea  rsi, [digest]
    lea  rdi, [vec_sha512_abc]
    mov  rcx, 64
    call m_eq
    test eax, eax
    jnz  crypto_fail
    mov  rcx, [g_hash]
    call CryptDestroyHash
    cmp  eax, 1
    jne  crypto_fail
    ; SHA3-256 "abc"
    mov  rcx, [g_prov]
    mov  edx, CALG_SHA3_256
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [g_hash]
    mov  [rsp+0x20], rax
    call CryptCreateHash
    cmp  eax, 1
    jne  crypto_fail
    mov  rcx, [g_hash]
    lea  rdx, [s_abc]
    mov  r8d, 3
    xor  r9d, r9d
    call CryptHashData
    cmp  eax, 1
    jne  crypto_fail
    mov  rcx, [g_hash]
    mov  edx, HP_HASHVAL
    lea  r8, [digest]
    lea  r9, [vlen]
    mov  qword [rsp+0x20], 0
    mov  dword [vlen], 64
    call CryptGetHashParam
    cmp  eax, 1
    jne  crypto_fail
    lea  rsi, [digest]
    lea  rdi, [vec_sha3_abc]
    mov  rcx, 32
    call m_eq
    test eax, eax
    jnz  crypto_fail
    mov  rcx, [g_hash]
    call CryptDestroyHash
    cmp  eax, 1
    jne  crypto_fail
    ; ragged feed: "a" + "b" + "c" == the "abc" vector (the buffering
    ; contract -- the engine must not depend on call boundaries)
    mov  rcx, [g_prov]
    mov  edx, CALG_SHA_256
    xor  r8d, r8d
    xor  r9d, r9d
    lea  rax, [g_hash]
    mov  [rsp+0x20], rax
    call CryptCreateHash
    cmp  eax, 1
    jne  crypto_fail
    %macro FEED 1
    mov  rcx, [g_hash]
    lea  rdx, [%1]
    mov  r8d, 1
    xor  r9d, r9d
    call CryptHashData
    cmp  eax, 1
    jne  crypto_fail
    %endmacro
    FEED s_a
    FEED s_b
    FEED s_c
    mov  rcx, [g_hash]
    mov  edx, HP_HASHVAL
    lea  r8, [digest]
    lea  r9, [vlen]
    mov  qword [rsp+0x20], 0
    mov  dword [vlen], 64
    call CryptGetHashParam
    cmp  eax, 1
    jne  crypto_fail
    lea  rsi, [digest]
    lea  rdi, [vec_sha256_abc]
    mov  rcx, 32
    call m_eq
    test eax, eax
    jnz  crypto_fail
    mov  rcx, [g_hash]
    call CryptDestroyHash
    cmp  eax, 1
    jne  crypto_fail
    ; provider lifetime: release, then double release refused
    mov  rcx, [g_prov]
    xor  edx, edx
    call CryptReleaseContext
    cmp  eax, 1
    jne  crypto_fail
    mov  rcx, [g_prov]
    xor  edx, edx
    call CryptReleaseContext
    test eax, eax
    jnz  crypto_fail
    CHKGLE crypto_fail, E_INVALID_HANDLE
    OK   m_crypto_ok, m_crypto_ok_len
    jmp  rnd_section
crypto_fail:
    FAIL  m_crypto_fail, m_crypto_fail_len

; ================= RND ========================================================
rnd_section:
    lea  rcx, [rnd1]
    mov  edx, 64
    call SystemFunction036
    cmp  eax, 1
    jne  rnd_fail
    lea  rcx, [rnd2]
    mov  edx, 64
    call SystemFunction036
    cmp  eax, 1
    jne  rnd_fail
    lea  rsi, [rnd1]
    lea  rdi, [rnd2]
    mov  rcx, 64
    call m_eq
    test eax, eax
    jz   rnd_fail                  ; identical draws would be a broken DRBG
    ; and not all zero (pointer walk: an index+RIP-relative operand is
    ; not encodable, and an absolute disp would need a HIGHLOW reloc
    ; the loader rightly refuses)
    lea  rsi, [rnd1]
    xor  eax, eax
    mov  ecx, 64
.zero_check:
    or   al, [rsi]
    inc  rsi
    dec  ecx
    jnz  .zero_check
    test al, al
    jz   rnd_fail
    OK   m_rnd_ok, m_rnd_ok_len
    jmp  ident_section
rnd_fail:
    FAIL  m_rnd_fail, m_rnd_fail_len

; ================= IDENT ======================================================
ident_section:
    lea  rcx, [user_a]
    lea  rdx, [nch]
    mov  dword [nch], 16
    call GetUserNameA
    cmp  eax, 1
    jne  ident_fail
    cmp  dword [nch], 5
    jne  ident_fail
    lea  rsi, [user_a]
    lea  rdi, [n_user]
    call strcmp_a
    test eax, eax
    jnz  ident_fail
    ; short buffer -> FALSE + needed + ERROR_INSUFFICIENT_BUFFER
    lea  rcx, [user_a]
    lea  rdx, [nch]
    mov  dword [nch], 3
    call GetUserNameA
    test eax, eax
    jnz  ident_fail
    cmp  dword [nch], 5
    jne  ident_fail
    CHKGLE ident_fail, E_INSUFFICIENT
    ; W variant
    lea  rcx, [user_w]
    lea  rdx, [nch]
    mov  dword [nch], 16
    call GetUserNameW
    cmp  eax, 1
    jne  ident_fail
    cmp  dword [nch], 5
    jne  ident_fail
    cmp  word [user_w+8], 0        ; L"user" NUL at unit 4
    jne  ident_fail
    ; IsTextUnicode (kernel32_loc.c's engine, bound under ADVAPI32 --
    ; the forwarder shape).  *result is the IN-MASK of tests to run, so
    ; preset the full set: LE text yes, packed ASCII no, odd no, BOM
    ; flagged.
    lea  rcx, [tu_le]
    mov  edx, 10
    lea  r8, [tuf]
    mov  dword [tuf], 0xFFFF
    call IsTextUnicode
    cmp  eax, 1
    jne  ident_fail
    lea  rcx, [tu_ascii]
    mov  edx, 4
    lea  r8, [tuf]
    mov  dword [tuf], 0xFFFF
    call IsTextUnicode
    test eax, eax
    jnz  ident_fail
    lea  rcx, [tu_le]
    mov  edx, 3
    lea  r8, [tuf]
    mov  dword [tuf], 0xFFFF
    call IsTextUnicode
    test eax, eax
    jnz  ident_fail
    test dword [tuf], 0x200        ; ODD_LENGTH reported
    jz   ident_fail
    lea  rcx, [tu_bom]
    mov  edx, 6
    lea  r8, [tuf]
    mov  dword [tuf], 0xFFFF
    call IsTextUnicode
    cmp  eax, 1
    jne  ident_fail
    test dword [tuf], 8            ; IS_TEXT_UNICODE_SIGNATURE
    jz   ident_fail
    OK   m_ident_ok, m_ident_ok_len
    jmp  failclean_section
ident_fail:
    FAIL  m_ident_fail, m_ident_fail_len

; ================= FAILCLEAN ==================================================
failclean_section:
    ; LsaOpenPolicy -> STATUS_ACCESS_DENIED, handle untouched
    mov  rax, 0x1122334455667788
    mov  [lsah], rax
    xor  ecx, ecx
    xor  edx, edx
    xor  r8d, r8d
    lea  r9, [lsah]
    call LsaOpenPolicy
    cmp  eax, STATUS_ACCESS_DENIED
    jne  failclean_fail
    cmp  qword [lsah], 0
    jne  failclean_fail
    xor  ecx, ecx
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    call LsaAddAccountRights
    cmp  eax, STATUS_ACCESS_DENIED
    jne  failclean_fail
    xor  ecx, ecx
    call LsaClose
    cmp  eax, STATUS_INVALID_HANDLE
    jne  failclean_fail
    ; LookupAccountNameW -> FALSE + ERROR_NONE_MAPPED
    xor  ecx, ecx
    lea  rdx, [k_user_w]
    xor  r8d, r8d
    xor  r9d, r9d
    mov  qword [rsp+0x20], 0
    mov  qword [rsp+0x28], 0
    mov  qword [rsp+0x30], 0
    call LookupAccountNameW
    test eax, eax
    jnz  failclean_fail
    CHKGLE failclean_fail, E_NONE_MAPPED
    ; LookupPrivilegeValueW -> FALSE + ERROR_NO_SUCH_PRIVILEGE
    xor  ecx, ecx
    lea  rdx, [k_backup_w]
    lea  r8, [luid]
    call LookupPrivilegeValueW
    test eax, eax
    jnz  failclean_fail
    CHKGLE failclean_fail, E_NO_SUCH_PRIV
    ; OpenProcessToken -> FALSE + ERROR_NO_TOKEN, token NULL
    mov  rcx, -1
    mov  edx, 0x02000000            ; TOKEN_READ shape
    lea  r8, [tok]
    mov  qword [tok], 0x1
    call OpenProcessToken
    test eax, eax
    jnz  failclean_fail
    cmp  qword [tok], 0
    jne  failclean_fail
    CHKGLE failclean_fail, E_NO_TOKEN
    ; AdjustTokenPrivileges -> TRUE + ERROR_NOT_ALL_ASSIGNED
    xor  ecx, ecx
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    mov  qword [rsp+0x20], 0
    mov  qword [rsp+0x28], 0
    call AdjustTokenPrivileges
    cmp  eax, 1
    jne  failclean_fail
    CHKGLE failclean_fail, E_NOT_ALL_ASSIGNED
    ; Get/SetFileSecurityW -> FALSE + ERROR_NOT_SUPPORTED
    lea  rcx, [k_zpath]
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    mov  qword [rsp+0x20], 0
    call GetFileSecurityW
    test eax, eax
    jnz  failclean_fail
    CHKGLE failclean_fail, E_NOT_SUPPORTED
    lea  rcx, [k_zpath]
    xor  edx, edx
    xor  r8d, r8d
    call SetFileSecurityW
    test eax, eax
    jnz  failclean_fail
    CHKGLE failclean_fail, E_NOT_SUPPORTED
    OK   m_failclean_ok, m_failclean_ok_len

    ; ---- the whole-surface receipt ------------------------------------------
    OK   m_all_ok, m_all_ok_len
    mov  ecx, 78
    call ExitProcess

failclean_fail:
    FAIL  m_failclean_fail, m_failclean_fail_len

; ---- data --------------------------------------------------------------------
section .data
g_stdout:  dq 0
g_written: dq 0

; markers (13,10 terminated)
%macro MARKER 2
%1: db %2, 13, 10
%1 %+ _len equ $ - %1
%endmacro
MARKER m_corrupt_ok,       "A9-CORRUPT-OK"
MARKER m_hive_ok,          "A9-HIVE-OK"
MARKER m_hive_fail,        "A9-HIVE-FAIL"
MARKER m_persist_write_ok, "A9-PERSIST-WRITE-OK"
MARKER m_persist_read_ok,  "A9-PERSIST-READ-OK"
MARKER m_persist_fail,     "A9-PERSIST-FAIL"
MARKER m_hklm_ok,          "A9-HKLM-OK"
MARKER m_hklm_fail,        "A9-HKLM-FAIL"
MARKER m_hkcr_ok,          "A9-HKCR-OK"
MARKER m_hkcr_fail,        "A9-HKCR-FAIL"
MARKER m_sid_ok,           "A9-SID-OK"
MARKER m_sid_fail,         "A9-SID-FAIL"
MARKER m_sd_ok,            "A9-SD-OK"
MARKER m_sd_fail,          "A9-SD-FAIL"
MARKER m_crypto_ok,        "A9-CRYPTO-OK"
MARKER m_crypto_fail,      "A9-CRYPTO-FAIL"
MARKER m_rnd_ok,           "A9-RND-OK"
MARKER m_rnd_fail,         "A9-RND-FAIL"
MARKER m_ident_ok,         "A9-IDENT-OK"
MARKER m_ident_fail,       "A9-IDENT-FAIL"
MARKER m_failclean_ok,     "A9-FAILCLEAN-OK"
MARKER m_failclean_fail,   "A9-FAILCLEAN-FAIL"
MARKER m_all_ok,           "W32A9-REGISTRY-OK"

; UTF-16 key/value names (du: UTF-16LE, single-quoted backslash is literal)
k_software:         WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', 0
k_sessions:         WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'W', '3', '2', 'A', '9', '\', 'S', 'e', 's', 's', 'i', 'o', 'n', 's', 0
k_sessions_lower:   WSTR 's', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'w', '3', '2', 'a', '9', '\', 's', 'e', 's', 's', 'i', 'o', 'n', 's', 0
k_enumtest:         WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'E', 'n', 'u', 'm', 'T', 'e', 's', 't', 0
k_sub_gamma:        WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'E', 'n', 'u', 'm', 'T', 'e', 's', 't', '\', 'g', 'a', 'm', 'm', 'a', 0
k_sub_alpha:        WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'E', 'n', 'u', 'm', 'T', 'e', 's', 't', '\', 'A', 'l', 'p', 'h', 'a', 0
k_sub_beta2:        WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'E', 'n', 'u', 'm', 'T', 'e', 's', 't', '\', 'b', 'e', 't', 'a', '2', 0
k_sub_beta2_child:  WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'E', 'n', 'u', 'm', 'T', 'e', 's', 't', '\', 'b', 'e', 't', 'a', '2', '\', 'c', 'h', 'i', 'l', 'd', 0
k_persist:          WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'W', '3', '2', 'A', '9', '\', 'P', 'e', 'r', 's', 'i', 's', 't', 0
k_lm_install:       WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'W', '3', '2', 'A', '9', 'I', 'n', 's', 't', 'a', 'l', 'l', 0
k_lm_system:        WSTR 'S', 'y', 's', 't', 'e', 'm', '\', 'N', 'o', 'p', 'e', 0
k_cu_z9:            WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'C', 'l', 'a', 's', 's', 'e', 's', '\', '.', 'z', '9', '\', 's', 'h', 'e', 'l', 'l', '\', 'o', 'p', 'e', 'n', 0
k_lm_z9:            WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'C', 'l', 'a', 's', 's', 'e', 's', '\', '.', 'z', '9', '\', 'i', 'c', 'o', 'n', 0
k_z9:               WSTR '.', 'z', '9', 0
k_shell_open:       WSTR 's', 'h', 'e', 'l', 'l', '\', 'o', 'p', 'e', 'n', 0
k_y9:               WSTR '.', 'y', '9', '\', 'h', 'a', 'n', 'd', 'l', 'e', 'r', 0
k_cu_y9:            WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'C', 'l', 'a', 's', 's', 'e', 's', '\', '.', 'y', '9', '\', 'h', 'a', 'n', 'd', 'l', 'e', 'r', 0
k_lm_y9:            WSTR 'S', 'o', 'f', 't', 'w', 'a', 'r', 'e', '\', 'C', 'l', 'a', 's', 's', 'e', 's', '\', '.', 'y', '9', '\', 'h', 'a', 'n', 'd', 'l', 'e', 'r', 0
k_user_w:           WSTR 'u', 's', 'e', 'r', 0
k_backup_w:         WSTR 'S', 'e', 'B', 'a', 'c', 'k', 'u', 'p', 'P', 'r', 'i', 'v', 'i', 'l', 'e', 'g', 'e', 0
k_zpath:            WSTR 'C', ':', '\', 'a', 'u', 't', 'o', 'e', 'x', 'e', 'c', '.', 'b', 'a', 't', 0
; ASCII key/value names (the A-variant spellings)
k_sessions_a:      db 'Software\W32A9\Sessions',0
k_seed:            db 'Software\AuraLite\CurrentVersion',0
v_hostname_a:      db 'HostName',0
v_product_a:       db 'ProductName',0

v_hostname:         WSTR 'H', 'o', 's', 't', 'N', 'a', 'm', 'e', 0
v_port:             WSTR 'P', 'o', 'r', 't', 'N', 'u', 'm', 'b', 'e', 'r', 0
v_big:              WSTR 'B', 'i', 'g', 0
v_raw:              WSTR 'R', 'a', 'w', 0
v_list:             WSTR 'L', 'i', 's', 't', 0
v_nope:             WSTR 'N', 'o', 'p', 'e', 0
v_marker:           WSTR 'M', 'a', 'r', 'k', 'e', 'r', 0
v_cmd:              WSTR 'c', 'm', 'd', 0
v_x:                WSTR 'X', 0
n_gamma:  db 'gamma',0
n_alpha:  db 'Alpha',0
n_beta2:  db 'beta2',0
n_user:   db 'user',0

s_myhost:  WSTR 'm', 'y', 'h', 'o', 's', 't', 0
s_myhost_len equ $ - s_myhost
s_multi:   WSTR 'a', 0, 'b', 'c', 0, 0
s_multi_len equ $ - s_multi
s_open:    WSTR 'o', 'p', 'e', 'n', 0
s_open_len equ $ - s_open
s_persist:  WSTR 'W', '3', '2', 'A', '9', '-', 'P', 'E', 'R', 'S', 'I', 'S', 'T', '-', 'V', 'A', 'L', 0
s_persist_len equ $ - s_persist
s_abc:    db 'abc'
s_a:      db 'a'
s_b:      db 'b'
s_c:      db 'c'

d_22:     dd 22
q_big:    dq 0x1122334455667788
bin_raw:  db 1, 2, 3, 4, 0FFh, 0, 9
bin_raw_len equ $ - bin_raw

; S-1-5-32-544 authority (big-endian 5): 6 identifier bytes after 2 pad
auth_nt:  db 0, 0, 0, 0, 0, 0, 0, 5
; a SID nobody holds: revision 1, count 1, authority 5, sub 999
sid_bogus: db 1, 1, 0, 0, 0, 0, 0, 5
          dd 999

; public "abc" vectors (FIPS/Keccak reference values)
vec_sha256_abc: db 0BAh,078h,016h,0BFh,08Fh,001h,0CFh,0EAh
               db 041h,041h,040h,0DEh,05Dh,0AEh,022h,023h
               db 0B0h,003h,061h,0A3h,096h,017h,07Ah,09Ch
               db 0B4h,010h,0FFh,061h,0F2h,000h,015h,0ADh
vec_sha512_abc: db 0DDh,0AFh,035h,0A1h,093h,061h,07Ah,0BAh
               db 0CCh,041h,073h,049h,0AEh,020h,041h,031h
               db 012h,0E6h,0FAh,04Eh,089h,0A9h,07Eh,0A2h
               db 00Ah,09Eh,0EEh,0E6h,04Bh,055h,0D3h,09Ah
               db 021h,092h,099h,02Ah,027h,04Fh,0C1h,0A8h
               db 036h,0BAh,03Ch,023h,0A3h,0FEh,0EBh,0BDh
               db 045h,04Dh,044h,023h,064h,03Ch,0E8h,00Eh
               db 02Ah,09Ah,0C9h,04Fh,0A5h,04Ch,0A4h,09Fh
vec_sha3_abc:   db 03Ah,098h,05Dh,0A7h,04Fh,0E2h,025h,0B2h
               db 004h,05Ch,017h,02Dh,06Bh,0D3h,090h,0BDh
               db 085h,05Fh,008h,06Eh,03Eh,09Dh,052h,05Bh
               db 046h,0BFh,0E2h,045h,011h,043h,015h,032h

; IsTextUnicode vectors
tu_le:    dw 'h','e','l','l','o'          ; UTF-16LE "hello", 10 bytes
tu_ascii: db 'a','b','c','d'
tu_bom:   db 0FFh,0FEh
          dw 'h','i'

; ---- bss ---------------------------------------------------------------------
section .bss
hk:       resq 1
hk2:      resq 1
disp:     resd 1
vtype:    resd 1
vlen:     resd 1
nsub:     resd 1
maxsub:   resd 1
nval:     resd 1
cap_w:    resd 1
nch:      resd 1
tuf:      resd 1
member:   resd 1
lsah:     resq 1
tok:      resq 1
luid:     resq 1
g_sid:    resq 1
g_prov:   resq 1
g_hash:   resq 1
ft:       resq 1
sid_copy: resb 64
sd_buf:   resb 48
data:     resb 512
digest:   resb 64
name_a:   resb 64
name_w:   resb 128
user_a:   resb 16
user_w:   resb 16
rnd1:     resb 64
rnd2:     resb 64
