#![no_std]
#![no_main]

extern crate rsbr;

use core::str;

use rsbr::fs::{read_dir, DirEntry};
use rsbr::gui::{
    self, App, Event, Flow, Window,
    EVT_CLOSE_REQ, EVT_KEY_DOWN,
    FLAG_HAS_CLOSE, FLAG_HAS_TITLE,
};
use rsbr::process;

const WINDOW_X: i32 = 72;
const WINDOW_Y: i32 = 54;
const WINDOW_WIDTH: i32 = 1120;
const WINDOW_HEIGHT: i32 = 760;

const SIDEBAR_WIDTH: i32 = 192;
const HEADER_HEIGHT: i32 = 88;
const GRID_ORIGIN_Y: i32 = 108;
const CARD_WIDTH: i32 = 108;
const CARD_HEIGHT: i32 = 106;
const CARD_GAP: i32 = 10;

// The portable AArch64/RV64 directory ABI returns at most 32 entries per call.
// Keeping the same bound also makes this application architecture-independent.
const MAX_ITEMS: usize = 32;
const MAX_NAME_BYTES: usize = 60; // 59 bytes plus room for a local NUL terminator.
const MAX_PATH_BYTES: usize = 256; // RSBR C-string wrappers accept 255 bytes + NUL.

// The RSBR process documentation uses this path in its gedit launch example.
const GEDIT_PATH: &str = "/bin/gedit";

const COLOR_BACKGROUND: u32 = 0x00F5F7FA;
const COLOR_SIDEBAR: u32 = 0x00EDF1F6;
const COLOR_HEADER: u32 = 0x00FFFFFF;
const COLOR_CARD: u32 = 0x00FFFFFF;
const COLOR_CARD_HOVER: u32 = 0x00EAF2FF;
const COLOR_CARD_BORDER: u32 = 0x00D7DEE8;
const COLOR_DIVIDER: u32 = 0x00D7DEE8;
const COLOR_TEXT: u32 = 0x00212B3A;
const COLOR_MUTED: u32 = 0x006B7788;
const COLOR_ACCENT: u32 = 0x003D78D8;
const COLOR_ACCENT_SOFT: u32 = 0x00DCEAFF;
const COLOR_FOLDER: u32 = 0x00E9B84C;
const COLOR_FOLDER_DARK: u32 = 0x00C89127;
const COLOR_FILE: u32 = 0x00FFFFFF;
const COLOR_FILE_LINE: u32 = 0x0096A2B2;

const ROOTS: [(&str, &str); 6] = [
    ("/", "/"),
    ("bin/", "/bin"),
    ("lib/", "/lib"),
    ("rv64/", "/rv64"),
    ("apps/", "/apps"),
    ("demos/", "/demos"),
];

#[derive(Clone, Copy)]
struct PathBuffer {
    bytes: [u8; MAX_PATH_BYTES],
    len: usize,
}

impl PathBuffer {
    const fn empty() -> Self {
        Self {
            bytes: [0; MAX_PATH_BYTES],
            len: 0,
        }
    }

    fn set(&mut self, path: &str) -> bool {
        let bytes = path.as_bytes();
        if bytes.is_empty() || bytes[0] != b'/' || bytes.len() >= MAX_PATH_BYTES {
            return false;
        }

        self.bytes = [0; MAX_PATH_BYTES];
        self.bytes[..bytes.len()].copy_from_slice(bytes);
        self.len = bytes.len();
        true
    }

    fn as_str(&self) -> &str {
        // Every value is copied from a validated UTF-8 path or assembled from
        // validated directory-entry names.
        str::from_utf8(&self.bytes[..self.len]).unwrap_or("/")
    }

