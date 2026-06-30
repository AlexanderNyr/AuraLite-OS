# AuraLite OS GDB init script
# Usage: gdb -x tools/gdb_scripts/kernel.gdb

file build/kernel.elf
target remote :1234

# Load pretty-printers
python exec(open('tools/gdb_scripts/kernel.py').read())

# Useful breakpoints (commented out - enable as needed)
# hbreak kmain
# hbreak signal_deliver
# hbreak do_fork
# hbreak paging_handle_cow_fault

# Useful TUI layouts
# layout src
# layout asm

echo [auralite] GDB connected. Run 'show-threads', 'show-runqueue', 'show-current'.\n
