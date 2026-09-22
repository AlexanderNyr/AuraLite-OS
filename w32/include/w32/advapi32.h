/* advapi32.h — W32APP_PLAN.md phase W32A-9: the registry and security DLL.
 *
 * Everything in this header is an interface fact about the documented Win32
 * registry and security API (predefined key values, value-type constants,
 * REG_* error codes, CryptoAPI constants, SID and SECURITY_DESCRIPTOR
 * layouts), pinned from published documentation exactly the way the A-1
 * ordinal map and the A-8 control constants were (see w32/PROVENANCE.md):
 * the values below are documented constants, not copied code.  No Microsoft
 * header text, code, or binary is included.
 *
 * Scope (decision D1): exactly the 48 ladder-measured advapi32.dll symbols
 * (w32/stub_map.tsv at the W32A-8 baseline) --
 *   * 37 REAL, implemented in w32/src/advapi32.c: the Reg* set (A and W as
 *     the ledger spells them), GetUserNameA/W, the SID builders, the
 *     security-descriptor builders, IsTextUnicode, the hash-only CryptoAPI
 *     set, and SystemFunction036 (the RtlGenRandom alias).
 *   * 9 FAIL-CLEAN finals, also implemented in advapi32.c with their final
 *     documented refusals: the LSA pair+close, account and privilege
 *     lookup, the token pair, and file security.  The single-user machine
 *     has no LSA database, no account table, no privileges to hold and no
 *     file ACLs; each refusal says so, once, and answers with the
 *     documented code.
 * RegEnumValue/RegFlushKey are not in any ledger; RegFlushKey exists anyway
 * because the hive's fsync policy needs a caller-visible flush point (it is
 * how the fixture observes persistence).  Nothing else beyond the ledger
 * is exported.
 *
 * THE HIVE (format W32HIVE1, ours — plan decision D5: the on-disk format is
 * documented in docs/win32.md and never byte-compatible with anything
 * Microsoft): file-backed at /disk/w32hive when /disk exists, else
 * /tmp/w32hive with the volatility logged at first use.  Key tree, value
 * types REG_SZ/REG_EXPAND_SZ/REG_DWORD/REG_QWORD/REG_BINARY/REG_MULTI_SZ,
 * whole-file writes with a sequence number and CRC32 over the payload, so
 * a torn write fails loud at the next open and is never half-read.
 *
 * The registry model, stated honestly:
 *   * HKEY_CURRENT_USER is the real root; everything an application writes
 *     lands under it (or under the writable HKLM subtree below).
 *   * HKEY_LOCAL_MACHINE is real and read-mostly: only the \Software
 *     subtree is writable (the documented installer surface); writes
 *     elsewhere fail with ERROR_ACCESS_DENIED.
 *   * HKEY_CLASSES_ROOT is the documented merge view -- HKCU\Software\Classes
 *     over HKLM\Software\Classes, HKCU winning reads; writes into HKCR
 *     land in the HKCU half.
 *   * Key and value names compare case-insensitively for ASCII letters and
 *     exactly otherwise (the documented folding scope).
 *   * One writer session at a time: the hive is loaded at first registry
 *     use in a process and written through on every mutation; a second
 *     process started later re-reads the file.  Single-user session model.
 */

#ifndef AURALITE_W32_ADVAPI32_H
#define AURALITE_W32_ADVAPI32_H

#include "w32/w32_abi.h"

/* ---- base scalar types -------------------------------------------------
 * W32_BOOL/W32_DWORD/W32_HANDLE/W32_LONG are spelled identically in
 * w32_abi.h / kernel32.h; restated here (C11-compatible redeclarations)
 * so this header stands alone the way comctl32.h does. */

typedef int32_t  W32_LONG;
typedef uint32_t W32_DWORD;
typedef uint32_t W32_ULONG;
typedef int32_t  W32_BOOL;
typedef uint64_t W32_QWORD;
typedef void    *W32_HKEY;
typedef void    *W32_HANDLE;
typedef uint16_t W32_WCHAR;

