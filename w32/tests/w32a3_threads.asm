; w32/tests/w32a3_threads.asm — W32A-3 guest fixture (threads + sync).
;
; Proves under AuraLite's own libc + swapgs what the host suite proves
; against Linux: thread birth/join/codes, CS/SRW/CV/Once/SList, events,
; mutexes (incl. abandonment), semaphores, waits, SleepEx+APC, the
; threadpool, a perf lane, and — last, so a failure still shows partial
; progress — preemptive TerminateThread of a pure spinner (the guest
; kernel kills one TCB; the host cannot).
;
; Exit code 66 means every check passed; 1 means one did not.  Frame rule
; (W32-5): RSP % 16 == 8 at every function entry, == 0 at every CALL, so
; pushes + sub must total 8 mod 16.  Every frame below is annotated.
;
; Imports: 45 (the gate asserts the bound count).

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern CreateThread
extern ExitThread
extern GetCurrentThreadId
extern GetCurrentThread
extern ResumeThread
extern TerminateThread
extern GetExitCodeThread
extern GetThreadTimes
extern SetThreadAffinityMask
extern FreeLibraryAndExitThread
extern InitializeCriticalSection
extern EnterCriticalSection
extern LeaveCriticalSection
extern DeleteCriticalSection
extern AcquireSRWLockExclusive
extern ReleaseSRWLockExclusive
extern TryAcquireSRWLockExclusive
extern SleepConditionVariableSRW
extern WakeAllConditionVariable
extern InitOnceBeginInitialize
extern InitOnceComplete
extern InitializeSListHead
extern InterlockedFlushSList
extern CreateEventA
extern SetEvent
extern ResetEvent
extern CreateMutexA
extern ReleaseMutex
extern CreateSemaphoreW
extern ReleaseSemaphore
extern WaitForSingleObject
extern WaitForMultipleObjects
extern QueueUserAPC
extern SleepEx
extern Sleep
extern GetTickCount64
extern CreateThreadpoolWork
extern SubmitThreadpoolWork
extern CloseThreadpoolWork
extern CloseHandle
extern SetLastError
extern GetLastError

%define INFINITE            0xFFFFFFFF
%define WAIT_OBJECT_0       0
%define WAIT_TIMEOUT        258
%define WAIT_ABANDONED_0    0x80
%define WAIT_IO_COMPLETION  192
%define WAIT_FAILED         0xFFFFFFFF
%define STILL_ACTIVE        259
%define CURRENT_THREAD      -2
%define CREATE_SUSPENDED    4
%define ERROR_SUCCESS       0
%define ERROR_INVALID_PARAMETER 87
%define ERROR_WAIT_TIMEOUT  258
%define ERROR_ALREADY_EXISTS 183
%define ERROR_ABANDONED_WAIT_0 80

section .text

; puts_raw(rsi = buf, edi = len).  1 push + 0x40 = 72 -> 8 mod 16. OK.
puts_raw:
    push rbp
    mov  rbp, rsp
    sub  rsp, 40h
    mov  rcx, [stdout_h]
    mov  rdx, rsi
    mov  r8d, edi
    lea  r9, [written]
    mov  qword [rsp+20h], 0
    call WriteFile
    add  rsp, 40h
    pop  rbp
    ret

; print_u64(rax = value): decimal + newline.  1 push + 0x30. OK.
print_u64:
    push rbx
    sub  rsp, 30h
    lea  rcx, [numbuf+31]
    mov  byte [rcx], 10
    dec  rcx
    mov  r10d, 10
.dig:
    xor  edx, edx
    div  r10                       ; rdx:rax / 10
    add  dl, '0'
    mov  [rcx], dl
    dec  rcx
    test rax, rax
    jnz  .dig
    inc  rcx
    mov  rsi, rcx
    lea  rax, [numbuf+32]
    sub  rax, rcx
    mov  edi, eax
    call puts_raw
    add  rsp, 30h
    pop  rbx
    ret

; --- w_incr(rcx = N): N CS-protected increments of w_sum. ---
; Returns N via w_arg (the loop clobbers RCX).  Phases never overlap, so the
; shared cell is safe.  1 push + 0x30. OK.
w_incr:
    push rbx
    sub  rsp, 30h
    mov  ebx, ecx
    test ebx, ebx
    jz   .done
.loop:
    lea  rcx, [the_cs]
    call EnterCriticalSection
    inc  dword [w_sum]
    lea  rcx, [the_cs]
    call LeaveCriticalSection
    dec  ebx
    jnz  .loop
.done:
    mov  eax, [w_arg]
    add  rsp, 30h
    pop  rbx
    ret

