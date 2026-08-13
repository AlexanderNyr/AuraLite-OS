#!/usr/bin/env python3
"""mk_pe_badwndproc.py — WIN32_PLAN.md W32-5 hostile fixture.

Copies the windowed test .exe and rewrites the WNDPROC address the program
stores into its WNDCLASSEXA, pointing it far outside the image.

The plan's gate asks for exactly this: "Hostile: a WNDPROC pointer outside the
image ... refused, no kernel fault".  What must happen is that dispatching to
it kills only this process -- and that the compositor reaps the window it had
already created, which is the `gui_cleanup_process` requirement in the same
gate.

Implementation: the program builds its class with `lea rax,[wndproc]` followed
by `mov [wc+8], rax`.  Rather than parse instructions, this finds the
RIP-relative LEA by its opcode (48 8D 05 disp32) and rewrites the displacement
so the computed address lands in unmapped space.  If the pattern is not found
the script fails loudly instead of silently producing a non-hostile file.
"""
import sys, struct

def main():
    if len(sys.argv) != 3:
        print("usage: mk_pe_badwndproc.py <in.exe> <out.exe>", file=sys.stderr)
        return 2
    data = bytearray(open(sys.argv[1], "rb").read())

    # 48 8D 05 <disp32>  =  lea rax, [rip+disp32]
    pat = bytes([0x48, 0x8D, 0x05])
    idx = data.find(pat)
    if idx < 0:
        print("mk_pe_badwndproc: LEA pattern not found; fixture would not be "
              "hostile, refusing to write it", file=sys.stderr)
        return 1

    old = struct.unpack_from("<i", data, idx + 3)[0]
    # A large positive displacement puts the target well past the image.
    struct.pack_into("<i", data, idx + 3, 0x30000000)
    open(sys.argv[2], "wb").write(data)
    print(f"  [pe] {sys.argv[2]} (WNDPROC lea disp {old} -> 0x30000000, "
          f"points outside the image)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