    fn push_name(&mut self, name: &str) -> bool {
        if name.is_empty() || name.as_bytes().contains(&b'/') {
            return false;
        }

        let needs_slash = self.len > 1 && self.bytes[self.len - 1] != b'/';
        let slash_len = if needs_slash { 1 } else { 0 };
        let required = self.len + slash_len + name.len();
        if required >= MAX_PATH_BYTES {
            return false;
        }

        let mut at = self.len;
        if needs_slash {
            self.bytes[at] = b'/';
            at += 1;
        }
        self.bytes[at..at + name.len()].copy_from_slice(name.as_bytes());
        self.len = required;
        self.bytes[self.len] = 0;
        true
    }

    fn parent(&mut self) -> bool {
        if self.len <= 1 {
            return false;
        }

        let mut cursor = self.len;
        while cursor > 1 && self.bytes[cursor - 1] != b'/' {
            cursor -= 1;
        }

        if cursor <= 1 {
            self.len = 1;
        } else {
            self.len = cursor - 1;
        }
        self.bytes[self.len] = 0;
        true
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ItemKind {
    Directory,
    File,
    Other,
}

#[derive(Clone, Copy)]
struct FileItem {
    name: [u8; MAX_NAME_BYTES],
    name_len: usize,
    kind: ItemKind,
}

impl FileItem {
    const fn empty() -> Self {
        Self {
            name: [0; MAX_NAME_BYTES],
            name_len: 0,
            kind: ItemKind::Other,
        }
    }

    fn name_str(&self) -> &str {
        str::from_utf8(&self.name[..self.name_len]).unwrap_or("<invalid name>")
    }
}

struct FileManagerApp {
    current_path: PathBuffer,
    items: [FileItem; MAX_ITEMS],
    item_count: usize,
    hovered_item: Option<usize>,
    status: &'static str,
}

impl FileManagerApp {
    fn new() -> Self {
        let mut app = Self {
            current_path: PathBuffer::empty(),
            items: [FileItem::empty(); MAX_ITEMS],
            item_count: 0,
            hovered_item: None,
            status: "Ready",
        };

        let _ = app.load_directory("/");
        app
    }

    fn load_directory(&mut self, path: &str) -> bool {
        let mut raw_entries = [DirEntry::empty(); MAX_ITEMS];
        let count = match read_dir(path, &mut raw_entries) {
            Ok(count) => count,
            Err(_) => {
                self.status = "Unable to read directory";
                return false;
            }
        };

        let mut next_items = [FileItem::empty(); MAX_ITEMS];
        let mut next_count = 0usize;
        let limit = if count < MAX_ITEMS { count } else { MAX_ITEMS };

        for entry in raw_entries.iter().take(limit) {
            let name = match entry.name_str() {
                Some(name) => name,
                None => continue,
            };

            if name.is_empty() || name == "." || name == ".." {
                continue;
            }

            let name_bytes = name.as_bytes();
            if name_bytes.len() >= MAX_NAME_BYTES {
                continue;
            }

            let kind = if entry.is_dir() {
                ItemKind::Directory
            } else if entry.is_file() {
                ItemKind::File
            } else {
                ItemKind::Other
            };

            let item = &mut next_items[next_count];
            item.name[..name_bytes.len()].copy_from_slice(name_bytes);
            item.name[name_bytes.len()] = 0;
            item.name_len = name_bytes.len();
            item.kind = kind;
            next_count += 1;
        }

        if !self.current_path.set(path) {
            self.status = "Path is too long";
            return false;
        }

        self.items = next_items;
        self.item_count = next_count;
        self.hovered_item = None;
        self.status = "Directory loaded";
        true
    }

    fn item_path(&self, index: usize) -> Option<PathBuffer> {
        if index >= self.item_count {
            return None;
        }

        let mut path = self.current_path;
        if !path.push_name(self.items[index].name_str()) {
            return None;
        }
        Some(path)
    }

    fn open_item(&mut self, index: usize) {
        if index >= self.item_count {
            return;
        }

        let item = self.items[index];
        let path = match self.item_path(index) {
            Some(path) => path,
            None => {
                self.status = "Selected path is too long";
                return;
            }
        };

        match item.kind {
            ItemKind::Directory => {
                let full_path = path.as_str();
                let _ = self.load_directory(full_path);
            }
            ItemKind::File => {
                if item.name_str().ends_with(".txt") {
                    let full_path = path.as_str();
                    match process::spawn(GEDIT_PATH, &[full_path]) {
                        Ok(_) => self.status = "Opened in gedit",
                        Err(_) => self.status = "Failed to start gedit",
                    }
                } else {
                    self.status = "Only .txt files are opened with gedit";
                }
            }
            ItemKind::Other => {
                self.status = "Unsupported filesystem entry";
            }
        }
    }

    fn open_parent(&mut self) {
        let mut parent = self.current_path;
        if parent.parent() {
            let _ = self.load_directory(parent.as_str());
        }
    }

    fn is_root_active(&self, root_path: &str) -> bool {
        let current = self.current_path.as_str();
        if root_path == "/" {
            return current == "/";
        }
        current == root_path
            || (current.starts_with(root_path)
                && current.as_bytes().get(root_path.len()) == Some(&b'/'))
    }

    fn grid_columns(&self, client_width: i32) -> usize {
        let available = (client_width - SIDEBAR_WIDTH - 32).max(CARD_WIDTH);
        ((available + CARD_GAP) / (CARD_WIDTH + CARD_GAP)).max(1) as usize
    }

    fn item_at(&self, x: i32, y: i32, client_width: i32) -> Option<usize> {
        if x < SIDEBAR_WIDTH + 12 || y < GRID_ORIGIN_Y {
            return None;
        }

        let columns = self.grid_columns(client_width);
        let relative_x = x - (SIDEBAR_WIDTH + 20);
        let relative_y = y - GRID_ORIGIN_Y;
        if relative_x < 0 || relative_y < 0 {
            return None;
        }

        let stride_x = CARD_WIDTH + CARD_GAP;
        let stride_y = CARD_HEIGHT + CARD_GAP;
        let column = (relative_x / stride_x) as usize;
        let row = (relative_y / stride_y) as usize;
        if column >= columns || relative_x % stride_x >= CARD_WIDTH {
            return None;
        }
        if relative_y % stride_y >= CARD_HEIGHT {
            return None;
        }

        let index = row * columns + column;
        if index < self.item_count { Some(index) } else { None }
    }

    // This is the input-independent part of pointer handling. The event-to-
    // coordinates/button adapter is intentionally left as a TODO below until
    // the exact Event mouse ABI is confirmed from gui/abi.rs.
    #[allow(dead_code)]
    fn pointer_move(&mut self, x: i32, y: i32, client_width: i32) -> Flow {
        let next = self.item_at(x, y, client_width);
        if next != self.hovered_item {
            self.hovered_item = next;
            return Flow::Changed;
        }
        Flow::Ignore
    }

    #[allow(dead_code)]
    fn pointer_click(&mut self, x: i32, y: i32, client_width: i32) -> Flow {
        if x < SIDEBAR_WIDTH {
            let root_y = 106;
            let row_height = 40;
            for (index, (_, path)) in ROOTS.iter().enumerate() {
                let top = root_y + index as i32 * row_height;
                if y >= top && y < top + row_height {
                    let _ = self.load_directory(path);
                    return Flow::Changed;
                }
            }
        }

        // Toolbar buttons: Parent and Refresh.
        if y >= 48 && y < 82 {
            if x >= SIDEBAR_WIDTH + 16 && x < SIDEBAR_WIDTH + 100 {
                self.open_parent();
                return Flow::Changed;
            }
            if x >= SIDEBAR_WIDTH + 108 && x < SIDEBAR_WIDTH + 196 {
                let current = self.current_path;
                let _ = self.load_directory(current.as_str());
                return Flow::Changed;
            }
        }

        if let Some(index) = self.item_at(x, y, client_width) {
            self.open_item(index);
            return Flow::Changed;
        }

        Flow::Ignore
    }
}

impl App for FileManagerApp {
    fn update(&mut self, ev: &Event) -> Flow {
        match ev.kind {
            EVT_CLOSE_REQ => Flow::Quit,
            EVT_KEY_DOWN if ev.key == 0x1B => Flow::Quit,
            _ => {
                // TODO(RSBR-GUI-INPUT): connect mouse events to pointer_move()
                // and pointer_click(). The supplied RSBR documentation names
                // Event.kind and Event.key, but does not document mouse event
                // constants, client-relative coordinates, button state, or
                // click-count fields. Do not guess these ABI values here.
                Flow::Ignore
            }
        }
    }

    fn render(&self, w: &Window) -> rsbr::Result<()> {
        let (client_width, client_height) = w.size();
        let width = client_width as i32;
        let height = client_height as i32;

        w.clear(COLOR_BACKGROUND)?;

        // Sidebar and its separator.
        w.fill_rect(0, 0, SIDEBAR_WIDTH as u32, height as u32, COLOR_SIDEBAR)?;
        w.fill_rect(SIDEBAR_WIDTH, 0, 1, height as u32, COLOR_DIVIDER)?;
        w.draw_text(22, 28, "LOCATIONS", COLOR_MUTED)?;

        for (index, (label, path)) in ROOTS.iter().enumerate() {
            let top = 106 + index as i32 * 40;
            if self.is_root_active(path) {
                w.fill_rect(10, top, (SIDEBAR_WIDTH - 20) as u32, 34, COLOR_ACCENT_SOFT)?;
                w.fill_rect(10, top + 5, 3, 24, COLOR_ACCENT)?;
            }
            let text_color = if self.is_root_active(path) { COLOR_ACCENT } else { COLOR_TEXT };
            w.draw_text(28, top + 11, label, text_color)?;
        }

        // Main header.
        w.fill_rect(SIDEBAR_WIDTH, 0, (width - SIDEBAR_WIDTH).max(0) as u32, HEADER_HEIGHT as u32, COLOR_HEADER)?;
        w.fill_rect(SIDEBAR_WIDTH, HEADER_HEIGHT, (width - SIDEBAR_WIDTH).max(0) as u32, 1, COLOR_DIVIDER)?;
        w.draw_text(SIDEBAR_WIDTH + 20, 18, "FILES", COLOR_TEXT)?;

        // Toolbar controls. Their hit-testing is implemented in pointer_click().
        w.fill_rect(SIDEBAR_WIDTH + 16, 48, 84, 28, COLOR_SIDEBAR)?;
        w.draw_rect(SIDEBAR_WIDTH + 16, 48, 84, 28, COLOR_CARD_BORDER)?;
        w.draw_text(SIDEBAR_WIDTH + 30, 57, "Parent", COLOR_TEXT)?;

        w.fill_rect(SIDEBAR_WIDTH + 108, 48, 88, 28, COLOR_SIDEBAR)?;
        w.draw_rect(SIDEBAR_WIDTH + 108, 48, 88, 28, COLOR_CARD_BORDER)?;
        w.draw_text(SIDEBAR_WIDTH + 122, 57, "Refresh", COLOR_TEXT)?;

        w.draw_text(SIDEBAR_WIDTH + 214, 57, self.current_path.as_str(), COLOR_MUTED)?;

        // Main file grid.
        let columns = self.grid_columns(width);
        for index in 0..self.item_count {
            let column = index % columns;
            let row = index / columns;
            let x = SIDEBAR_WIDTH + 20 + column as i32 * (CARD_WIDTH + CARD_GAP);
            let y = GRID_ORIGIN_Y + row as i32 * (CARD_HEIGHT + CARD_GAP);

            if y + CARD_HEIGHT > height - 36 {
                // TODO(RSBR-GUI-SCROLL): add wheel/scroll support when the
                // input ABI exposes a documented wheel event and delta.
                break;
            }

            let hovered = self.hovered_item == Some(index);
            let card_color = if hovered { COLOR_CARD_HOVER } else { COLOR_CARD };
            w.fill_rect(x, y, CARD_WIDTH as u32, CARD_HEIGHT as u32, card_color)?;
            w.draw_rect(x, y, CARD_WIDTH as u32, CARD_HEIGHT as u32, COLOR_CARD_BORDER)?;

            match self.items[index].kind {
                ItemKind::Directory => draw_folder_icon(w, x + 31, y + 13)?,
                ItemKind::File | ItemKind::Other => draw_file_icon(w, x + 37, y + 11)?,
            }

            let name = self.items[index].name_str();
            let label = short_label(name);
            let label_width = (label.len() as i32 * 8).min(CARD_WIDTH - 8);
            let label_x = x + ((CARD_WIDTH - label_width) / 2).max(4);
            let label_color = if hovered { COLOR_ACCENT } else { COLOR_TEXT };
            w.draw_text(label_x, y + 78, label, label_color)?;
        }

        // Bottom status bar.
        let footer_y = (height - 30).max(HEADER_HEIGHT);
        w.fill_rect(SIDEBAR_WIDTH, footer_y, (width - SIDEBAR_WIDTH).max(0) as u32, 1, COLOR_DIVIDER)?;
        w.draw_text(SIDEBAR_WIDTH + 20, footer_y + 9, self.status, COLOR_MUTED)?;
        w.draw_text(
            (width - 130).max(SIDEBAR_WIDTH + 20),
            footer_y + 9,
            "32-entry limit",
            COLOR_MUTED,
        )?;

        Ok(())
    }
}

fn short_label(name: &str) -> &str {
    const MAX_LABEL_BYTES: usize = 12;
    if name.len() <= MAX_LABEL_BYTES {
        return name;
    }

    let mut end = MAX_LABEL_BYTES - 3;
    while end > 0 && !name.is_char_boundary(end) {
        end -= 1;
    }
    &name[..end]
}

fn draw_folder_icon(w: &Window, x: i32, y: i32) -> rsbr::Result<()> {
    w.fill_rect(x + 3, y + 4, 21, 9, COLOR_FOLDER_DARK)?;
    w.fill_rect(x, y + 10, 46, 29, COLOR_FOLDER)?;
    w.draw_rect(x, y + 10, 46, 29, COLOR_FOLDER_DARK)?;
    w.draw_line(x + 3, y + 14, x + 42, y + 14, 0x00F7D98C)?;
    Ok(())
}

fn draw_file_icon(w: &Window, x: i32, y: i32) -> rsbr::Result<()> {
    w.fill_rect(x + 2, y + 1, 34, 42, COLOR_FILE)?;
    w.draw_rect(x + 2, y + 1, 34, 42, COLOR_CARD_BORDER)?;
    w.draw_line(x + 9, y + 14, x + 29, y + 14, COLOR_FILE_LINE)?;
    w.draw_line(x + 9, y + 21, x + 29, y + 21, COLOR_FILE_LINE)?;
    w.draw_line(x + 9, y + 28, x + 25, y + 28, COLOR_FILE_LINE)?;
    Ok(())
}

fn main() -> i32 {
    let window = match Window::create(
        WINDOW_X,
        WINDOW_Y,
        WINDOW_WIDTH as u32,
        WINDOW_HEIGHT as u32,
        Some("Files"),
        FLAG_HAS_TITLE | FLAG_HAS_CLOSE,
    ) {
        Ok(window) => window,
        Err(_) => return 1,
    };

    if window.show().is_err() {
        return 1;
    }

    let app = FileManagerApp::new();
    match gui::run(app, window) {
        Ok(()) => 0,
        Err(_) => 1,
    }
}
