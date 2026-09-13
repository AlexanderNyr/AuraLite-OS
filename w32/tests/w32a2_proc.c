/* w32/tests/w32a2_proc.c — W32A-2 guest fixture: processes and system info.
 *
 * Spawn (self-spawn, redirection, refusals), process info, toolhelp
 * snapshots, module names, environment, directories, version identity,
 * machine info, and FormatMessage.  Exits 55/1.
 *
 * The fixture spawns ITSELF for the child cases: a command line
 * containing "w32a2child" prints one marker and exits 42, one containing
 * "w32a2sleep" sleeps for the TerminateProcess case.  No host binary is
 * needed except /bin/hello for the ELF-via-spawn proof.
 */

#include "w32a2_common.h"

static DWORD waitForExit(HANDLE h) {
    DWORD code = STILL_ACTIVE;
    int i;
    for (i = 0; i < 100; i++) {
        if (!GetExitCodeProcess(h, &code))
            break;
        if (code != STILL_ACTIVE)
            break;
        Sleep(10);
    }
    return code;
}

/* selfpath + L" " + arg -> out. */
static void wcmd(const WCHAR *self, const WCHAR *arg, WCHAR *out) {
    DWORD i = 0, j = 0;
    while (self[i] != 0) {
        out[i] = self[i];
        i++;
    }
    out[i++] = L' ';
    while (arg[j] != 0)
        out[i++] = arg[j++];
    out[i] = 0;
}