; --- w_incr3(rcx = N): same on cs3/sum3 (contended perf lane). ---
w_incr3:
    push rbx
    sub  rsp, 30h
    mov  ebx, ecx
.loop:
    lea  rcx, [cs3]
    call EnterCriticalSection
    inc  dword [sum3]
    lea  rcx, [cs3]
    call LeaveCriticalSection
    dec  ebx
    jnz  .loop
    xor  eax, eax
    add  rsp, 30h
    pop  rbx
    ret

; --- w_tid(rcx = out ptr): store our tid.  1 push + 0x30. OK. ---
w_tid:
    push rbx
    sub  rsp, 30h
    mov  rbx, rcx
    call GetCurrentThreadId
    mov  [rbx], eax
    xor  eax, eax
    add  rsp, 30h
    pop  rbx
    ret

; --- w_cv_consumer: wait for ready under srw2, count, release. ---
w_cv_consumer:
    push rbx
    sub  rsp, 30h
    lea  rcx, [srw2]
    call AcquireSRWLockExclusive
.wait:
    cmp  dword [cv_ready], 0
    jne  .got
    lea  rcx, [the_cv]
    lea  rdx, [srw2]
    mov  r8d, INFINITE
    xor  r9d, r9d
    call SleepConditionVariableSRW
    jmp  .wait
.got:
    inc  dword [cv_got]
    lea  rcx, [srw2]
    call ReleaseSRWLockExclusive
    xor  eax, eax
    add  rsp, 30h
    pop  rbx
    ret

; --- w_once: InitOnce race. ---
w_once:
    push rbx
    sub  rsp, 30h
    lea  rcx, [the_once]
    xor  edx, edx
    lea  r8, [rsp+20h]              ; &pending (stack scratch)
    xor  r9d, r9d
    call InitOnceBeginInitialize
    test eax, eax
    jz   .done
    cmp  dword [rsp+20h], 0
    je   .done
    inc  dword [once_ran]
    lea  rcx, [the_once]
    xor  edx, edx
    xor  r8d, r8d
    call InitOnceComplete
.done:
    xor  eax, eax
    add  rsp, 30h
    pop  rbx
    ret

; --- w_mutex_spin(rcx = mutex): 500 take/inc/release. ---
w_mutex_spin:
    push rbx
    sub  rsp, 30h
    mov  rbx, rcx
    mov  edi, 500
.loop:
    mov  rcx, rbx
    mov  edx, INFINITE
    call WaitForSingleObject
    inc  dword [sum2]
    mov  rcx, rbx
    call ReleaseMutex
    dec  edi
    jnz  .loop
    xor  eax, eax
    add  rsp, 30h
    pop  rbx
    ret

; --- w_mutex_holder(rcx = mutex): take and exit WITHOUT release. ---
w_mutex_holder:
    push rbx
    sub  rsp, 30h
    mov  edx, INFINITE
    call WaitForSingleObject       ; rcx already the mutex
    xor  eax, eax
    add  rsp, 30h
    pop  rbx
    ret

; --- w_apc_target: alertable sleep; returns its result (192). ---
w_apc_target:
    push rbx
    sub  rsp, 30h
    mov  ecx, INFINITE
    mov  edx, 1
    call SleepEx
    add  rsp, 30h                  ; eax = result, preserved
    pop  rbx
    ret

; --- apc_proc(rcx = data): record + count.  Leaf. ---
apc_proc:
    mov  [apc_got], rcx
    inc  dword [apc_fired]
    ret

; --- pool_cb(instance, ctx, work): ctx in edx; lock-add.  Leaf. ---
pool_cb:
    mov  eax, edx
    lock xadd [pool_sum], eax
    lock inc dword [pool_n]
    ret

; --- w_flib: FreeLibraryAndExitThread(NULL, 44).  Never returns. ---
w_flib:
    xor  ecx, ecx
    mov  edx, 44
    call FreeLibraryAndExitThread
    ret                            ; unreachable

; --- w_exit33: ExitThread(33).  Never returns. ---
w_exit33:
    mov  ecx, 33
    call ExitThread
    ret                            ; unreachable

; --- w_spinner: PURE spin, no w32 calls (preemptive victim). ---
w_spinner:
    pause
    jmp  w_spinner

; --- spawn(rcx = proc, rdx = param, r8d = flags) -> rax = handle -------
; Helper: CreateThread(0, 0, proc, param, flags, &tid_tmp).  1+0x30. OK.
spawn:
    push rbx
    sub  rsp, 30h
    mov  rbx, rcx
    mov  r10, rdx
    mov  r11d, r8d
    xor  ecx, ecx
    xor  edx, edx
    mov  r8, rbx
    mov  r9, r10
    mov  [rsp+20h], r11d
    lea  rax, [tid_tmp]
    mov  [rsp+28h], rax
    call CreateThread
    add  rsp, 30h
    pop  rbx
    ret

