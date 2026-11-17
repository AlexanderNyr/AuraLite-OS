#![no_std]
#![no_main]

extern crate rsbr;

use core::cmp::{max, min};
use core::str;

use rsbr::fs::{self, DirEntry, File};
use rsbr::gui::{self, Event, Window};
use rsbr::gui::{
    BLACK, GREEN, GRAY, WHITE, YELLOW,
    EVT_CLOSE_REQ, EVT_KEY_DOWN, EVT_RESIZE,
    FLAG_HAS_CLOSE, FLAG_HAS_TITLE, FLAG_MOVABLE, FLAG_RESIZABLE,
    KEY_DELETE, KEY_DOWN, KEY_END, KEY_HOME, KEY_LEFT, KEY_RIGHT, KEY_UP,
};
use rsbr::{Error, Result};
use rsbr::time;
use rsbr::syscall as syscalls;

const WINDOW_X: i32 = 140;
const WINDOW_Y: i32 = 90;
const WINDOW_W: u32 = 680;
const WINDOW_H: u32 = 340;
const WINDOW_TITLE: &str = "Terminal";

const PAD_X: i32 = 12;
const PAD_Y: i32 = 10;
const LINE_H: i32 = 16;
const CHAR_W: i32 = 8;

const COLS: usize = 82;
const VISIBLE_ROWS: usize = 20;
const HIST_SIZE: usize = 96;
const CMD_HIST_SIZE: usize = 32;
const INPUT_CAP: usize = 160;
const DIR_CAP: usize = 96;
const IO_CHUNK: usize = 256;
const MAX_ARGS: usize = 9;

const BG_COLOR: u32 = BLACK;
const OUTPUT_COLOR: u32 = GREEN;
const COMMAND_COLOR: u32 = YELLOW;
const SYSTEM_COLOR: u32 = GRAY;

const F_GETFL: usize = 3;
const F_SETFL: usize = 4;
const O_NONBLOCK: usize = 0x800;
const WNOHANG: usize = 1;

#[derive(Clone, Copy)]
struct TermLine {
    bytes: [u8; COLS],
    len: usize,
}

impl TermLine {
    const EMPTY: Self = Self {
        bytes: [0; COLS],
        len: 0,
    };

    fn as_str(&self) -> &str {
        // Scrollback accepts printable ASCII only, so the byte range is UTF-8.
        unsafe { str::from_utf8_unchecked(&self.bytes[..self.len]) }
    }
}

#[derive(Clone, Copy)]
struct CommandLine {
    bytes: [u8; INPUT_CAP],
    len: usize,
}

impl CommandLine {
    const EMPTY: Self = Self {
        bytes: [0; INPUT_CAP],
        len: 0,
    };

    fn as_str(&self) -> &str {
        unsafe { str::from_utf8_unchecked(&self.bytes[..self.len]) }
    }
}

struct TextBuf<const N: usize> {
    bytes: [u8; N],
    len: usize,
}

impl<const N: usize> TextBuf<N> {
    const fn new() -> Self {
        Self {
            bytes: [0; N],
            len: 0,
        }
    }

    fn push_byte(&mut self, byte: u8) {
        if self.len < N {
            self.bytes[self.len] = byte;
            self.len += 1;
        }
    }

    fn push_bytes(&mut self, bytes: &[u8]) {
        let count = min(N.saturating_sub(self.len), bytes.len());
        self.bytes[self.len..self.len + count].copy_from_slice(&bytes[..count]);
        self.len += count;
    }

    fn push_str(&mut self, text: &str) {
        self.push_bytes(text.as_bytes());
    }

    fn push_u32(&mut self, mut value: u32) {
        let mut digits = [0u8; 10];
        let mut count = 0;
        if value == 0 {
            self.push_byte(b'0');
            return;
        }
        while value != 0 && count < digits.len() {
            digits[count] = b'0' + (value % 10) as u8;
            count += 1;
            value /= 10;
        }
        while count != 0 {
            count -= 1;
            self.push_byte(digits[count]);
        }
    }

    fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }

    fn as_str(&self) -> &str {
        unsafe { str::from_utf8_unchecked(self.as_bytes()) }
    }
}