void __stdcall winstart(void) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR self[512], cmd[600], tmp[512], file[512], w[512];
    char narrow[512], abuf[512];
    DWORD n;
    HANDLE h;

    w32a2_out = GetStdHandle(STD_OUTPUT_HANDLE);

    /* Child modes run first and return without testing. */
    if (wsubstr(GetCommandLineW(), L"w32a2child")) {
        say("W32A2-CHILD-RAN\r\n");
        ExitProcess(42);
    }
    if (wsubstr(GetCommandLineW(), L"w32a2sleep")) {
        Sleep(30000);
        ExitProcess(0);
    }

    n = GetTempPathW(512, tmp);
    CHECKX(n > 2 && n < 500, "proc-tmp");
    if (n < 3 || n >= 500) {
        w32a2_done("PROC");
        return;
    }

    /* Self knowledge. */
    n = GetModuleFileNameW(NULL, self, 512);
    CHECKX(n > 4 && self[0] == L'C', "proc-self");
    CHECKX(wsubstr(self, L"w32a2_proc"), "proc-self-name");
    CHECKX(GetCommandLineW() != NULL && GetCommandLineW()[0] != 0,
        "proc-cmdline");
    CHECKX(wsubstr(GetCommandLineW(), L"w32a2_proc"), "proc-cmdline-self");

    /* Spawn self with stdout redirected; the marker must land in the file. */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    wjoin(tmp, L"w32a2_childout.txt", file);
    DeleteFileW(file);
    h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECKX(h != INVALID_HANDLE_VALUE, "proc-childfile");
    if (h != INVALID_HANDLE_VALUE) {
        STARTUPINFOW si2;
        BOOL ok;
        memset(&si2, 0, sizeof(si2));
        si2.cb = sizeof(si2);
        si2.dwFlags = STARTF_USESTDHANDLES;
        si2.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si2.hStdOutput = h;
        si2.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        wcmd(self, L"w32a2child", cmd);
        memset(&pi, 0, sizeof(pi));
        ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL,
            &si2, &pi);
        CHECKX(CloseHandle(h), "proc-childfile-close");
        CHECKX(ok, "proc-spawn");
        if (ok) {
            CHECKX(waitForExit(pi.hProcess) == 42, "proc-child-code");
            CHECKX(CloseHandle(pi.hProcess), "proc-child-hp");
            CHECKX(CloseHandle(pi.hThread), "proc-child-ht");
            h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL,
                OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                char cbuf[64];
                DWORD got = 0;
                int k, found = 0;
                ReadFile(h, cbuf, sizeof(cbuf) - 1, &got, NULL);
                for (k = 0; k + 14 <= (int)got; k++) {
                    if (memcmp(cbuf + k, "W32A2-CHILD-RAN", 14) == 0)
                        found = 1;
                }
                CHECKX(found, "proc-child-output");
                if (found)
                    say("CHILD-42\r\n");
                CloseHandle(h);
            } else {
                CHECKX(0, "proc-child-reopen");
            }
        }
        DeleteFileW(file);
    }

    /* Terminate a self-spawned sleeper: SIGKILL shows as 137. */
    wcmd(self, L"w32a2sleep", cmd);
    memset(&pi, 0, sizeof(pi));
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si,
        &pi)) {
        DWORD code = STILL_ACTIVE;
        CHECKX(GetExitCodeProcess(pi.hProcess, &code) &&
            code == STILL_ACTIVE, "proc-sleep-active");
        CHECKX(TerminateProcess(pi.hProcess, 3), "proc-terminate");
        CHECKX(waitForExit(pi.hProcess) == 137, "proc-kill-code");
        say("KILL-137\r\n");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CHECKX(0, "proc-sleep-spawn");
    }

    /* An ELF from the image exits through the same path. */
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(L"C:\\bin\\hello", NULL, NULL, NULL, FALSE, 0,
        NULL, NULL, &si, &pi)) {
        CHECKX(waitForExit(pi.hProcess) == 0, "proc-hello-code");
        say("HELLO-0\r\n");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CHECKX(0, "proc-hello-spawn");
    }

    /* Spawn refusals, each by name. */
    CHECKX(!CreateProcessW(L"/no/such/bin", NULL, NULL, NULL, FALSE, 0,
        NULL, NULL, &si, &pi), "proc-missing-fails");
    CHECKX(GetLastError() == ERROR_FILE_NOT_FOUND, "proc-missing-code");
    wjoin(tmp, L"w32a2_notexe.txt", file);
    DeleteFileW(file);
    h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    CHECKX(!CreateProcessW(file, NULL, NULL, NULL, FALSE, 0, NULL, NULL,
        &si, &pi), "proc-badexe-fails");
    CHECKX(GetLastError() == ERROR_BAD_EXE_FORMAT, "proc-badexe-code");
    DeleteFileW(file);
    CHECKX(!CreateProcessW(L"C:\\bin\\hello", NULL, NULL, NULL, FALSE,
        CREATE_SUSPENDED, NULL, NULL, &si, &pi), "proc-susp-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "proc-susp-code");
    CHECKX(!CreateProcessW(L"C:\\bin\\hello", NULL, NULL, NULL, FALSE, 0,
        NULL, L"/no/such/dir", &si, &pi), "proc-badcwd-fails");
    CHECKX(GetLastError() == ERROR_PATH_NOT_FOUND, "proc-badcwd-code");

    /* Custom environment block parses. */
    {
        static WCHAR block[32];
        block[0] = L'W'; block[1] = L'3'; block[2] = L'2';
        block[3] = L'='; block[4] = L'1'; block[5] = 0; block[6] = 0;
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessW(L"C:\\bin\\hello", NULL, NULL, NULL, FALSE, 0,
            block, NULL, &si, &pi)) {
            CHECKX(waitForExit(pi.hProcess) == 0, "proc-envblock-code");
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            CHECKX(0, "proc-envblock-spawn");
        }
    }

    /* The A form spawns too. */
    {
        STARTUPINFOA sia;
        DWORD k = 0;
        memset(&sia, 0, sizeof(sia));
        sia.cb = sizeof(sia);
        while (self[k] != 0 && k < sizeof(narrow) - 9) {
            narrow[k] = (char)self[k];
            k++;
        }
        narrow[k] = 0;
        {
            const char *tail = " w32a2child";
            DWORD t = 0;
            while (tail[t] != 0)
                narrow[k++] = tail[t++];
            narrow[k] = 0;
        }
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessA(NULL, narrow, NULL, NULL, FALSE, 0, NULL,
            NULL, &sia, &pi)) {
            CHECKX(waitForExit(pi.hProcess) == 42, "proc-afrom-code");
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            CHECKX(0, "proc-afrom-spawn");
        }
    }

    /* Process info. */
    {
        STARTUPINFOW si0;
        HANDLE me;
        DWORD code = 0;
        GetStartupInfoW(&si0);
        CHECKX(si0.cb == sizeof(si0), "proc-si-cb");
        CHECKX(GetCurrentProcessId() != 0, "proc-pid");
        me = OpenProcess(0, FALSE, GetCurrentProcessId());
        CHECKX(me != NULL, "proc-open-self");
        if (me != NULL) {
            FILETIME cr, ex, kn, us;
            DWORD_PTR pm, sm;
            CHECKX(GetExitCodeProcess(me, &code) && code == STILL_ACTIVE,
                "proc-active");
            CHECKX(GetProcessTimes(me, &cr, &ex, &kn, &us), "proc-times");
            CHECKX(cr.dwHighDateTime != 0 || cr.dwLowDateTime != 0,
                "proc-created");
            CHECKX(ex.dwLowDateTime == 0, "proc-noexit");
            CHECKX(GetProcessAffinityMask(me, &pm, &sm), "proc-affinity");
            CHECKX(pm != 0 && sm != 0, "proc-affinity-mask");
            CHECKX(CloseHandle(me), "proc-open-close");
        }
        CHECKX(GetExitCodeProcess(GetCurrentProcess(), &code) &&
            code == STILL_ACTIVE, "proc-self-active");
        CHECKX(OpenProcess(0, FALSE, 0x7FFFFFFFu) == NULL,
            "proc-badpid-fails");
        CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
            "proc-badpid-code");
    }

    /* The snapshot finds us. */
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        CHECKX(snap != NULL && snap != INVALID_HANDLE_VALUE, "proc-snap");
        if (snap != NULL && snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            int found = 0;
            memset(&pe, 0, sizeof(pe));
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (pe.th32ProcessID == GetCurrentProcessId()) {
                        found = 1;
                        CHECKX(pe.szExeFile[0] != 0, "proc-snap-name");
                        CHECKX(pe.cntThreads == 1, "proc-snap-threads");
                    }
                } while (Process32NextW(snap, &pe));
                CHECKX(GetLastError() == ERROR_NO_MORE_FILES,
                    "proc-snap-exhausted");
            }
            CHECKX(found, "proc-snap-found");
            CHECKX(CloseHandle(snap), "proc-snap-close");
        }
        CHECKX(CreateToolhelp32Snapshot(0xFFFFu, 0) == NULL ||
            CreateToolhelp32Snapshot(0xFFFFu, 0) == INVALID_HANDLE_VALUE,
            "proc-snap-badflags");
        CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
            "proc-snap-badflags-code");
    }

    /* Module names. */
    n = GetModuleFileNameA(NULL, abuf, sizeof(abuf));
    CHECKX(n > 4 && abuf[0] == 'C' && abuf[1] == ':', "proc-mod-a");
    {
        int k, found = 0;
        for (k = 0; abuf[k] != 0; k++) {
            if (memcmp(abuf + k, "w32a2_proc", 10) == 0)
                found = 1;
        }
        CHECKX(found, "proc-mod-self");
    }
    CHECKX(GetModuleFileNameA(NULL, abuf, 4) == 4, "proc-mod-short");
    CHECKX(GetLastError() == ERROR_INSUFFICIENT_BUFFER,
        "proc-mod-short-code");
    CHECKX(GetModuleHandleW(NULL) != NULL, "proc-gmh-null");
    CHECKX(GetModuleHandleW(L"nope.dll") == NULL, "proc-gmh-nope-fails");
    CHECKX(GetLastError() == ERROR_PROC_NOT_FOUND, "proc-gmh-nope-code");
    CHECKX(GetModuleHandleW(L"kernel32.dll") != NULL, "proc-gmh-loader");
    {
        HMODULE mod = NULL;
        CHECKX(GetModuleHandleExW(0, NULL, &mod) && mod != NULL,
            "proc-gmhex");
        CHECKX(!GetModuleHandleExW(0xFFF0u, NULL, &mod), "proc-gmhex-bad");
    }

    /* Loader round-trip on a scratch file, through the guest FreeLibrary. */
    wjoin(tmp, L"w32a2_mod.tmp", file);
    DeleteFileW(file);
    h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    {
        HMODULE mod = LoadLibraryW(file);
        CHECKX(mod != NULL, "proc-load");
        if (mod != NULL) {
            n = GetModuleFileNameA(mod, abuf, sizeof(abuf));
            CHECKX(n > 4, "proc-load-name");
            CHECKX(FreeLibrary(mod), "proc-free");
            CHECKX(GetModuleFileNameA(mod, abuf, sizeof(abuf)) == 0,
                "proc-afterfree");
        }
    }
    CHECKX(LoadLibraryW(L"C:\\no\\such.dll") == NULL,
        "proc-load-missing-fails");
    CHECKX(GetLastError() == ERROR_FILE_NOT_FOUND, "proc-load-missing");
    CHECKX(LoadLibraryExW(L"C:\\no\\such.dll", NULL, 0xFFF0u) == NULL,
        "proc-loadex-badflags");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
        "proc-loadex-badflags-code");
    CHECKX(LoadLibraryExA("C:\\no\\such.dll", NULL, 0) == NULL,
        "proc-loadexa-fails");
    CHECKX(GetLastError() == ERROR_FILE_NOT_FOUND, "proc-loadexa-code");
    DeleteFileW(file);

    /* Environment. */
    {
        WCHAR var[32], val[32], bad[16], exp[64], got[64];
        var[0] = L'W'; var[1] = L'3'; var[2] = L'2'; var[3] = L'A';
        var[4] = L'2'; var[5] = L'_'; var[6] = L'V'; var[7] = L'A';
        var[8] = L'R'; var[9] = 0;
        val[0] = L'h'; val[1] = L'e'; val[2] = L'l'; val[3] = L'l';
        val[4] = L'o'; val[5] = 0;
        CHECKX(SetEnvironmentVariableW(var, val), "proc-env-set");
        CHECKX(GetEnvironmentVariableA("W32A2_VAR", abuf, sizeof(abuf))
            == 5, "proc-env-get");
        CHECKX(memcmp(abuf, "hello", 5) == 0, "proc-env-value");
        CHECKX(SetEnvironmentVariableW(var, NULL), "proc-env-del");
        CHECKX(GetEnvironmentVariableA("W32A2_VAR", abuf, sizeof(abuf))
            == 0, "proc-env-gone");
        CHECKX(GetLastError() == ERROR_ENVVAR_NOT_FOUND,
            "proc-env-gone-code");
        bad[0] = L'A'; bad[1] = L'='; bad[2] = L'B'; bad[3] = 0;
        CHECKX(!SetEnvironmentVariableW(bad, val), "proc-env-badname");
        CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
            "proc-env-badname-code");
        /* Expand. */
        var[0] = L'W'; var[1] = L'3'; var[2] = L'2'; var[3] = L'A';
        var[4] = L'2'; var[5] = L'_'; var[6] = L'E'; var[7] = 0;
        val[0] = L'e'; val[1] = L'x'; val[2] = L'p'; val[3] = 0;
        CHECKX(SetEnvironmentVariableW(var, val), "proc-env-exp-set");
        exp[0] = L'%'; exp[1] = L'W'; exp[2] = L'3'; exp[3] = L'2';
        exp[4] = L'A'; exp[5] = L'2'; exp[6] = L'_'; exp[7] = L'E';
        exp[8] = L'%'; exp[9] = L'-'; exp[10] = L' '; exp[11] = L't';
        exp[12] = L'a'; exp[13] = L'i'; exp[14] = L'l'; exp[15] = L' ';
        exp[16] = L'%'; exp[17] = L'%'; exp[18] = L' '; exp[19] = L'%';
        exp[20] = L'N'; exp[21] = L'O'; exp[22] = L'P'; exp[23] = L'E';
        exp[24] = L'%'; exp[25] = 0;
        n = ExpandEnvironmentStringsW(exp, got, 64);
        CHECKX(n > 4, "proc-expand");
        CHECKX(weq(got, "exp- tail % %NOPE%"), "proc-expand-value");
        SetEnvironmentVariableW(var, NULL);
    }
    {
        LPWSTR blk = GetEnvironmentStringsW();
        CHECKX(blk != NULL, "proc-blk");
        if (blk != NULL) {
            CHECKX(blk[0] != 0, "proc-blk-nonempty");
            CHECKX(FreeEnvironmentStringsW(blk), "proc-blk-free");
            CHECKX(!FreeEnvironmentStringsW(blk), "proc-blk-double");
        }
        CHECKX(!FreeEnvironmentStringsW(w), "proc-blk-foreign");
    }

    /* Directories. */
    n = GetCurrentDirectoryW(512, w);
    CHECKX(n > 2 && w[1] == L':', "proc-cwd");
    CHECKX(GetCurrentDirectoryA(sizeof(abuf), abuf) > 2, "proc-cwd-a");
    {
        char back[512];
        GetCurrentDirectoryA(sizeof(back), back);
        CHECKX(SetCurrentDirectoryW(tmp), "proc-cd-tmp");
        CHECKX(GetCurrentDirectoryA(sizeof(abuf), abuf) > 2,
            "proc-cd-verify");
        CHECKX(SetCurrentDirectoryA(back), "proc-cd-back");
        wjoin(tmp, L"w32a2_cdfile.txt", file);
        DeleteFileW(file);
        h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0,
            NULL);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        CHECKX(!SetCurrentDirectoryW(file), "proc-cd-file-fails");
        CHECKX(GetLastError() == ERROR_DIRECTORY, "proc-cd-file-code");
        DeleteFileW(file);
    }
    CHECKX(GetTempPathA(sizeof(abuf), abuf) > 2, "proc-tempa");
    CHECKX(GetWindowsDirectoryA(abuf, sizeof(abuf)) > 2, "proc-windir-a");
    {
        int k = 0;
        while (abuf[k] != 0)
            k++;
        CHECKX(k == 6 && memcmp(abuf, "C:\\win", 6) == 0, "proc-windir-v");
    }
    CHECKX(GetWindowsDirectoryW(w, 512) > 2, "proc-windir-w");
    CHECKX(GetSystemDirectoryA(abuf, sizeof(abuf)) > 2, "proc-sysdir");
    {
        int k = 0;
        while (abuf[k] != 0)
            k++;
        CHECKX(k == 10 && memcmp(abuf, "C:\\win\\sys", 10) == 0,
            "proc-sysdir-v");
    }

    /* Version identity: the documented impersonation. */
    CHECKX(GetVersion() == ((DWORD)19045u << 16 | 0x000Au), "proc-ver");
    {
        OSVERSIONINFOEXW osv;
        DWORD type = 0;
        memset(&osv, 0, sizeof(osv));
        osv.dwOSVersionInfoSize = sizeof(osv);
        CHECKX(GetVersionExW(&osv), "proc-verex");
        CHECKX(osv.dwMajorVersion == 10, "proc-verex-major");
        CHECKX(osv.dwBuildNumber == 19045, "proc-verex-build");
        CHECKX(osv.dwPlatformId == 2, "proc-verex-platform");
        CHECKX(osv.wProductType == 1, "proc-verex-product");
        osv.dwOSVersionInfoSize = 0;
        CHECKX(!GetVersionExW(&osv), "proc-verex-badsize");
        CHECKX(GetProductInfo(10, 0, 0, 0, &type) && type == 0x30u,
            "proc-product");
        CHECKX(!GetProductInfo(6, 1, 0, 0, &type), "proc-product-bad");
    }

    /* Machine. */
    {
        SYSTEM_INFO sys;
        GetSystemInfo(&sys);
        CHECKX(sys.wProcessorArchitecture == 9, "proc-sys-arch");
        CHECKX(sys.dwPageSize == 4096, "proc-sys-page");
        CHECKX(sys.dwNumberOfProcessors >= 1, "proc-sys-cpu");
        CHECKX(sys.dwProcessorType == 8664, "proc-sys-type");
        CHECKX(sys.dwAllocationGranularity == 65536, "proc-sys-gran");
        GetNativeSystemInfo(&sys);
        CHECKX(sys.wProcessorArchitecture == 9, "proc-sys-native");
    }
    CHECKX(!IsDebuggerPresent(), "proc-nodebug");
    CHECKX(IsProcessorFeaturePresent(PF_RDTSC_INSTRUCTION_AVAILABLE),
        "proc-pf-tsc");
    CHECKX(IsProcessorFeaturePresent(PF_MMX_INSTRUCTIONS_AVAILABLE),
        "proc-pf-mmx");
    CHECKX(!IsProcessorFeaturePresent(999), "proc-pf-bad");

    CHECKX(Beep(440, 1), "proc-beep");
    CHECKX(!Beep(10, 100), "proc-beep-bad");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "proc-beep-code");
    CHECKX(SleepEx(1, FALSE) == 0, "proc-sleepex");
    OutputDebugStringW(L"w32a2-proc-hi");

    /* Restart registration round-trips in-process. */
    {
        WCHAR get[64];
        DWORD cch, fl;
        CHECKX(GetApplicationRestartSettings(get, &cch, &fl) ==
            (HRESULT)0x80004005u, "proc-restart-empty");
        CHECKX(RegisterApplicationRestart(L"/restart", 0) == S_OK,
            "proc-restart-reg");
        cch = 64;
        CHECKX(GetApplicationRestartSettings(get, &cch, &fl) == S_OK,
            "proc-restart-get");
        CHECKX(weq(get, "/restart"), "proc-restart-value");
        cch = 2;
        CHECKX(GetApplicationRestartSettings(get, &cch, &fl) ==
            (HRESULT)0x8007017Au, "proc-restart-short");
        CHECKX(UnregisterApplicationRestart() == S_OK,
            "proc-restart-unreg");
    }
    CHECKX(MulDiv(10, 20, 4) == 50, "proc-muldiv");
    CHECKX(MulDiv(1, 1, 0) == -1, "proc-muldiv-zero");
    CHECKX(MulDiv(0x7FFFFFFF, 2, 1) == -1, "proc-muldiv-ovf");
    {
        void *p = (void *)(uintptr_t)0x12345678;
        CHECKX(DecodePointer(EncodePointer(p)) == p, "proc-ptr-round");
        CHECKX(EncodePointer(NULL) == NULL, "proc-ptr-null");
    }
    {
        MEMORYSTATUS ms;
        MEMORYSTATUSEX mx;
        memset(&ms, 0, sizeof(ms));
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatus(&ms);
        CHECKX(ms.dwLength == 40, "proc-mem-len");
        CHECKX(ms.dwTotalPhys == 0, "proc-mem-zero");
        memset(&mx, 0, sizeof(mx));
        mx.dwLength = sizeof(mx);
        CHECKX(!GlobalMemoryStatusEx(&mx), "proc-memex-fails");
        CHECKX(GetLastError() == ERROR_NOT_SUPPORTED,
            "proc-memex-code");
    }

    /* FormatMessage: system, string, ignored. */
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, 2, 0, w, 512,
        NULL);
    CHECKX(n > 5, "proc-msg-sys");
    CHECKX(weq(w, "The named file was not found."), "proc-msg-sys-text");
    CHECKX(FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, 9999, 0, w,
        512, NULL) == 0, "proc-msg-sys-missing");
    {
        WCHAR fmt[64], ins[16];
        DWORD_PTR args[3];
        fmt[0] = L'E'; fmt[1] = L'%'; fmt[2] = L'1'; fmt[3] = L'!';
        fmt[4] = L'd'; fmt[5] = L'!'; fmt[6] = L':'; fmt[7] = L'%';
        fmt[8] = L'2'; fmt[9] = L'!'; fmt[10] = L's'; fmt[11] = L'!';
        fmt[12] = L':'; fmt[13] = L'%'; fmt[14] = L'3'; fmt[15] = L'!';
        fmt[16] = L'x'; fmt[17] = L'!'; fmt[18] = L'%'; fmt[19] = L'%';
        fmt[20] = 0;
        ins[0] = L'E'; ins[1] = L'a'; ins[2] = L'r'; ins[3] = L't';
        ins[4] = L'h'; ins[5] = 0;
        args[0] = (DWORD_PTR)(int32_t)-5;
        args[1] = (DWORD_PTR)ins;
        args[2] = 0xABu;
        n = FormatMessageW(FORMAT_MESSAGE_FROM_STRING |
            FORMAT_MESSAGE_ARGUMENT_ARRAY, fmt, 0, 0, w, 512,
            (va_list *)args);
        CHECKX(n > 5, "proc-msg-array");
        CHECKX(weq(w, "E-5:Earth:ab%"), "proc-msg-array-text");
        n = FormatMessageW(FORMAT_MESSAGE_FROM_STRING |
            FORMAT_MESSAGE_IGNORE_INSERTS, fmt, 0, 0, w, 512,
            (va_list *)args);
        CHECKX(weq(w, "E::%"), "proc-msg-ignore");
    }
    {
        WCHAR fmt[16];
        fmt[0] = L'a'; fmt[1] = L'b'; fmt[2] = L'%'; fmt[3] = L'0';
        fmt[4] = L'c'; fmt[5] = L'd'; fmt[6] = 0;
        FormatMessageW(FORMAT_MESSAGE_FROM_STRING, fmt, 0, 0, w, 512,
            NULL);
        CHECKX(weq(w, "ab"), "proc-msg-zero");
    }
    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, 2, 0, abuf,
        sizeof(abuf), NULL);
    CHECKX(n > 5, "proc-msg-a");
    {
        int k = 0;
        while (abuf[k] != 0)
            k++;
        CHECKX(k == 29 &&
            memcmp(abuf, "The named file was not found.", 29) == 0,
            "proc-msg-a-text");
    }

    w32a2_done("PROC");
}
