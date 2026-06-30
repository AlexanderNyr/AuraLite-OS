#!/usr/bin/env python3

"""AuraLite OS kernel GDB pretty-printers."""

import gdb
import gdb.printing

THREAD_STATES = ['READY', 'RUNNING', 'BLOCKED', 'DEAD']

class TcbPrinter:
    """Pretty-printer for tcb_t."""
    def __init__(self, val):
        self.val = val

    def to_string(self):
        state_idx = int(self.val['state'])
        state = THREAD_STATES[state_idx] if 0 <= state_idx < 4 else f'?({state_idx})'
        name = self.val['name'].string()
        pid = int(self.val['id'])
        pml4 = int(self.val['pml4_phys'])
        sig_pending = int(self.val['sig_pending'])
        cwd = self.val['cwd'].string()
        return (f"TCB(pid={pid}, name='{name}', state={state}, "
                f"pml4={pml4:#x}, sig_pending={sig_pending:#x}, cwd='{cwd}')")

class OfdPrinter:
    """Pretty-printer for struct ofd."""
    def __init__(self, val):
        self.val = val

    def to_string(self):
        pos = int(self.val['pos'])
        flags = int(self.val['flags'])
        refcount = int(self.val['refcount'])
        return f"OFD(pos={pos}, flags={flags:#x}, refcount={refcount})"

class VnodePrinter:
    """Pretty-printer for struct vnode."""
    TYPES = {1:'FILE', 2:'DIR', 3:'CHARDEV', 4:'SYMLINK', 5:'FIFO'}
    def __init__(self, val):
        self.val = val

    def to_string(self):
        t = int(self.val['type'])
        name = self.val['name'].string()
        size = int(self.val['size'])
        return f"Vnode(type={self.TYPES.get(t, '?')}, name='{name}', size={size})"

class WaitQueuePrinter:
    """Pretty-printer for struct wait_queue."""
    def __init__(self, val):
        self.val = val

    def to_string(self):
        head = self.val['head']
        count = 0
        ptr = head
        while ptr != 0:
            count += 1
            ptr = ptr['next']
            if count > 100: break
        return f"WaitQueue(waiters={count})"

def build_pp():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("auralite")
    pp.add_printer('tcb_t',        r'^tcb_t$',          TcbPrinter)
    pp.add_printer('ofd',          r'^(struct )?ofd$',   OfdPrinter)
    pp.add_printer('vnode',        r'^(struct )?vnode$', VnodePrinter)
    pp.add_printer('wait_queue',   r'^(struct )?wait_queue$', WaitQueuePrinter)
    return pp

gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pp())

# --- Custom GDB Commands ---

class ShowThreadsCommand(gdb.Command):
    """Print all registered kernel threads. Usage: show-threads"""
    def __init__(self):
        super().__init__("show-threads", gdb.COMMAND_USER)

    def invoke(self, args, from_tty):
        try:
            arr = gdb.parse_and_eval("all_threads")
            max_t = int(gdb.parse_and_eval("all_threads_count"))
            for i in range(max_t):
                p = arr[i]
                if p != 0:
                    print(f"[{i}] {p.dereference()}")
        except gdb.error as e:
            print(f"Error: {e}")

class ShowRunQueueCommand(gdb.Command):
    """Print the current CPU's run queues. Usage: show-runqueue"""
    def __init__(self):
        super().__init__("show-runqueue", gdb.COMMAND_USER)

    def invoke(self, args, from_tty):
        try:
            # Check all CPUs
            for cpu_id in range(8):
                try:
                    cl = gdb.parse_and_eval(f"cpu_locals[{cpu_id}]")
                    head = cl['rq_head']
                    qlen = int(cl['rq_len'])
                    print(f"CPU[{cpu_id}] rq_len={qlen}:")
                    cur = head
                    while cur != 0:
                        print(f"  {cur.dereference()}")
                        cur = cur['next']
                except: break
        except gdb.error as e:
            print(f"Error: {e}")

class ShowCurrentCommand(gdb.Command):
    """Print the currently running thread. Usage: show-current"""
    def __init__(self):
        super().__init__("show-current", gdb.COMMAND_USER)

    def invoke(self, args, from_tty):
        try:
            cur = gdb.parse_and_eval("sched_current()")
            if cur != 0: print(cur.dereference())
            else: print("No current thread")
        except gdb.error as e:
            print(f"Error: {e}")

ShowThreadsCommand()
ShowRunQueueCommand()
ShowCurrentCommand()

print("[auralite] GDB pretty-printers loaded. Commands: show-threads, show-runqueue, show-current")