fn write_c_string(text: &str, output: &mut [u8]) -> Result<()> {
    if text.as_bytes().contains(&0) || text.len() + 1 > output.len() {
        return Err(Error::Syscall(22));
    }

    output[..text.len()].copy_from_slice(text.as_bytes());
    output[text.len()] = 0;

    Ok(())
}

fn sys_chdir(path: &str) -> Result<()> {
    let mut buffer = [0u8; 256];
    write_c_string(path, &mut buffer)?;

    syscalls::call1(
        syscalls::SYS_CHDIR,
        buffer.as_ptr() as usize,
    )?;

    Ok(())
}

fn sys_getcwd(output: &mut [u8]) -> Result<()> {
    syscalls::call2(
        syscalls::SYS_GETCWD,
        output.as_mut_ptr() as usize,
        output.len(),
    )?;

    Ok(())
}

fn sys_pipe(fds: &mut [i32; 2]) -> Result<()> {
    syscalls::call1(
        syscalls::SYS_PIPE,
        fds.as_mut_ptr() as usize,
    )?;

    Ok(())
}

fn sys_close(fd: i32) -> Result<()> {
    syscalls::call1(syscalls::SYS_CLOSE, fd as usize)?;
    Ok(())
}

fn sys_dup2(old_fd: i32, new_fd: i32) -> Result<()> {
    syscalls::call2(
        syscalls::SYS_DUP2,
        old_fd as usize,
        new_fd as usize,
    )?;

    Ok(())
}

fn sys_fcntl(fd: i32, command: usize, argument: usize) -> Result<usize> {
    syscalls::call3(
        syscalls::SYS_FCNTL,
        fd as usize,
        command,
        argument,
    )
}

fn sys_fork() -> Result<i32> {
    let pid = syscalls::call0(syscalls::SYS_FORK)?;

    if pid > i32::MAX as usize {
        return Err(Error::InvalidSyscallResult);
    }

    Ok(pid as i32)
}

fn sys_waitpid(pid: i32, status: &mut i32, options: usize) -> Result<i32> {
    let result = syscalls::call4(
        syscalls::SYS_WAIT4,
        pid as usize,
        status as *mut i32 as usize,
        options,
        0,
    )?;

    if result > i32::MAX as usize {
        return Err(Error::InvalidSyscallResult);
    }

    Ok(result as i32)
}

struct TerminalApp {
    window: Window,
    scrollback: [TermLine; HIST_SIZE],
    scroll_total: u64,

    command_history: [CommandLine; CMD_HIST_SIZE],
    command_total: u64,
    history_cursor: usize,
    saved_live_line: CommandLine,

    input: [u8; INPUT_CAP],
    input_len: usize,
    cursor_pos: usize,

    cols: usize,
    visible_rows: usize,
    want_exit: bool,
    prompt_visible: bool,
}

impl TerminalApp {
    fn new() -> Result<Self> {
        let flags = FLAG_HAS_TITLE | FLAG_HAS_CLOSE | FLAG_RESIZABLE | FLAG_MOVABLE;
        let mut app = Self {
            window: Window::create(WINDOW_X, WINDOW_Y, WINDOW_W, WINDOW_H, Some(WINDOW_TITLE), flags)?,
            scrollback: [TermLine::EMPTY; HIST_SIZE],
            scroll_total: 0,
            command_history: [CommandLine::EMPTY; CMD_HIST_SIZE],
            command_total: 0,
            history_cursor: 0,
            saved_live_line: CommandLine::EMPTY,
            input: [0; INPUT_CAP],
            input_len: 0,
            cursor_pos: 0,
            cols: COLS,
            visible_rows: VISIBLE_ROWS,
            want_exit: false,
            prompt_visible: true,
        };
        app.window.show()?;
        app.term_push("AuraLite GUI Terminal");
        app.term_push("Type 'help' to list commands.");
        app.repaint()?;
        Ok(app)
    }