; --- join1(rcx = handle) -> eax = wait result.  1 push + 0x28? -----------
; 8 + 0x28 = 48 -> 0 mod 16. WRONG. Use 1 push + 0x30. OK.
join1:
    push rbx
    sub  rsp, 30h
    mov  rbx, rcx
    mov  edx, INFINITE
    call WaitForSingleObject
    mov  rcx, rbx
    add  rsp, 30h
    pop  rbx
    ret                            ; eax = result, rcx = handle (for caller)

; --- entry -----------------------------------------------------------------
; 1 push + 0x58 = 96 -> 0 mod 16. WRONG — use 1 push + 0x50 = 88. OK.
global start
start:
    push rbx
    sub  rsp, 50h

    mov  ecx, -11
    call GetStdHandle
    mov  [stdout_h], rax

    ; --- A. ids: main tid + CURRENT_THREAD + distinct workers ---
    call GetCurrentThreadId
    mov  [main_tid], eax
    test eax, eax
    jz   .fail
    call GetCurrentThread
    cmp  rax, -2
    jne  .fail
    lea  rcx, [w_tid]
    lea  rdx, [tid_a]
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h_tmp], rax
    mov  rcx, rax
    call join1
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [h_tmp]
    call CloseHandle
    lea  rcx, [w_tid]
    lea  rdx, [tid_b]
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h_tmp], rax
    mov  rcx, rax
    call join1
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [h_tmp]
    call CloseHandle
    mov  eax, [tid_a]
    cmp  eax, [tid_b]
    je   .fail
    cmp  eax, [main_tid]
    je   .fail
    mov  eax, [tid_b]
    cmp  eax, [main_tid]
    je   .fail
    lea  rsi, [msg_ids]
    mov  edi, msg_ids_l
    call puts_raw

    ; --- B. 4 workers x CS increments; join; codes; sum ---
    lea  rcx, [the_cs]
    call InitializeCriticalSection
    test eax, eax
    jz   .fail
    mov  dword [w_sum], 0
    mov  dword [w_arg], 2000
    xor  ebx, ebx
.mk4:
    lea  rcx, [w_incr]
    mov  edx, 2000
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h4+rbx*8], rax
    inc  ebx
    cmp  ebx, 4
    jl   .mk4
    ; Race-allowed peek at worker 0's code: STILL_ACTIVE or done.
    mov  rcx, [h4]
    lea  rdx, [ecode]
    call GetExitCodeThread
    test eax, eax
    jz   .fail
    cmp  dword [ecode], STILL_ACTIVE
    je   .peekok
    cmp  dword [ecode], 2000
    jne  .fail
.peekok:
    xor  ebx, ebx
.join4:
    mov  rcx, [h4+rbx*8]
    call join1
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    inc  ebx
    cmp  ebx, 4
    jl   .join4
    xor  ebx, ebx