/* ---- error codes (documented Win32 error values) --------------------- */
/* The common ones (ERROR_SUCCESS, ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND,
 * ERROR_ACCESS_DENIED, ERROR_INVALID_HANDLE, ERROR_NOT_ENOUGH_MEMORY) are
 * already spelled in w32_errno.h / kernel32.h and are not redefined here. */

#define W32_ERROR_MORE_DATA          234u   /* buffer too small; *lpcbData = needed */
#define W32_ERROR_NO_TOKEN          1008u   /* OpenProcessToken: no token object */
#define W32_ERROR_NOT_ALL_ASSIGNED  1300u   /* AdjustTokenPrivileges: nothing held */
#define W32_ERROR_NONE_MAPPED       1332u   /* LookupAccountName: no account db */
#define W32_ERROR_NO_SUCH_PRIVILEGE 1530u   /* LookupPrivilegeValue: no privilege db */
#define W32_ERROR_FILE_CORRUPT      1392u   /* hive: torn write / bad CRC */
#define W32_ERROR_UNSUPPORTED_TYPE  1630u   /* RegGetValue: wrong value type */
#define W32_ERROR_REGISTRY_IO_FAILED 1017u  /* hive write-through failed */

/* ---- predefined keys -------------------------------------------------- */
/* Documented HKEY_* constants.  They are passed as HKEY values directly
 * (the Windows shape: the constants ARE valid keys, no open required). */

#define W32_HKEY_CLASSES_ROOT   ((W32_HKEY)(uintptr_t)0x80000000u)
#define W32_HKEY_CURRENT_USER   ((W32_HKEY)(uintptr_t)0x80000001u)
#define W32_HKEY_LOCAL_MACHINE  ((W32_HKEY)(uintptr_t)0x80000002u)

/* ---- registry value types --------------------------------------------- */

#define W32_REG_NONE           0u
#define W32_REG_SZ             1u
#define W32_REG_EXPAND_SZ      2u
#define W32_REG_BINARY         3u
#define W32_REG_DWORD          4u
#define W32_REG_DWORD_LITTLE_ENDIAN 4u
#define W32_REG_QWORD          11u
#define W32_REG_QWORD_LITTLE_ENDIAN 11u
#define W32_REG_MULTI_SZ       7u

/* ---- RegCreateKeyEx disposition ---------------------------------------- */

#define W32_REG_CREATED_NEW_KEY      1u
#define W32_REG_OPENED_EXISTING_KEY  2u

/* ---- RegGetValue type masks (documented RRF_RT_* values) -------------- */

#define W32_RRF_RT_REG_SZ       0x00000002u
#define W32_RRF_RT_REG_BINARY   0x00000008u
#define W32_RRF_RT_REG_DWORD    0x00000010u
#define W32_RRF_NOEXPAND        0x10000000u

/* ---- registry API (the ledger's A/W spellings) ------------------------ */

W32_LONG W32ABI RegOpenKeyExA(W32_HKEY key, const char *subkey, W32_ULONG reserved,
                              W32_ULONG sam, W32_HKEY *out);
W32_LONG W32ABI RegOpenKeyExW(W32_HKEY key, const uint16_t *subkey, W32_ULONG reserved,
                              W32_ULONG sam, W32_HKEY *out);
W32_LONG W32ABI RegCreateKeyExA(W32_HKEY key, const char *subkey, W32_ULONG reserved,
                                const uint16_t *cls, W32_DWORD options,
                                W32_ULONG sam, void *security,
                                W32_HKEY *out, W32_DWORD *disposition);
W32_LONG W32ABI RegCreateKeyExW(W32_HKEY key, const uint16_t *subkey, W32_ULONG reserved,
                                const uint16_t *cls, W32_DWORD options,
                                W32_ULONG sam, void *security,
                                W32_HKEY *out, W32_DWORD *disposition);
W32_LONG W32ABI RegCloseKey(W32_HKEY key);
W32_LONG W32ABI RegQueryValueExA(W32_HKEY key, const char *name, W32_ULONG *reserved,
                                 W32_DWORD *type, uint8_t *data, W32_DWORD *len);
W32_LONG W32ABI RegQueryValueExW(W32_HKEY key, const uint16_t *name, W32_ULONG *reserved,
                                 W32_DWORD *type, uint8_t *data, W32_DWORD *len);