    fn push_raw(&mut self, bytes: &[u8]) {
        let slot = (self.scroll_total as usize) % HIST_SIZE;
        let count = min(bytes.len(), COLS);
        let mut line = TermLine::EMPTY;
        line.bytes[..count].copy_from_slice(&bytes[..count]);
        line.len = count;
        self.scrollback[slot] = line;
        self.scroll_total = self.scroll_total.wrapping_add(1);
    }

    // Each call is a logical text fragment. Newlines split lines; fragments
    // longer than the current terminal width are hard-wrapped.
    fn term_bytes(&mut self, bytes: &[u8]) {
        let mut line = [0u8; COLS];
        let mut used = 0usize;
        let width = max(1, min(self.cols, COLS));

        for &source in bytes {
            if source == b'\n' {
                self.push_raw(&line[..used]);
                used = 0;
                continue;
            }

            let byte = if source == b'\r' || source == b'\t' {
                b' '
            } else if source < 0x20 || source == 0x7f || source >= 0x80 {
                b' '
            } else {
                source
            };

            line[used] = byte;
            used += 1;
            if used == width {
                self.push_raw(&line[..used]);
                used = 0;
            }
        }

        if used != 0 {
            self.push_raw(&line[..used]);
        }
    }

    fn term_push(&mut self, text: &str) {
        self.term_bytes(text.as_bytes());
    }

    fn input_rows(&self) -> usize {
        if !self.prompt_visible {
            return 0;
        }
        let total = 2 + self.input_len + 1; // prompt plus room for the caret cell
        max(1, (total + self.cols - 1) / self.cols)
    }

    fn repaint(&mut self) -> Result<()> {
        self.window.frame_begin()?;
        self.window.clear(BG_COLOR)?;

        let input_rows = self.input_rows();
        let rows_available = self.visible_rows.saturating_sub(input_rows);
        let stored = min(self.scroll_total as usize, HIST_SIZE);
        let shown = min(stored, rows_available);
        let first_seq = self.scroll_total.saturating_sub(shown as u64);

        for row in 0..shown {
            let seq = first_seq + row as u64;
            let line = &self.scrollback[(seq as usize) % HIST_SIZE];
            let color = if line.len > 0 && line.bytes[0] == b'$' {
                COMMAND_COLOR
            } else if line.len > 0 && line.bytes[0] == b'[' {
                SYSTEM_COLOR
            } else {
                OUTPUT_COLOR
            };
            self.window.draw_text(
                PAD_X,
                PAD_Y + row as i32 * LINE_H,
                line.as_str(),
                color,
            )?;
        }

        if self.prompt_visible {
            let prompt_row = shown;
            let prompt_y = PAD_Y + prompt_row as i32 * LINE_H;
            self.window.draw_text(PAD_X, prompt_y, "$ ", GREEN)?;

            let first_capacity = self.cols.saturating_sub(2);
            let first_len = min(self.input_len, first_capacity);
            if first_len != 0 {
                let first = unsafe { str::from_utf8_unchecked(&self.input[..first_len]) };
                self.window.draw_text(PAD_X + 2 * CHAR_W, prompt_y, first, WHITE)?;
            }

            let mut consumed = first_len;
            let mut input_row = 1usize;
            while consumed < self.input_len && input_row < input_rows {
                let count = min(self.cols, self.input_len - consumed);
                let text = unsafe { str::from_utf8_unchecked(&self.input[consumed..consumed + count]) };
                self.window.draw_text(
                    PAD_X,
                    prompt_y + input_row as i32 * LINE_H,
                    text,
                    WHITE,
                )?;
                consumed += count;
                input_row += 1;
            }

            let cursor_absolute = 2 + self.cursor_pos;
            let cursor_row = cursor_absolute / self.cols;
            let cursor_col = cursor_absolute % self.cols;
            let cursor_x = PAD_X + cursor_col as i32 * CHAR_W;
            let cursor_y = prompt_y + cursor_row as i32 * LINE_H;
            self.window.fill_rect(cursor_x, cursor_y, CHAR_W as u32, (LINE_H - 1) as u32, WHITE)?;
            if self.cursor_pos < self.input_len {
                let glyph = unsafe {
                    str::from_utf8_unchecked(&self.input[self.cursor_pos..self.cursor_pos + 1])
                };
                self.window.draw_text(cursor_x, cursor_y, glyph, BLACK)?;
            }
        }

        self.window.frame_end()?;
        self.window.invalidate()?;
        Ok(())
    }