.code4:
    mov  rcx, [h4+rbx*8]
    lea  rdx, [ecode]
    call GetExitCodeThread
    cmp  dword [ecode], 2000
    jne  .fail
    mov  rcx, [h4+rbx*8]
    call CloseHandle
    inc  ebx
    cmp  ebx, 4
    jl   .code4
    cmp  dword [w_sum], 8000
    jne  .fail
    lea  rcx, [the_cs]
    call DeleteCriticalSection
    lea  rsi, [msg_join]
    mov  edi, msg_join_l
    call puts_raw

    ; --- C. suspended create + ResumeThread counts ---
    mov  dword [w_sum], 0
    mov  dword [w_arg], 100
    lea  rcx, [w_incr]
    mov  edx, 100
    mov  r8d, CREATE_SUSPENDED
    call spawn
    test rax, rax
    jz   .fail
    mov  [h_tmp], rax
    mov  ecx, 50
    call Sleep
    cmp  dword [w_sum], 0
    jne  .fail
    mov  rcx, [h_tmp]
    call ResumeThread
    cmp  eax, 1
    jne  .fail
    mov  rcx, [h_tmp]
    call ResumeThread
    test eax, eax
    jnz  .fail
    mov  rcx, [h_tmp]
    call join1
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    cmp  dword [w_sum], 100
    jne  .fail
    mov  rcx, [h_tmp]
    lea  rdx, [ecode]
    call GetExitCodeThread
    cmp  dword [ecode], 100
    jne  .fail
    mov  rcx, [h_tmp]
    call ResumeThread
    cmp  eax, WAIT_FAILED          ; -1: dead refuses
    jne  .fail
    mov  rcx, [h_tmp]
    call CloseHandle
    lea  rsi, [msg_susp]
    mov  edi, msg_susp_l
    call puts_raw

    ; --- D. times + affinity ---
    mov  rcx, CURRENT_THREAD
    lea  rdx, [ft_c]
    lea  r8, [ft_e]
    lea  r9, [ft_k]
    lea  rax, [ft_u]
    mov  [rsp+20h], rax
    call GetThreadTimes
    test eax, eax
    jz   .fail
    mov  eax, [ft_c]
    or   eax, [ft_c+4]
    jz   .fail                      ; creation must be nonzero
    mov  rcx, CURRENT_THREAD
    mov  rdx, 1
    call SetThreadAffinityMask
    test rax, rax
    jz   .fail                      ; previous mask nonzero
    mov  [aff_prev], rax
    mov  rcx, CURRENT_THREAD
    xor  edx, edx
    call SetThreadAffinityMask
    test rax, rax
    jnz  .fail                      ; zero mask refuses
    call GetLastError
    cmp  eax, ERROR_INVALID_PARAMETER
    jne  .fail
    mov  rcx, CURRENT_THREAD
    mov  rdx, [aff_prev]
    call SetThreadAffinityMask
    lea  rsi, [msg_times]
    mov  edi, msg_times_l
    call puts_raw

    ; --- E. FreeLibraryAndExitThread(NULL, 44): exit unconditional ---
    lea  rcx, [w_flib]
    xor  edx, edx
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h_tmp], rax
    mov  rcx, rax
    call join1
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [h_tmp]
    lea  rdx, [ecode]
    call GetExitCodeThread
    cmp  dword [ecode], 44
    jne  .fail
    mov  rcx, [h_tmp]
    call CloseHandle
    lea  rsi, [msg_flib]
    mov  edi, msg_flib_l
    call puts_raw

    ; --- F. explicit ExitThread(33) ---
    lea  rcx, [w_exit33]
    xor  edx, edx
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h_tmp], rax
    mov  rcx, rax
    call join1
    mov  rcx, [h_tmp]
    lea  rdx, [ecode]
    call GetExitCodeThread
    cmp  dword [ecode], 33
    jne  .fail
    mov  rcx, [h_tmp]
    call CloseHandle
    lea  rsi, [msg_exit33]
    mov  edi, msg_exit33_l
    call puts_raw

    ; --- G. CS recursion, single-threaded ---
    lea  rcx, [cs2]
    call InitializeCriticalSection
    lea  rcx, [cs2]
    call EnterCriticalSection
    lea  rcx, [cs2]
    call EnterCriticalSection
    lea  rcx, [cs2]
    call LeaveCriticalSection
    lea  rcx, [cs2]
    call LeaveCriticalSection
    lea  rcx, [cs2]
    call DeleteCriticalSection
    lea  rsi, [msg_cs]
    mov  edi, msg_cs_l
    call puts_raw

    ; --- H. SRW: no same-thread recursion ---
    lea  rcx, [the_srw]
    call AcquireSRWLockExclusive
    lea  rcx, [the_srw]
    call TryAcquireSRWLockExclusive
    test eax, eax
    jnz  .fail
    lea  rcx, [the_srw]
    call ReleaseSRWLockExclusive
    lea  rcx, [the_srw]
    call TryAcquireSRWLockExclusive
    test eax, eax
    jz   .fail
    lea  rcx, [the_srw]
    call ReleaseSRWLockExclusive
    lea  rsi, [msg_srw]
    mov  edi, msg_srw_l
    call puts_raw

    ; --- I. CV: 2 consumers + WakeAll + timed sleep ---
    mov  dword [cv_ready], 0
    mov  dword [cv_got], 0
    lea  rcx, [w_cv_consumer]
    xor  edx, edx
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h4], rax
    lea  rcx, [w_cv_consumer]
    xor  edx, edx
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h4+8], rax
    mov  ecx, 50
    call Sleep
    lea  rcx, [srw2]
    call AcquireSRWLockExclusive
    mov  dword [cv_ready], 1
    lea  rcx, [the_cv]
    call WakeAllConditionVariable
    lea  rcx, [srw2]
    call ReleaseSRWLockExclusive
    mov  ecx, 2
    lea  rdx, [h4]
    mov  r8d, 1
    mov  r9d, INFINITE
    call WaitForMultipleObjects
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    cmp  dword [cv_got], 2
    jne  .fail
    mov  rcx, [h4]
    call CloseHandle
    mov  rcx, [h4+8]
    call CloseHandle
    lea  rcx, [srw2]
    call AcquireSRWLockExclusive
    lea  rcx, [the_cv]
    lea  rdx, [srw2]
    mov  r8d, 50
    xor  r9d, r9d
    call SleepConditionVariableSRW
    test eax, eax
    jnz  .fail
    call GetLastError
    cmp  eax, ERROR_WAIT_TIMEOUT
    jne  .fail
    lea  rcx, [srw2]
    call ReleaseSRWLockExclusive
    lea  rsi, [msg_cv]
    mov  edi, msg_cv_l
    call puts_raw

    ; --- J. InitOnce race ---
    mov  dword [once_ran], 0
    lea  rcx, [w_once]
    xor  edx, edx
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h4], rax
    lea  rcx, [w_once]
    xor  edx, edx
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h4+8], rax
    mov  ecx, 2
    lea  rdx, [h4]
    mov  r8d, 1
    mov  r9d, INFINITE
    call WaitForMultipleObjects
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    cmp  dword [once_ran], 1
    jne  .fail
    mov  rcx, [h4]
    call CloseHandle
    mov  rcx, [h4+8]
    call CloseHandle
    ; Done once stays done: pending comes back FALSE.
    lea  rcx, [the_once]
    xor  edx, edx
    lea  r8, [once_pending]
    xor  r9d, r9d
    call InitOnceBeginInitialize
    test eax, eax
    jz   .fail
    cmp  dword [once_pending], 0
    jne  .fail
    lea  rsi, [msg_once]
    mov  edi, msg_once_l
    call puts_raw

    ; --- K. SList: manual chain + flush ---
    lea  rcx, [slist_head]
    call InitializeSListHead
    lea  rcx, [slist_head]
    call InterlockedFlushSList
    test rax, rax
    jnz  .fail
    xor  eax, eax
    mov  [e1], rax
    lea  rax, [e1]
    mov  [e2], rax
    lea  rax, [e2]
    mov  [e3], rax
    lea  rax, [e3]
    mov  [slist_head], rax
    mov  word [slist_head+8], 3
    lea  rcx, [slist_head]
    call InterlockedFlushSList
    lea  rcx, [e3]
    cmp  rax, rcx
    jne  .fail
    mov  rax, [rax]                ; e3.next == e2?
    lea  rcx, [e2]
    cmp  rax, rcx
    jne  .fail
    cmp  qword [slist_head], 0
    jne  .fail
    lea  rsi, [msg_slist]
    mov  edi, msg_slist_l
    call puts_raw

    ; --- L. events: manual + auto + named ---
    xor  ecx, ecx
    mov  edx, 1
    xor  r8d, r8d
    xor  r9d, r9d
    call CreateEventA
    test rax, rax
    jz   .fail
    mov  [ev1], rax
    mov  rcx, rax
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_TIMEOUT
    jne  .fail
    mov  rcx, [ev1]
    call SetEvent
    mov  rcx, [ev1]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [ev1]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [ev1]
    call ResetEvent
    mov  rcx, [ev1]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_TIMEOUT
    jne  .fail
    xor  ecx, ecx                  ; auto, nonsignaled
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    call CreateEventA
    test rax, rax
    jz   .fail
    mov  [ev2], rax
    mov  rcx, rax
    call SetEvent
    mov  rcx, [ev2]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [ev2]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_TIMEOUT
    jne  .fail
    xor  ecx, ecx                  ; named pair
    mov  edx, 1
    xor  r8d, r8d
    lea  r9, [ev_name]
    call CreateEventA
    test rax, rax
    jz   .fail
    mov  [ev3], rax
    call GetLastError
    test eax, eax
    jnz  .fail
    xor  ecx, ecx
    mov  edx, 1
    xor  r8d, r8d
    lea  r9, [ev_name]
    call CreateEventA
    test rax, rax
    jz   .fail
    mov  [ev4], rax
    call GetLastError
    cmp  eax, ERROR_ALREADY_EXISTS
    jne  .fail
    mov  rcx, [ev3]
    call SetEvent
    mov  rcx, [ev4]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [ev1]
    call CloseHandle
    mov  rcx, [ev2]
    call CloseHandle
    mov  rcx, [ev3]
    call CloseHandle
    mov  rcx, [ev4]
    call CloseHandle
    lea  rsi, [msg_ev]
    mov  edi, msg_ev_l
    call puts_raw

    ; --- M. mutex: contention + recursion + abandonment ---
    xor  ecx, ecx
    xor  edx, edx
    xor  r8d, r8d
    call CreateMutexA
    test rax, rax
    jz   .fail
    mov  [mtx], rax
    mov  dword [sum2], 0
    xor  ebx, ebx
