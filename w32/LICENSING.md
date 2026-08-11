# w32 — licensing rules for contributors

**Read this before writing a line of code under `w32/`.**

This subsystem re-implements a published API. That is lawful, and there is a
large body of practice and case law behind it — but only if it is done a
particular way. The rules below are what keep it that way. They are not
suggestions and they are not about being cautious for its own sake: the asset
being protected is *provenance*, and provenance cannot be repaired after the
fact.

`WIN32_PLAN.md` §1 explains the reasoning in full. This file is the short,
operative version.

---

## The one-paragraph version

Write your own implementation, from public documentation and from the
permissively licensed declarations already vendored here. Do not read, copy
from, or consult any implementation of the Windows API that is not licence-
compatible with this repository — including Wine and ReactOS, which are the two
you will be tempted by.

---

## Allowed sources

| Source | Why it is fine |
|---|---|
| `w32/include/` (vendored mingw-w64 headers) | Public domain / ZPL-2.1 / BSD-3-Clause; see `PROVENANCE.md` |
| Published Microsoft documentation (learn.microsoft.com) | Readable for facts. Describe behaviour in your own words; do not paste sample code |
| The PE/COFF specification | A published format specification |
| Observable behaviour of a binary you lawfully possess | Black-box testing is the classic clean-room method |
| Your own head | The point |

## Forbidden sources

| Source | Why |
|---|---|
| **Wine** | LGPL-2.1-or-later — incompatible with this Apache-2.0 tree |
| **ReactOS** | GPL-2.0 / LGPL-2.1 — incompatible |
| **Microsoft SDK / DDK headers** | Proprietary EULA. Not one line, not "just this struct" |
| **Leaked Windows source** | Stolen. No licence exists to rely on |
| **Disassembly or decompilation of Windows binaries** | Creates a derivation argument you cannot disprove |
| **AI output you cannot attribute** | If a tool reproduces Wine's implementation you have the same problem, with worse records |

The Wine and ReactOS entries say *read*, not merely *copy*. A contributor who
has studied Wine's `user32` cannot easily demonstrate that their
`CreateWindowExA` is independent. That is why the rule is drawn wider than
copyright strictly requires.

---

## What we ship, and do not ship

- AuraLite ships **no Microsoft binary**: no `kernel32.dll`, no `user32.dll`,
  no `msvcrt.dll`, no redistributables. Ever.
- The user supplies the `.exe` they want to run. `w32` supplies the
  implementation behind its imports.
- Test binaries in this repository are built from source with **mingw-w64**,
  never downloaded.

---

## Naming and trademarks

"Windows", "Win32", "Microsoft" and "MSVC" are trademarks of Microsoft
Corporation. We use them **nominatively** — to state truthfully what is being
interoperated with — and never:

- in a product, subsystem or directory name (the subsystem is `w32`);
- in a logo, icon, splash screen or theme;
- in any phrasing that suggests endorsement, affiliation or certification.

Say "a Win32-compatible personality", not "Windows for AuraLite". Do not claim
"Windows compatibility" in the README, in `docs/`, or in a commit message.

---

## Adding a vendored file

1. It must be licensed public domain, ZPL-2.1, BSD-3-Clause or MIT.
2. Keep its licence header verbatim. Do not reformat or "tidy" it.
3. Add a row to `PROVENANCE.md` recording upstream project, version, path and
   licence.
4. `tools/check_provenance.sh` enforces 1 and 3, and fails the build otherwise.

## Contributing implementation code

By contributing to `w32/` you affirm, in addition to the usual CLA
(`docs/CLA_INDIVIDUAL.md`):

> I have not consulted Wine, ReactOS, Microsoft SDK headers, leaked Windows
> source, or any disassembly of a Microsoft binary while producing this
> contribution.

If you cannot make that statement about a change, say so in the pull request.
A change that has to be rewritten by someone else is a minor cost; a file with
an unclear history is a permanent one.

---

*This file is a working rule set, not legal advice. Anyone shipping AuraLite
commercially should have counsel review `WIN32_PLAN.md` §1.*