W32_LONG W32ABI RegSetValueExA(W32_HKEY key, const char *name, W32_ULONG reserved,
                               W32_DWORD type, const uint8_t *data, W32_DWORD len);
W32_LONG W32ABI RegSetValueExW(W32_HKEY key, const uint16_t *name, W32_ULONG reserved,
                               W32_DWORD type, const uint8_t *data, W32_DWORD len);
W32_LONG W32ABI RegDeleteValueW(W32_HKEY key, const uint16_t *name);
W32_LONG W32ABI RegDeleteKeyA(W32_HKEY key, const char *subkey);
W32_LONG W32ABI RegDeleteKeyW(W32_HKEY key, const uint16_t *subkey);
W32_LONG W32ABI RegDeleteKeyExW(W32_HKEY key, const uint16_t *subkey,
                                W32_ULONG sam, W32_ULONG reserved);
W32_LONG W32ABI RegEnumKeyA(W32_HKEY key, W32_DWORD index,
                            char *name, W32_DWORD cap);
W32_LONG W32ABI RegEnumKeyExW(W32_HKEY key, W32_DWORD index,
                              uint16_t *name, W32_DWORD *cap,
                              W32_ULONG *reserved, uint16_t *cls,
                              W32_DWORD *cls_cap, void *ft_last_write);
W32_LONG W32ABI RegQueryInfoKeyW(W32_HKEY key,
                                 uint16_t *cls, W32_DWORD *cls_cap,
                                 W32_ULONG *reserved,
                                 W32_DWORD *subkeys, W32_DWORD *max_subkey_len,
                                 W32_DWORD *max_class_len,
                                 W32_DWORD *values, W32_DWORD *max_value_name_len,
                                 W32_DWORD *max_value_len,
                                 void *security_desc_len, void *ft_last_write);
W32_LONG W32ABI RegGetValueW(W32_HKEY hkey, const uint16_t *subkey,
                             const uint16_t *value, W32_DWORD flags,
                             W32_DWORD *type, void *data, W32_DWORD *len);
W32_LONG W32ABI RegFlushKey(W32_HKEY key);

/* ---- identity ---------------------------------------------------------- */
/* The single-user machine: one session, one name.  GetUserName answers
 * "user" (documented; there is no account database to ask). */

W32_BOOL W32ABI GetUserNameA(char *buf, W32_ULONG *len);
W32_BOOL W32ABI GetUserNameW(uint16_t *buf, W32_ULONG *len);

/* IsTextUnicode: NOT implemented in advapi32.c.  The engine lives in
 * kernel32_loc.c (the base kernel32 surface); the ledger's apps import
 * it from ADVAPI32.dll, so the bind table forwards A32!IsTextUnicode
 * to it -- the real-world forwarder shape.  The prototype and the
 * W32_IS_TEXT_UNICODE_* flag values (the real winnls.h values) are in
 * kernel32.h.  Note the in-mask contract: *result is the set of tests
 * to run (NULL flags = all tests); 0 runs none. */

/* ---- SIDs --------------------------------------------------------------- */
/* Layout (documented, absolute form): Revision (1 byte, value 1),
 * SubAuthorityCount (1 byte), IdentifierAuthority (6 bytes, big-endian),
 * SubAuthority[count] (4 bytes each, little-endian).  The identifier
 * authority is passed as an 8-byte struct whose first two bytes are
 * padding (the Windows shape); only the low 6 bytes are stored. */

#pragma pack(push, 1)
typedef struct {
    uint8_t pad[2];             /* the documented 8-byte struct shape */
    uint8_t value[6];           /* big-endian authority */
} W32_SID_IDENTIFIER_AUTHORITY;

typedef struct {
    uint8_t  revision;          /* 1 */
    uint8_t  sub_count;
    uint8_t  authority[6];      /* big-endian */
    uint32_t sub[8];            /* only sub_count are meaningful; the
                                 * fixed tail is always allocated so
                                 * indexing is clean (GetLengthSid gates) */
} W32_SID;
#pragma pack(pop)

#define W32_SID_REVISION 1u