.mk4m:
    lea  rcx, [w_mutex_spin]
    mov  rdx, [mtx]
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h4+rbx*8], rax
    inc  ebx
    cmp  ebx, 4
    jl   .mk4m
    mov  ecx, 4
    lea  rdx, [h4]
    mov  r8d, 1
    mov  r9d, INFINITE
    call WaitForMultipleObjects
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    cmp  dword [sum2], 2000
    jne  .fail
    xor  ebx, ebx
.cl4m:
    mov  rcx, [h4+rbx*8]
    call CloseHandle
    inc  ebx
    cmp  ebx, 4
    jl   .cl4m
    mov  rcx, [mtx]                ; recursive take x2
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [mtx]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [mtx]
    call ReleaseMutex
    test eax, eax
    jz   .fail
    mov  rcx, [mtx]
    call ReleaseMutex
    test eax, eax
    jz   .fail
    mov  rcx, [mtx]                ; free: refuses
    call ReleaseMutex
    test eax, eax
    jnz  .fail
    xor  ecx, ecx                  ; abandoned: holder exits
    xor  edx, edx
    xor  r8d, r8d
    call CreateMutexA
    test rax, rax
    jz   .fail
    mov  [mtx2], rax
    lea  rcx, [w_mutex_holder]
    mov  rdx, rax
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h_tmp], rax
    mov  rcx, rax
    call join1
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [h_tmp]
    call CloseHandle
    mov  rcx, [mtx2]
    mov  edx, INFINITE
    call WaitForSingleObject
    cmp  eax, WAIT_ABANDONED_0
    jne  .fail
    call GetLastError
    cmp  eax, ERROR_ABANDONED_WAIT_0
    jne  .fail
    mov  rcx, [mtx2]
    call ReleaseMutex
    mov  rcx, [mtx]
    call CloseHandle
    mov  rcx, [mtx2]
    call CloseHandle
    lea  rsi, [msg_mutex]
    mov  edi, msg_mutex_l
    call puts_raw

    ; --- N. semaphore ---
    xor  ecx, ecx
    mov  edx, 2
    mov  r8d, 5
    xor  r9d, r9d
    call CreateSemaphoreW
    test rax, rax
    jz   .fail
    mov  [sem], rax
    mov  rcx, rax
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [sem]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [sem]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_TIMEOUT
    jne  .fail
    mov  rcx, [sem]
    mov  edx, 3
    lea  r8, [sem_prev]
    call ReleaseSemaphore
    test eax, eax
    jz   .fail
    cmp  dword [sem_prev], 0
    jne  .fail
    mov  rcx, [sem]
    mov  edx, 99
    xor  r8d, r8d
    call ReleaseSemaphore
    test eax, eax
    jnz  .fail
    mov  rcx, [sem]
    xor  edx, edx
    xor  r8d, r8d
    call ReleaseSemaphore
    test eax, eax
    jnz  .fail
    xor  ecx, ecx                  ; initial -1: refuses
    mov  edx, -1
    mov  r8d, 5
    xor  r9d, r9d
    call CreateSemaphoreW
    test rax, rax
    jnz  .fail
    mov  rcx, [sem]
    xor  edx, edx
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [sem]
    call CloseHandle
    lea  rsi, [msg_sem]
    mov  edi, msg_sem_l
    call puts_raw

    ; --- O. waits: finite timeout + multi any/all ---
    xor  ecx, ecx
    mov  edx, 1
    xor  r8d, r8d
    xor  r9d, r9d
    call CreateEventA
    test rax, rax
    jz   .fail
    mov  [ev1], rax
    xor  ecx, ecx
    mov  edx, 1
    xor  r8d, r8d
    xor  r9d, r9d
    call CreateEventA
    test rax, rax
    jz   .fail
    mov  [ev2], rax
    call GetTickCount64
    mov  [tick0], rax
    mov  rcx, [ev1]
    mov  edx, 100
    call WaitForSingleObject
    cmp  eax, WAIT_TIMEOUT
    jne  .fail
    call GetLastError
    cmp  eax, ERROR_WAIT_TIMEOUT
    jne  .fail
    call GetTickCount64
    sub  rax, [tick0]
    cmp  rax, 100
    jl   .fail                      ; must really have waited
    cmp  rax, 500
    jg   .fail                      ; generous: loaded-CI jitter
    mov  rax, [ev1]
    mov  [h4], rax
    mov  rax, [ev2]
    mov  [h4+8], rax
    mov  rcx, [ev2]
    call SetEvent
    mov  ecx, 2
    lea  rdx, [h4]
    xor  r8d, r8d
    xor  r9d, r9d
    call WaitForMultipleObjects
    cmp  eax, 1
    jne  .fail
    mov  ecx, 2
    lea  rdx, [h4]
    mov  r8d, 1
    xor  r9d, r9d
    call WaitForMultipleObjects
    cmp  eax, WAIT_TIMEOUT
    jne  .fail
    mov  rcx, [ev1]
    call SetEvent
    mov  ecx, 2
    lea  rdx, [h4]
    mov  r8d, 1
    xor  r9d, r9d
    call WaitForMultipleObjects
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [ev1]
    call CloseHandle
    mov  rcx, [ev2]
    call CloseHandle
    lea  rsi, [msg_wait]
    mov  edi, msg_wait_l
    call puts_raw

    ; --- P. SleepEx + APC ---
    xor  ecx, ecx
    mov  edx, 1
    xor  r8d, r8d
    xor  r9d, r9d
    call CreateEventA
    test rax, rax
    jz   .fail
    mov  [ev1], rax
    lea  rcx, [w_apc_target]
    xor  edx, edx
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h_tmp], rax
    mov  ecx, 50
    call Sleep
    lea  rcx, [apc_proc]
    mov  rdx, [h_tmp]
    mov  r8, 0x7777
    call QueueUserAPC
    test eax, eax
    jz   .fail
    mov  rcx, [h_tmp]
    call join1
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [h_tmp]
    lea  rdx, [ecode]
    call GetExitCodeThread
    cmp  dword [ecode], WAIT_IO_COMPLETION
    jne  .fail
    cmp  dword [apc_fired], 1
    jne  .fail
    cmp  qword [apc_got], 0x7777
    jne  .fail
    mov  rcx, [h_tmp]
    call CloseHandle
    mov  rcx, [ev1]
    call CloseHandle
    lea  rsi, [msg_apc]
    mov  edi, msg_apc_l
    call puts_raw

    ; --- Q. threadpool: 32 items, sum 1..32 ---
    mov  dword [pool_n], 0
    mov  dword [pool_sum], 0
    mov  ebx, 1