    fn resize_layout(&mut self) {
        if let Ok((width, height)) = self.window.size() {
            let drawable_cols = (width as i32 - PAD_X * 2).max(CHAR_W) / CHAR_W;
            let drawable_rows = (height as i32 - PAD_Y * 2).max(LINE_H) / LINE_H;
            self.cols = max(1, min(COLS, drawable_cols as usize));
            self.visible_rows = max(1, min(VISIBLE_ROWS, drawable_rows as usize));
        }
    }

    fn copy_input(&self) -> CommandLine {
        let mut line = CommandLine::EMPTY;
        line.bytes[..self.input_len].copy_from_slice(&self.input[..self.input_len]);
        line.len = self.input_len;
        line
    }

    fn set_input(&mut self, line: &CommandLine) {
        self.input = [0; INPUT_CAP];
        self.input[..line.len].copy_from_slice(&line.bytes[..line.len]);
        self.input_len = line.len;
        self.cursor_pos = line.len;
    }

    fn store_command(&mut self, line: &CommandLine) {
        if line.len == 0 {
            return;
        }
        if self.command_total != 0 {
            let previous = &self.command_history[(self.command_total as usize - 1) % CMD_HIST_SIZE];
            if previous.len == line.len && previous.bytes[..previous.len] == line.bytes[..line.len] {
                return;
            }
        }
        self.command_history[(self.command_total as usize) % CMD_HIST_SIZE] = *line;
        self.command_total = self.command_total.wrapping_add(1);
    }

    fn recall_command(&mut self, older: bool) {
        let count = min(self.command_total as usize, CMD_HIST_SIZE);
        if count == 0 {
            return;
        }

        if older {
            if self.history_cursor == 0 {
                self.saved_live_line = self.copy_input();
                self.history_cursor = 1;
            } else if self.history_cursor < count {
                self.history_cursor += 1;
            }
        } else if self.history_cursor > 1 {
            self.history_cursor -= 1;
        } else if self.history_cursor == 1 {
            self.history_cursor = 0;
            let saved = self.saved_live_line;
            self.set_input(&saved);
            return;
        } else {
            return;
        }

        if self.history_cursor != 0 {
            let seq = self.command_total - self.history_cursor as u64;
            let entry = self.command_history[(seq as usize) % CMD_HIST_SIZE];
            self.set_input(&entry);
        }
    }

    fn handle_key(&mut self, key: u32) -> Result<()> {
        match key {
            0x08 | 0x7f => {
                if self.cursor_pos > 0 {
                    self.input.copy_within(self.cursor_pos..self.input_len, self.cursor_pos - 1);
                    self.input_len -= 1;
                    self.cursor_pos -= 1;
                    self.input[self.input_len] = 0;
                }
            }
            KEY_DELETE => {
                if self.cursor_pos < self.input_len {
                    self.input.copy_within(self.cursor_pos + 1..self.input_len, self.cursor_pos);
                    self.input_len -= 1;
                    self.input[self.input_len] = 0;
                }
            }
            KEY_LEFT => self.cursor_pos = self.cursor_pos.saturating_sub(1),
            KEY_RIGHT => self.cursor_pos = min(self.cursor_pos + 1, self.input_len),
            KEY_HOME => self.cursor_pos = 0,
            KEY_END => self.cursor_pos = self.input_len,
            KEY_UP => self.recall_command(true),
            KEY_DOWN => self.recall_command(false),
            10 | 13 => self.submit_input()?,
            0x20..=0x7e => {
                if self.input_len < INPUT_CAP - 1 {
                    self.input.copy_within(self.cursor_pos..self.input_len, self.cursor_pos + 1);
                    self.input[self.cursor_pos] = key as u8;
                    self.input_len += 1;
                    self.cursor_pos += 1;
                }
            }
            _ => {}
        }
        Ok(())
    }