W32_BOOL W32ABI AllocateAndInitializeSid(const W32_SID_IDENTIFIER_AUTHORITY *auth,
                                         uint8_t count,
                                         W32_DWORD s0, W32_DWORD s1, W32_DWORD s2,
                                         W32_DWORD s3, W32_DWORD s4, W32_DWORD s5,
                                         W32_DWORD s6, W32_DWORD s7,
                                         void **out_sid);
W32_DWORD W32ABI GetLengthSid(const void *sid);
W32_BOOL  W32ABI CopySid(W32_DWORD len, void *dst, const void *src);
W32_BOOL  W32ABI EqualSid(const void *a, const void *b);
void *    W32ABI FreeSid(void *sid);
W32_BOOL  W32ABI CheckTokenMembership(W32_HANDLE token, const void *sid,
                                      W32_BOOL *is_member);

/* The single-user SID model, documented: the session user is
 * S-1-5-21-0-0-1000 and is a member of BUILTIN\Administrators
 * (S-1-5-32-544).  CheckTokenMembership answers TRUE for those two and
 * FALSE for everything else.  This is the one place the plan sanctions
 * "admin", and the rationale is one paragraph in docs/win32.md, not a
 * pretence of ACLs. */

/* ---- security descriptors (absolute form, the builder set) -------------- */
/* Layout (documented): 1-byte revision, 1-byte pad, 16-bit control word,
 * then Owner/Group/Sacl/Dacl pointers.  Only the builder pair in the
 * ledger is exported: Initialize + SetDacl + SetOwner.  Enforcement is
 * owner-only and lives in the VFS, not here; this is descriptor BUILDING. */

#define W32_SECURITY_DESCRIPTOR_REVISION 1u
#define W32_SE_OWNER_DEFAULTED  0x0001u
#define W32_SE_DACL_PRESENT     0x0004u
#define W32_SE_DACL_DEFAULTED   0x0008u

/* Natural x64 alignment (the layout a mingw-built binary's stack buffer
 * has): revision@0, sbz1@1, control@2 (word), pad to 8, owner@8, group@16,
 * sacl@24, dacl@32; sizeof 40. */
typedef struct {
    uint8_t  revision;
    uint8_t  sbz1;
    uint16_t control;
    uint64_t owner;
    uint64_t group;
    uint64_t sacl;
    uint64_t dacl;
} W32_SECURITY_DESCRIPTOR;

W32_BOOL W32ABI InitializeSecurityDescriptor(void *sd, W32_DWORD revision);
W32_BOOL W32ABI SetSecurityDescriptorDacl(void *sd, W32_BOOL present,
                                          void *dacl, W32_BOOL defaulted);
W32_BOOL W32ABI SetSecurityDescriptorOwner(void *sd, void *owner,
                                           W32_BOOL defaulted);

/* ---- CryptoAPI (hash-only, mapped onto libatls) -------------------------- */
/* Implemented algorithms: CALG_SHA_256/384/512 (incremental via libatls
 * contexts) and SHA3-256/512 (buffered, hashed on query -- documented).
 * The SHA3 CALG values are AuraLite-chosen (0x8025/0x8026) and marked as
 * such: Microsoft never published CALG constants for SHA-3 in the classic
 * CryptoAPI.  CALG_SHA1 and CALG_MD5 refuse with NTE_BAD_ALGID -- SHA-1 is
 * absent from libatls and no receipt shows a core flow needing it (the
 * plan's recorded decision); MD5 is not in any ledger flow. */

typedef void *W32_HCRYPTPROV;
typedef void *W32_HCRYPTHASH;

#define W32_PROV_RSA_FULL      1u
#define W32_CRYPT_VERIFYCONTEXT 0xF0000000u

#define W32_CALG_MD5           0x8003u
#define W32_CALG_SHA1          0x8004u
#define W32_CALG_SHA_256       0x800Cu
#define W32_CALG_SHA_384       0x800Du
#define W32_CALG_SHA_512       0x800Eu
#define W32_CALG_SHA3_256      0x8025u   /* AuraLite-chosen, documented */
#define W32_CALG_SHA3_512      0x8026u   /* AuraLite-chosen, documented */