.mkpool:
    lea  rcx, [pool_cb]
    mov  edx, ebx
    xor  r8d, r8d
    call CreateThreadpoolWork
    test rax, rax
    jz   .fail
    mov  [pool_w+rbx*8-8], rax
    inc  ebx
    cmp  ebx, 32
    jle  .mkpool
    mov  ebx, 1
.subpool:
    mov  rcx, [pool_w+rbx*8-8]
    call SubmitThreadpoolWork
    inc  ebx
    cmp  ebx, 32
    jle  .subpool
    call GetTickCount64
    add  rax, 10000
    mov  [tick0], rax
.pollpool:
    cmp  dword [pool_n], 32
    je   .pooldone
    call GetTickCount64
    cmp  rax, [tick0]
    jg   .fail
    mov  ecx, 5
    call Sleep
    jmp  .pollpool
.pooldone:
    cmp  dword [pool_sum], 528
    jne  .fail
    mov  ebx, 1
.clpool:
    mov  rcx, [pool_w+rbx*8-8]
    call CloseThreadpoolWork
    inc  ebx
    cmp  ebx, 32
    jle  .clpool
    lea  rsi, [msg_pool]
    mov  edi, msg_pool_l
    call puts_raw

    ; --- R. perf lane: uncontended + contended, printed ---
    lea  rcx, [cs3]
    call InitializeCriticalSection
    call GetTickCount64
    mov  [tick0], rax
    mov  ebx, 1000000