    fn submit_input(&mut self) -> Result<()> {
        let command = self.copy_input();
        let mut start = 0usize;
        while start < command.len && command.bytes[start] == b' ' {
            start += 1;
        }
        if start == command.len {
            self.input = [0; INPUT_CAP];
            self.input_len = 0;
            self.cursor_pos = 0;
            self.history_cursor = 0;
            return Ok(());
        }

        let mut echo = TextBuf::<{ COLS + 4 }>::new();
        echo.push_str("$ ");
        echo.push_bytes(&command.bytes[start..command.len]);
        self.push_raw(echo.as_bytes());
        self.store_command(&command);

        self.input = [0; INPUT_CAP];
        self.input_len = 0;
        self.cursor_pos = 0;
        self.history_cursor = 0;

        self.run_command(&command.bytes[start..command.len])?;
        self.repaint()?;
        Ok(())
    }

    fn run_command(&mut self, line: &[u8]) -> Result<()> {
        let mut command = [0u8; INPUT_CAP];
        let copy_len = min(line.len(), INPUT_CAP - 1);
        command[..copy_len].copy_from_slice(&line[..copy_len]);
        command[copy_len] = 0;

        let mut starts = [0usize; MAX_ARGS];
        let mut ends = [0usize; MAX_ARGS];
        let mut argc = 0usize;
        let mut pos = 0usize;
        while pos < copy_len && argc < MAX_ARGS {
            while pos < copy_len && command[pos] == b' ' {
                pos += 1;
            }
            if pos >= copy_len {
                break;
            }
            starts[argc] = pos;
            while pos < copy_len && command[pos] != b' ' {
                pos += 1;
            }
            ends[argc] = pos;
            if pos < copy_len {
                command[pos] = 0;
                pos += 1;
            }
            argc += 1;
        }
        if argc == 0 {
            return Ok(());
        }

        let cmd = unsafe { str::from_utf8_unchecked(&command[starts[0]..ends[0]]) };
        let arg = if argc > 1 {
            unsafe { str::from_utf8_unchecked(&command[starts[1]..ends[1]]) }
        } else {
            ""
        };

        match cmd {
            "help" => {
                self.term_push("builtins: help clear exit pwd cd ls cat mkdir rm echo uname history");
                self.term_push("external programs: weather, sysinfo, snake, ...");
            }
            "clear" => {
                self.scroll_total = 0;
                self.scrollback = [TermLine::EMPTY; HIST_SIZE];
            }
            "exit" => self.want_exit = true,
            "uname" => {
                #[cfg(target_arch = "x86_64")]
                self.term_push("AuraLite OS x86_64");
                #[cfg(not(target_arch = "x86_64"))]
                self.term_push("AuraLite OS");
            }
            "echo" => {
                let rest_start = ends[0];
                let mut from = rest_start;
                if from < copy_len && line.get(from) == Some(&b' ') {
                    from += 1;
                }
                self.term_bytes(&line[from..]);
            }
            "pwd" => self.builtin_pwd(),
            "cd" => {
                let path = if arg.is_empty() { "/" } else { arg };
                if sys_chdir(path).is_err() {
                    self.term_push("cd: no such directory");
                }
            }
            "history" => self.builtin_history(),
            "ls" => self.builtin_ls(arg),
            "cat" => self.builtin_cat(arg),
            "mkdir" => {
                if arg.is_empty() || fs::create_dir(arg, 0o755).is_err() {
                    self.term_push("mkdir: failed");
                }
            }
            "rm" => {
                if arg.is_empty() {
                    self.term_push("rm: missing operand");
                } else if fs::remove_file(arg).is_err() && fs::remove_dir(arg).is_err() {
                    self.term_push("rm: failed");
                }
            }
            _ => self.run_external_command(cmd, &command, &starts, &ends, argc)?,
        }
        Ok(())
    }