#define W32_HP_ALGID           1u
#define W32_HP_HASHVAL         2u
#define W32_HP_HASHSIZE        4u

#define W32_NTE_BAD_ALGID      0x80090008u

W32_BOOL W32ABI CryptAcquireContextW(W32_HCRYPTPROV *out, const uint16_t *container,
                                     const uint16_t *provider, W32_DWORD type,
                                     W32_ULONG flags);
W32_BOOL W32ABI CryptReleaseContext(W32_HCRYPTPROV prov, W32_ULONG flags);
W32_BOOL W32ABI CryptCreateHash(W32_HCRYPTPROV prov, W32_ULONG alg,
                                void *key, W32_ULONG flags, W32_HCRYPTHASH *out);
W32_BOOL W32ABI CryptHashData(W32_HCRYPTHASH hash,
                              const uint8_t *data, W32_DWORD len, W32_ULONG flags);
W32_BOOL W32ABI CryptGetHashParam(W32_HCRYPTHASH hash, W32_DWORD param,
                                  uint8_t *data, W32_DWORD *len, W32_ULONG flags);
W32_BOOL W32ABI CryptDestroyHash(W32_HCRYPTHASH hash);

/* ---- RtlGenRandom (the 7z.dll single advapi32 import) --------------------- */
/* SystemFunction036 = RtlGenRandom, documented alias: fills the buffer from
 * the kernel CSPRNG (getrandom).  BOOLEAN return, TRUE on complete fill. */

W32_BOOL W32ABI SystemFunction036(void *buf, W32_ULONG len);

/* ---- the FAIL-CLEAN finals (documented refusals, one reason each) --------- */
/* Single-user machine: no LSA database, no account table, no privileges to
 * hold, no file ACLs.  Each function logs its reason once and answers with
 * the documented code, so callers degrade the way they would on Windows
 * when the object does not exist. */

/* NTSTATUS values (documented constants, hex as published). */
#define W32_STATUS_ACCESS_DENIED   ((W32_LONG)(int32_t)0xC0000022u)
#define W32_STATUS_INVALID_HANDLE  ((W32_LONG)(int32_t)0xC0000008u)

W32_LONG W32ABI LsaOpenPolicy(void *system_name, void *attr,
                              W32_ULONG access, void **handle);
W32_LONG W32ABI LsaAddAccountRights(void *handle, void *sid,
                                    void *user_rights, W32_ULONG count);
W32_LONG W32ABI LsaClose(void *handle);
W32_BOOL W32ABI LookupAccountNameW(const uint16_t *system_name,
                                   const uint16_t *account,
                                   void *sid, W32_DWORD *sid_len,
                                   void *domain, W32_DWORD *domain_len,
                                   W32_DWORD *name_use);
W32_BOOL W32ABI LookupPrivilegeValueW(const uint16_t *system_name,
                                      const uint16_t *name, void *luid);
W32_BOOL W32ABI OpenProcessToken(W32_HANDLE process, W32_ULONG access,
                                 W32_HANDLE *token);
W32_BOOL W32ABI AdjustTokenPrivileges(W32_HANDLE token, W32_BOOL disable_all,
                                      void *new_state, W32_ULONG buf_len,
                                      void *prev_state, W32_ULONG *ret_len);
W32_BOOL W32ABI GetFileSecurityW(const uint16_t *path, W32_ULONG info,
                                 void *sd, W32_DWORD len, W32_DWORD *needed);
W32_BOOL W32ABI SetFileSecurityW(const uint16_t *path, W32_ULONG info,
                                 void *sd);

/* ---- engine knobs (test seams; production code never touches them) -------- */
/* Hive location override: when non-NULL the hive lives at this exact path
 * (host unit tests point it at a scratch directory; the guest never sets
 * it).  Volatility selection (/disk vs /tmp) is skipped while set. */
extern const char *w32_advapi_hive_override;

/* Host-test reset: drops the loaded tree, the handle/hash tables and any
 * corrupt-hive latch, so each test section starts from the file's truth.
 * Guest code never calls it. */
void w32_advapi_reset_for_host_test(void);

#endif /* AURALITE_W32_ADVAPI32_H */