.perf1:
    lea  rcx, [cs3]
    call EnterCriticalSection
    lea  rcx, [cs3]
    call LeaveCriticalSection
    dec  ebx
    jnz  .perf1
    call GetTickCount64
    sub  rax, [tick0]
    mov  [tick1], rax
    lea  rsi, [msg_perf1]
    mov  edi, msg_perf1_l
    call puts_raw
    mov  rax, [tick1]
    call print_u64
    mov  dword [sum3], 0
    xor  ebx, ebx
.mkperf:
    lea  rcx, [w_incr3]
    mov  edx, 250000
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h4+rbx*8], rax
    inc  ebx
    cmp  ebx, 4
    jl   .mkperf
    call GetTickCount64
    mov  [tick0], rax
    mov  ecx, 4
    lea  rdx, [h4]
    mov  r8d, 1
    mov  r9d, INFINITE
    call WaitForMultipleObjects
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    call GetTickCount64
    sub  rax, [tick0]
    mov  [tick1], rax
    cmp  dword [sum3], 1000000
    jne  .fail
    xor  ebx, ebx
.clperf:
    mov  rcx, [h4+rbx*8]
    call CloseHandle
    inc  ebx
    cmp  ebx, 4
    jl   .clperf
    lea  rcx, [cs3]
    call DeleteCriticalSection
    lea  rsi, [msg_perf4]
    mov  edi, msg_perf4_l
    call puts_raw
    mov  rax, [tick1]
    call print_u64
    lea  rsi, [msg_perf]
    mov  edi, msg_perf_l
    call puts_raw

    ; --- S. TerminateThread LAST: a pure spinner, kernel-killed ---
    lea  rcx, [w_spinner]
    xor  edx, edx
    xor  r8d, r8d
    call spawn
    test rax, rax
    jz   .fail
    mov  [h_tmp], rax
    mov  ecx, 50
    call Sleep
    mov  rcx, [h_tmp]
    mov  edx, 55
    call TerminateThread
    test eax, eax
    jz   .fail
    mov  rcx, [h_tmp]
    call join1
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [h_tmp]
    lea  rdx, [ecode]
    call GetExitCodeThread
    cmp  dword [ecode], 55
    jne  .fail
    mov  rcx, [h_tmp]
    call CloseHandle
    lea  rsi, [msg_term]
    mov  edi, msg_term_l
    call puts_raw

    lea  rsi, [msg_ok]
    mov  edi, msg_ok_l
    call puts_raw
    mov  ecx, 66
    call ExitProcess