    fn getcwd_text<'a>(&mut self, output: &'a mut [u8]) -> Option<&'a str> {
        if sys_getcwd(output).is_err() {
            return None;
        }
        let length = output.iter().position(|&b| b == 0).unwrap_or(output.len());
        str::from_utf8(&output[..length]).ok()
    }

    fn builtin_pwd(&mut self) {
        let mut path = [0u8; 256];
        match self.getcwd_text(&mut path) {
            Some(value) => self.term_push(value),
            None => self.term_push("?"),
        }
    }

    fn builtin_history(&mut self) {
        let count = min(self.command_total as usize, CMD_HIST_SIZE);
        if count == 0 {
            self.term_push("(no history yet)");
            return;
        }
        let first = self.command_total - count as u64;
        for seq in first..self.command_total {
            let entry = self.command_history[(seq as usize) % CMD_HIST_SIZE];
            let mut line = TextBuf::<{ COLS + 32 }>::new();
            line.push_str("  ");
            line.push_u32((seq + 1) as u32);
            line.push_str("  ");
            line.push_bytes(&entry.bytes[..entry.len]);
            self.term_bytes(line.as_bytes());
        }
    }

    fn make_path(&mut self, arg: &str, output: &mut TextBuf<256>) -> bool {
        if arg.is_empty() {
            let mut cwd = [0u8; 256];
            let Some(current) = self.getcwd_text(&mut cwd) else {
                return false;
            };
            output.push_str(current);
            return true;
        }
        if arg.starts_with('/') {
            output.push_str(arg);
            return output.len < 256;
        }

        let mut cwd = [0u8; 256];
        let Some(current) = self.getcwd_text(&mut cwd) else {
            return false;
        };
        if current == "/" {
            output.push_byte(b'/');
        } else {
            output.push_str(current);
            output.push_byte(b'/');
        }
        output.push_str(arg);
        output.len < 256
    }

    fn builtin_ls(&mut self, arg: &str) {
        let mut path = TextBuf::<256>::new();
        if !self.make_path(arg, &mut path) {
            self.term_push("ls: getcwd failed");
            return;
        }
        let path_str = path.as_str();
        let mut entries: [DirEntry; DIR_CAP] = core::array::from_fn(|_| DirEntry::empty());
        let count = match fs::read_dir(path_str, &mut entries) {
            Ok(value) => min(value, DIR_CAP),
            Err(_) => {
                self.term_push("ls: cannot read directory");
                return;
            }
        };

        if count == 0 {
            self.term_push("(empty directory)");
            return;
        }

        let mut line = TextBuf::<COLS>::new();
        for entry in entries.iter().take(count) {
            let Some(name) = entry.name_str() else { continue };
            let suffix = if entry.is_dir() { "/" } else { "" };
            let required = name.len() + suffix.len() + 2;
            if line.len != 0 && line.len + required > self.cols {
                self.term_bytes(line.as_bytes());
                line = TextBuf::new();
            }
            line.push_str(name);
            line.push_str(suffix);
            line.push_str("  ");
        }
        if line.len != 0 {
            self.term_bytes(line.as_bytes());
        }
    }

    fn builtin_cat(&mut self, arg: &str) {
        if arg.is_empty() {
            self.term_push("cat: missing file");
            return;
        }
        let mut file = match File::open(arg) {
            Ok(value) => value,
            Err(_) => {
                self.term_push("cat: cannot open file");
                return;
            }
        };
        let mut total = 0usize;
        loop {
            let mut chunk = [0u8; IO_CHUNK];
            match file.read(&mut chunk) {
                Ok(0) => break,
                Ok(count) => {
                    total = total.saturating_add(count);
                    self.term_bytes(&chunk[..count]);
                }
                Err(_) => {
                    self.term_push("cat: read failed");
                    break;
                }
            }
        }
        if total == 0 {
            self.term_push("(empty file)");
        }
    }

    fn resolve_program(&mut self, command: &str, out: &mut TextBuf<256>) -> bool {
        // Keep the resolver allocation-free. Commands containing '/' are
        // treated as explicit paths, matching prog_resolve's documented rule.
        if command.as_bytes().contains(&b'/') {
            out.push_str(command);
            return File::open(out.as_str()).is_ok();
        }

        // These are the standard AuraLite user program locations. If the
        // libc prog_resolve implementation is linked into this target, it can
        // replace this small fallback without changing the command runner.
        const SEARCH_DIRS: [&str; 6] = ["/bin", "/apps", "/demos", "/tests", "/opt", "/"];
        for directory in SEARCH_DIRS {
            out.len = 0;
            if directory != "/" {
                out.push_str(directory);
                out.push_byte(b'/');
            } else {
                out.push_byte(b'/');
            }
            out.push_str(command);
            if out.len < 256 && File::open(out.as_str()).is_ok() {
                return true;
            }
        }
        false
    }

    fn run_external_command(
        &mut self,
        command_name: &str,
        command: &[u8; INPUT_CAP],
        starts: &[usize; MAX_ARGS],
        ends: &[usize; MAX_ARGS],
        argc: usize,
    ) -> Result<()> {
        let mut resolved = TextBuf::<256>::new();
        if !self.resolve_program(command_name, &mut resolved) {
            let mut message = TextBuf::<COLS>::new();
            message.push_str(command_name);
            message.push_str(": command not found (try help)");
            self.term_bytes(message.as_bytes());
            return Ok(());
        }

        let mut args = [""; MAX_ARGS];
        args[0] = resolved.as_str();
        for index in 1..argc {
            args[index] = unsafe { str::from_utf8_unchecked(&command[starts[index]..ends[index]]) };
        }

        self.run_external(resolved.as_str(), &args[..argc])
    }
    fn execute_child(
        path: &str,
        argv: &[&str],
        read_fd: i32,
        write_fd: i32,
    ) -> ! {
        let _ = sys_close(read_fd);

        if sys_dup2(write_fd, 1).is_err()
            || sys_dup2(write_fd, 2).is_err()
        {
            Self::child_fail(b"[!] dup2 failed\n");
        }

        let _ = sys_close(write_fd);

        let mut path_buffer = [0u8; 256];

        if write_c_string(path, &mut path_buffer).is_err() {
            Self::child_fail(b"[!] invalid executable path\n");
        }

        if argv.len() > MAX_ARGS {
            Self::child_fail(b"[!] too many arguments\n");
        }

        let mut argument_storage = [[0u8; INPUT_CAP]; MAX_ARGS];
        let mut argument_pointers = [0usize; MAX_ARGS + 1];

        argument_pointers[0] = path_buffer.as_ptr() as usize;

        for index in 1..argv.len() {
            if write_c_string(
                argv[index],
                &mut argument_storage[index],
            ).is_err() {
                Self::child_fail(b"[!] invalid argument\n");
            }

            argument_pointers[index] =
                argument_storage[index].as_ptr() as usize;
        }

        let environment: [usize; 1] = [0];

        let _ = syscalls::call3(
            syscalls::SYS_EXECVE,
            path_buffer.as_ptr() as usize,
            argument_pointers.as_ptr() as usize,
            environment.as_ptr() as usize,
        );

        Self::child_fail(b"[!] execve failed\n");
    }

    fn child_fail(message: &[u8]) -> ! {
        let _ = syscalls::call3(
            syscalls::SYS_WRITE,
            2,
            message.as_ptr() as usize,
            message.len(),
        );

        let _ = syscalls::call1(
            syscalls::SYS_EXIT,
            127,
        );

        loop {
            core::hint::spin_loop();
        }
    }

    fn run_external(&mut self, path: &str, argv: &[&str]) -> Result<()> {
        let mut pipe_fds = [-1i32; 2];

        if sys_pipe(&mut pipe_fds).is_err() {
            self.term_push("[!] pipe() failed");
            return Ok(());
        }

        let flags = match sys_fcntl(pipe_fds[0], F_GETFL, 0) {
            Ok(value) => value,
            Err(_) => {
                let _ = sys_close(pipe_fds[0]);
                let _ = sys_close(pipe_fds[1]);
                self.term_push("[!] fcntl(F_GETFL) failed");
                return Ok(());
            }
        };

        if sys_fcntl(
            pipe_fds[0],
            F_SETFL,
            flags | O_NONBLOCK,
        ).is_err() {
            let _ = sys_close(pipe_fds[0]);
            let _ = sys_close(pipe_fds[1]);
            self.term_push("[!] fcntl(F_SETFL) failed");
            return Ok(());
        }

        let pid = match sys_fork() {
            Ok(value) => value,
            Err(_) => {
                let _ = sys_close(pipe_fds[0]);
                let _ = sys_close(pipe_fds[1]);
                self.term_push("[!] fork() failed");
                return Ok(());
            }
        };

        if pid == 0 {
            Self::execute_child(
                path,
                argv,
                pipe_fds[0],
                pipe_fds[1],
            );
        }

        let _ = sys_close(pipe_fds[1]);

        let mut running = TextBuf::<128>::new();
        running.push_str("[running ");
        running.push_str(path);
        running.push_str(" (pid ");
        running.push_u32(pid as u32);
        running.push_str(")]");

        self.term_bytes(running.as_bytes());

        self.prompt_visible = false;
        self.repaint()?;

        let mut child_status = 0i32;
        let mut child_reaped = false;
        let mut pipe_eof = false;
        let mut read_error = false;
        let mut wait_error = false;

        while !pipe_eof || !child_reaped {
            if !pipe_eof {
                let mut chunk = [0u8; IO_CHUNK];

                match sysbr_read(pipe_fds[0] as usize, &mut chunk) {
                    Ok(0) => {
                        pipe_eof = true;
                    }

                    Ok(count) => {
                        self.term_bytes(&chunk[..count]);
                        self.repaint()?;
                    }

                    Err(error) if is_would_block(&error) => {}

                    Err(_) => {
                        pipe_eof = true;
                        read_error = true;
                    }
                }
            }

            if !child_reaped {
                match sys_waitpid(pid, &mut child_status, WNOHANG) {
                    Ok(value) if value == pid => {
                        child_reaped = true;
                    }

                    Ok(_) => {}

                    Err(error) if is_interrupted(&error) => {}

                    Err(_) => {
                        child_reaped = true;
                        wait_error = true;
                    }
                }
            }

            self.poll_during_external()?;

            if !pipe_eof || !child_reaped {
                time::sleep_ms(8)?;
            }
        }

        let _ = sys_close(pipe_fds[0]);

        self.prompt_visible = true;

        if read_error {
            self.term_push("[!] pipe read failed");
        }

        if wait_error {
            self.term_push("[!] waitpid failed");
        }

        if child_status != 0 {
            let mut tail = TextBuf::<64>::new();
            tail.push_str("[exit status ");
            tail.push_u32(child_status as u32);
            tail.push_byte(b']');
            self.term_bytes(tail.as_bytes());
        }

        Ok(())
    }

    fn poll_during_external(&mut self) -> Result<()> {
        while let Some(event) = self.window.poll()? {
            match event.kind {
                EVT_CLOSE_REQ => self.want_exit = true,
                EVT_RESIZE => self.resize_layout(),
                _ => {}
            }
        }
        Ok(())
    }

    fn run(&mut self) -> Result<()> {
        while !self.want_exit {
            match self.window.poll()? {
                Some(event) => {
                    self.handle_event(event)?;
                    if !self.want_exit {
                        self.repaint()?;
                    }
                }
                None => time::sleep_ms(8)?,
            }
        }
        Ok(())
    }

    fn handle_event(&mut self, event: Event) -> Result<()> {
        match event.kind {
            EVT_CLOSE_REQ => self.want_exit = true,
            EVT_RESIZE => self.resize_layout(),
            EVT_KEY_DOWN => self.handle_key(event.key)?,
            _ => {}
        }
        Ok(())
    }
}

fn is_would_block(error: &Error) -> bool {
    matches!(*error, Error::Syscall(11))
}

fn is_interrupted(error: &Error) -> bool {
    matches!(*error, Error::Syscall(4))
}

fn sysbr_read(fd: usize, buffer: &mut [u8]) -> Result<usize> {
    rsbr::io::read(fd, buffer)
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    match TerminalApp::new() {
        Ok(mut app) => match app.run() {
            Ok(()) => 0,
            Err(e) => {
                rsbr::eprintln!("gterm run error: {}", e);
                1
            }
        },
        Err(e) => {
            rsbr::eprintln!("gterm init error: {}", e);
            1
        }
    }
}