.fail:
    lea  rsi, [msg_fail]
    mov  edi, msg_fail_l
    call puts_raw
    mov  ecx, 1
    call ExitProcess

section .rdata
msg_ids:    db "IDS-OK", 10
msg_ids_l   equ $ - msg_ids
msg_join:   db "JOIN-OK", 10
msg_join_l  equ $ - msg_join
msg_susp:   db "SUSP-OK", 10
msg_susp_l  equ $ - msg_susp
msg_times:  db "TIMES-OK", 10
msg_times_l equ $ - msg_times
msg_flib:   db "FLIB-OK", 10
msg_flib_l  equ $ - msg_flib
msg_exit33: db "EXIT33-OK", 10
msg_exit33_l equ $ - msg_exit33
msg_cs:     db "CS-OK", 10
msg_cs_l    equ $ - msg_cs
msg_srw:    db "SRW-OK", 10
msg_srw_l   equ $ - msg_srw
msg_cv:     db "CV-OK", 10
msg_cv_l    equ $ - msg_cv
msg_once:   db "ONCE-OK", 10
msg_once_l  equ $ - msg_once
msg_slist:  db "SLIST-OK", 10
msg_slist_l equ $ - msg_slist
msg_ev:     db "EV-OK", 10
msg_ev_l    equ $ - msg_ev
msg_mutex:  db "MUTEX-OK", 10
msg_mutex_l equ $ - msg_mutex
msg_sem:    db "SEM-OK", 10
msg_sem_l   equ $ - msg_sem
msg_wait:   db "WAIT-OK", 10
msg_wait_l  equ $ - msg_wait
msg_apc:    db "APC-OK", 10
msg_apc_l   equ $ - msg_apc
msg_pool:   db "POOL-OK", 10
msg_pool_l  equ $ - msg_pool
msg_perf1:  db "PERF-CS1M-MS:", 10
msg_perf1_l equ $ - msg_perf1
msg_perf4:  db "PERF-CS4X-MS:", 10
msg_perf4_l equ $ - msg_perf4
msg_perf:   db "PERF-OK", 10
msg_perf_l  equ $ - msg_perf
msg_term:   db "TERM-OK", 10
msg_term_l  equ $ - msg_term
msg_ok:     db "W32A3-THREADS-OK", 10
msg_ok_l    equ $ - msg_ok
msg_fail:   db "W32A3-THREADS-FAIL", 10
msg_fail_l  equ $ - msg_fail
ev_name:    db "w32a3ev", 0

section .bss
stdout_h:  resq 1
written:   resq 1
main_tid:  resd 1
tid_a:     resd 1
tid_b:     resd 1
tid_tmp:   resd 1
h_tmp:     resq 1
h4:        resq 4
ecode:     resd 1
w_sum:     resd 1
w_arg:     resd 1
sum2:      resd 1
sum3:      resd 1
tick0:     resq 1
tick1:     resq 1
ft_c:      resq 1
ft_e:      resq 1
ft_k:      resq 1
ft_u:      resq 1
aff_prev:  resq 1
the_cs:    resb 24
cs2:       resb 24
cs3:       resb 24
the_srw:   resb 8
srw2:      resb 8
the_cv:    resb 8
cv_ready:  resd 1
cv_got:    resd 1
the_once:  resb 8
once_ran:  resd 1
once_pending: resd 1
slist_head: resb 16
e1:        resq 1
e2:        resq 1
e3:        resq 1
ev1:       resq 1
ev2:       resq 1
ev3:       resq 1
ev4:       resq 1
mtx:       resq 1
mtx2:      resq 1
sem:       resq 1
sem_prev:  resd 1
apc_fired: resd 1
apc_got:   resq 1
pool_n:    resd 1
pool_sum:  resd 1
pool_w:    resq 32
numbuf:    resb 32
