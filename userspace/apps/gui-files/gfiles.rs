#![no_std]
#![no_main]

extern crate rsbr;

use core::str;

use rsbr::fs::{read_dir, DirEntry};
use rsbr::gui::{
    self, App, Cursor, Event, Flow, Window,
    EVT_CLOSE_REQ, EVT_CONTEXT_MENU, EVT_KEY_DOWN, EVT_MOUSE_DOWN,
    EVT_MOUSE_DBLCLICK, EVT_MOUSE_MOVE, EVT_MOUSE_WHEEL, EVT_RESIZE,
    FLAG_HAS_CLOSE, FLAG_HAS_TITLE, FLAG_HAS_MINMAX, FLAG_RESIZABLE,
    KEY_DOWN, KEY_END, KEY_HOME, KEY_LEFT, KEY_RIGHT, KEY_UP,
};
use rsbr::process;

const WINDOW_X: i32 = 72;
const WINDOW_Y: i32 = 54;
const WINDOW_WIDTH: i32 = 1120;
const WINDOW_HEIGHT: i32 = 760;

const SIDEBAR_WIDTH: i32 = 192;
const HEADER_HEIGHT: i32 = 88;
const GRID_ORIGIN_Y: i32 = 108;
const FOOTER_HEIGHT: i32 = 30;
const CARD_WIDTH: i32 = 108;
const CARD_HEIGHT: i32 = 106;
const CARD_GAP: i32 = 10;

const TOOLBAR_X: i32 = SIDEBAR_WIDTH + 16;
const TOOLBAR_Y: i32 = 48;
const TOOLBAR_H: i32 = 28;

const BTN_PARENT: Rect = Rect::new(TOOLBAR_X, TOOLBAR_Y, 84, TOOLBAR_H);
const BTN_REFRESH: Rect = Rect::new(TOOLBAR_X + 84, TOOLBAR_Y, 88, TOOLBAR_H);

// The portable AArch64/RV64 directory ABI returns at most 32 entries per call.
// Keeping the same bound also makes this application architecture-independent.
const MAX_ITEMS: usize = 32;
const MAX_NAME_BYTES: usize = 60; // 59 bytes plus room for a local NUL terminator.
const MAX_PATH_BYTES: usize = 256; // RSBR C-string wrappers accept 255 bytes + NUL.

const GEDIT_PATH: &str = "/bin/gedit";

const COLOR_BACKGROUND: u32 = 0x00F5F7FA;
const COLOR_SIDEBAR: u32 = 0x00EDF1F6;
const COLOR_HEADER: u32 = 0x00FFFFFF;
const COLOR_CARD: u32 = 0x00FFFFFF;
const COLOR_CARD_HOVER: u32 = 0x00EAF2FF;
const COLOR_CARD_SELECTED: u32 = 0x00DCEAFF;
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

#[derive(Clone, Copy)]
struct Rect {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
}

impl Rect {
    const fn new(x: i32, y: i32, w: i32, h: i32) -> Self {
        Self { x, y, w, h }
    }

    const fn contains(&self, px: i32, py: i32) -> bool {
        px >= self.x && px < self.x + self.w && py >= self.y && py < self.y + self.h
    }
}


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
        // Paths are copied from validated UTF-8 strings or assembled from
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

#[derive(Clone, Copy, PartialEq, Eq)]
enum HoverTarget {
    None,
    Root(usize),
    Parent,
    Refresh,
    Item(usize),
}

struct FileManagerApp {
    current_path: PathBuffer,
    items: [FileItem; MAX_ITEMS],
    item_count: usize,
    hovered_target: HoverTarget,
    selected_item: Option<usize>,
    scroll_row: usize,
    client_width: i32,
    client_height: i32,
    hand_cursor: bool,
    ignore_next_double_click: bool,
    status: &'static str,
}

impl FileManagerApp {
    fn new() -> Self {
        let mut app = Self {
            current_path: PathBuffer::empty(),
            items: [FileItem::empty(); MAX_ITEMS],
            item_count: 0,
            hovered_target: HoverTarget::None,
            selected_item: None,
            scroll_row: 0,
            client_width: WINDOW_WIDTH,
            client_height: WINDOW_HEIGHT,
            hand_cursor: false,
            ignore_next_double_click: false,
            status: "Ready",
        };

        let _ = app.load_directory("/");
        app
    }

    fn load_directory(&mut self, path: &str) -> bool {
        let mut next_path = PathBuffer::empty();
        if !next_path.set(path) {
            self.status = "Path is too long";
            return false;
        }

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
        let limit = count.min(MAX_ITEMS);

        for entry in raw_entries.iter().take(limit) {
            let name = match entry.name_str() {
                Some(name) => name,
                None => continue,
            };

            if name.is_empty() || name == "." || name == ".." {
                continue;
            }

            let name_bytes = name.as_bytes();
            if name_bytes.len() >= MAX_NAME_BYTES || next_count >= MAX_ITEMS {
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

        self.current_path = next_path;
        self.items = next_items;
        self.item_count = next_count;
        self.hovered_target = HoverTarget::None;
        self.selected_item = None;
        self.scroll_row = 0;
        self.ignore_next_double_click = false;
        if self.hand_cursor {
            self.hand_cursor = false;
            let _ = gui::set_cursor(Cursor::Arrow);
        }
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
                let name = item.name_str();
                let name_bytes = name.as_bytes();
                if name_bytes.len() >= 4
                    && name_bytes[name_bytes.len() - 4..].eq_ignore_ascii_case(b".txt")
                {
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
        } else {
            self.status = "Already at filesystem root";
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

    fn total_rows(&self) -> usize {
        if self.item_count == 0 {
            return 0;
        }
        let columns = self.grid_columns(self.client_width).max(1);
        (self.item_count + columns - 1) / columns
    }

    fn visible_rows(&self) -> usize {
        let viewport_height =
            (self.client_height - FOOTER_HEIGHT - GRID_ORIGIN_Y).max(0);
        if viewport_height < CARD_HEIGHT {
            return 1;
        }
        ((viewport_height + CARD_GAP) / (CARD_HEIGHT + CARD_GAP)).max(1) as usize
    }

    fn max_scroll_row(&self) -> usize {
        self.total_rows().saturating_sub(self.visible_rows())
    }

    fn clamp_scroll(&mut self) {
        self.scroll_row = self.scroll_row.min(self.max_scroll_row());
    }

    fn item_at(&self, x: i32, y: i32) -> Option<usize> {
        let grid_bottom = self.client_height - FOOTER_HEIGHT;
        if x < SIDEBAR_WIDTH + 12
            || y < GRID_ORIGIN_Y
            || y >= grid_bottom
        {
            return None;
        }

        let columns = self.grid_columns(self.client_width);
        let relative_x = x - (SIDEBAR_WIDTH + 20);
        let relative_y = y - GRID_ORIGIN_Y;
        if relative_x < 0 || relative_y < 0 {
            return None;
        }

        let stride_x = CARD_WIDTH + CARD_GAP;
        let stride_y = CARD_HEIGHT + CARD_GAP;
        let column = (relative_x / stride_x) as usize;
        let visible_row = (relative_y / stride_y) as usize;
        if column >= columns
            || relative_x % stride_x >= CARD_WIDTH
            || relative_y % stride_y >= CARD_HEIGHT
        {
            return None;
        }

        let row = self.scroll_row + visible_row;
        let index = row * columns + column;
        if index < self.item_count { Some(index) } else { None }
    }

    fn root_at(x: i32, y: i32) -> Option<usize> {
        if x < 0 || x >= SIDEBAR_WIDTH {
            return None;
        }

        const ROOT_TOP: i32 = 106;
        const ROOT_ROW_HEIGHT: i32 = 40;
        if y < ROOT_TOP {
            return None;
        }

        let index = ((y - ROOT_TOP) / ROOT_ROW_HEIGHT) as usize;
        let row_top = ROOT_TOP + index as i32 * ROOT_ROW_HEIGHT;
        if index < ROOTS.len() && y < row_top + 34 {
            Some(index)
        } else {
            None
        }
    }

    fn toolbar_at(x: i32, y: i32) -> HoverTarget {
        if BTN_PARENT.contains(x, y) {
            HoverTarget::Parent
        } else if BTN_REFRESH.contains(x, y) {
            HoverTarget::Refresh
        } else {
            HoverTarget::None
        }
    }


    fn target_at(&self, x: i32, y: i32) -> HoverTarget {
        if let Some(index) = Self::root_at(x, y) {
            return HoverTarget::Root(index);
        }

        let toolbar = Self::toolbar_at(x, y);
        if toolbar != HoverTarget::None {
            return toolbar;
        }

        match self.item_at(x, y) {
            Some(index) => HoverTarget::Item(index),
            None => HoverTarget::None,
        }
    }

    fn pointer_move(&mut self, x: i32, y: i32) -> Flow {
        let target = self.target_at(x, y);
        if target == self.hovered_target {
            return Flow::Ignore;
        }

        self.hovered_target = target;
        let wants_hand = target != HoverTarget::None;
        if wants_hand != self.hand_cursor {
            self.hand_cursor = wants_hand;
            let cursor = if wants_hand { Cursor::Hand } else { Cursor::Arrow };
            let _ = gui::set_cursor(cursor);
        }

        Flow::Changed
    }

    fn pointer_click(&mut self, x: i32, y: i32) -> Flow {
        // A fresh ordinary click starts a new click sequence. A directory
        // opened by this click may cause the kernel's following double-click
        // event to land over a different item in the newly loaded directory.
        self.ignore_next_double_click = false;

        if let Some(index) = Self::root_at(x, y) {
            let (_, path) = ROOTS[index];
            let _ = self.load_directory(path);
            return Flow::Changed;
        }

        match Self::toolbar_at(x, y) {
            HoverTarget::Parent => {
                self.open_parent();
                return Flow::Changed;
            }
            HoverTarget::Refresh => {
                let current = self.current_path;
                let _ = self.load_directory(current.as_str());
                return Flow::Changed;
            }
            _ => {}
        }

        if let Some(index) = self.item_at(x, y) {
            self.selected_item = Some(index);
            if self.items[index].kind == ItemKind::Directory {
                self.open_item(index);
                // The kernel sends DBLCLICK instead of DOWN for the second
                // press. Ignore it after single-click directory navigation.
                self.ignore_next_double_click = true;
            } else {
                self.status = "Item selected - double-click to open";
            }
            return Flow::Changed;
        }

        if self.selected_item.take().is_some() {
            Flow::Changed
        } else {
            Flow::Ignore
        }
    }

    fn pointer_double_click(&mut self, x: i32, y: i32) -> Flow {
        if self.ignore_next_double_click {
            self.ignore_next_double_click = false;
            return Flow::Ignore;
        }

        if let Some(index) = self.item_at(x, y) {
            self.selected_item = Some(index);
            self.open_item(index);
            return Flow::Changed;
        }

        // Sidebar and toolbar actions are intentionally single-click actions.
        Flow::Ignore
    }

    fn scroll_wheel(&mut self, x: i32, y: i32, delta: i32) -> Flow {
        if delta == 0
            || x < SIDEBAR_WIDTH
            || y < GRID_ORIGIN_Y
            || y >= self.client_height - FOOTER_HEIGHT
        {
            return Flow::Ignore;
        }

        let previous = self.scroll_row;
        if delta > 0 {
            self.scroll_row = self.scroll_row.saturating_sub(1);
        } else {
            self.scroll_row = (self.scroll_row + 1).min(self.max_scroll_row());
        }

        if self.scroll_row != previous {
            Flow::Changed
        } else {
            Flow::Ignore
        }
    }

    fn select_item(&mut self, index: usize) -> Flow {
        if index >= self.item_count || self.selected_item == Some(index) {
            return Flow::Ignore;
        }

        self.selected_item = Some(index);
        let columns = self.grid_columns(self.client_width).max(1);
        let row = index / columns;
        let visible = self.visible_rows();
        if row < self.scroll_row {
            self.scroll_row = row;
        } else if row >= self.scroll_row + visible {
            self.scroll_row = row + 1 - visible;
        }
        self.clamp_scroll();
        self.status = "Item selected";
        Flow::Changed
    }

    fn move_selection(&mut self, key: u32) -> Flow {
        if self.item_count == 0 {
            return Flow::Ignore;
        }

        let columns = self.grid_columns(self.client_width).max(1);
        let current = self.selected_item.unwrap_or(0);
        let next = match key {
            KEY_LEFT => current.saturating_sub(1),
            KEY_RIGHT => (current + 1).min(self.item_count - 1),
            KEY_UP => current.saturating_sub(columns),
            KEY_DOWN => (current + columns).min(self.item_count - 1),
            KEY_HOME => 0,
            KEY_END => self.item_count - 1,
            _ => return Flow::Ignore,
        };

        self.select_item(next)
    }

    fn open_selected(&mut self) -> Flow {
        match self.selected_item {
            Some(index) if index < self.item_count => {
                self.open_item(index);
                Flow::Changed
            }
            _ => Flow::Ignore,
        }
    }
}

impl App for FileManagerApp {
    fn update(&mut self, ev: &Event) -> Flow {
        match ev.kind {
            EVT_CLOSE_REQ => Flow::Quit,
            EVT_KEY_DOWN => match ev.key {
                0x1B => Flow::Quit,
                0x08 => {
                    self.open_parent();
                    Flow::Changed
                }
                0x0D | 0x0A => self.open_selected(),
                KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN | KEY_HOME | KEY_END => {
                    self.move_selection(ev.key)
                }
                _ => Flow::Ignore,
            },
            EVT_MOUSE_MOVE => self.pointer_move(ev.x, ev.y),
            EVT_MOUSE_DOWN => self.pointer_click(ev.x, ev.y),
            EVT_MOUSE_DBLCLICK => self.pointer_double_click(ev.x, ev.y),
            EVT_MOUSE_WHEEL => self.scroll_wheel(ev.x, ev.y, ev.key as i32),
            EVT_CONTEXT_MENU => {
                if let Some(index) = self.item_at(ev.x, ev.y) {
                    self.selected_item = Some(index);
                    self.status = "Context menu is not implemented";
                    Flow::Changed
                } else {
                    Flow::Ignore
                }
            }
            EVT_RESIZE => {
                // gui_resize_window() posts the new client/back-buffer size in x/y.
                self.client_width = ev.x.max(1);
                self.client_height = ev.y.max(1);
                self.clamp_scroll();
                Flow::Ignore // gui::run always repaints after EVT_RESIZE.
            }
            _ => Flow::Ignore,
        }
    }

    fn render(&self, w: &Window) -> rsbr::Result<()> {
        let (client_width, client_height) = w.size()?;
        let width = client_width as i32;
        let height = client_height as i32;
        let main_width = (width - SIDEBAR_WIDTH).max(0) as u32;
        let grid_bottom = (height - FOOTER_HEIGHT).max(GRID_ORIGIN_Y);

        w.clear(COLOR_BACKGROUND)?;

        self.render_sidebar(w, height)?;
        self.render_header(w, main_width)?;
        self.render_grid(w, width, grid_bottom)?;
        self.render_footer(w, width, main_width, grid_bottom)?;

        Ok(())
    }
}

impl FileManagerApp {
    fn draw_button(
        &self,
        w: &Window,
        rect: Rect,
        label: &str,
        hovered: bool,
    ) -> rsbr::Result<()> {
        let bg = if hovered { COLOR_ACCENT_SOFT } else { COLOR_SIDEBAR };
        w.fill_rect(rect.x, rect.y, rect.w as u32, rect.h as u32, bg)?;
        w.draw_rect(rect.x, rect.y, rect.w as u32, rect.h as u32, COLOR_CARD_BORDER)?;

        // Text centered (if symbol widht is 8px)
        let text_w = label.len() as i32 * 8;
        let text_x = rect.x + ((rect.w - text_w) / 2).max(4);
        let text_y = rect.y + (rect.h - 10) / 2;
        w.draw_text(text_x, text_y, label, COLOR_TEXT)
    }

        fn draw_toolbar_button(
        &self,
        w: &Window,
        rect: Rect,
        label: &str,
        hovered: bool,
    ) -> rsbr::Result<()> {
        if hovered {
            w.fill_rect(
                rect.x + 1,
                rect.y + 1,
                (rect.w - 1) as u32,
                (rect.h - 2) as u32,
                COLOR_ACCENT_SOFT,
            )?;
        }

        let text_w = label.len() as i32 * 8;
        let text_x = rect.x + ((rect.w - text_w) / 2).max(4);
        let text_y = rect.y + (rect.h - 10) / 2;
        let text_color = if hovered { COLOR_ACCENT } else { COLOR_TEXT };
        w.draw_text(text_x, text_y, label, text_color)
    }


    fn render_sidebar(&self, w: &Window, height: i32) -> rsbr::Result<()> {
        w.fill_rect(0, 0, SIDEBAR_WIDTH as u32, height as u32, COLOR_SIDEBAR)?;
        w.fill_rect(SIDEBAR_WIDTH, 0, 1, height as u32, COLOR_DIVIDER)?;
        w.draw_text(22, 28, "LOCATIONS", COLOR_MUTED)?;

        for (index, (label, path)) in ROOTS.iter().enumerate() {
            let top = 106 + index as i32 * 40;
            let active = self.is_root_active(path);
            let hovered = self.hovered_target == HoverTarget::Root(index);

            if active || hovered {
                let bg = if active { COLOR_ACCENT_SOFT } else { COLOR_CARD };
                w.fill_rect(10, top, (SIDEBAR_WIDTH - 20) as u32, 34, bg)?;
                if active {
                    w.fill_rect(10, top + 5, 3, 24, COLOR_ACCENT)?;
                }
            }

            let text_color = if active || hovered { COLOR_ACCENT } else { COLOR_TEXT };
            w.draw_text(28, top + 11, label, text_color)?;
        }
        Ok(())
    }

    fn render_header(&self, w: &Window, main_width: u32) -> rsbr::Result<()> {
        w.fill_rect(SIDEBAR_WIDTH, 0, main_width, HEADER_HEIGHT as u32, COLOR_HEADER)?;
        w.fill_rect(SIDEBAR_WIDTH, HEADER_HEIGHT, main_width, 1, COLOR_DIVIDER)?;
        w.draw_text(SIDEBAR_WIDTH + 20, 18, "FILES", COLOR_TEXT)?;

        let bar_w = (main_width as i32 - 32).max(220) as u32;
        w.fill_rect(TOOLBAR_X, TOOLBAR_Y, bar_w, TOOLBAR_H as u32, COLOR_SIDEBAR)?;
        w.draw_rect(TOOLBAR_X, TOOLBAR_Y, bar_w, TOOLBAR_H as u32, COLOR_CARD_BORDER)?;

        self.draw_toolbar_button(w, BTN_PARENT, "Parent", self.hovered_target == HoverTarget::Parent)?;
        w.fill_rect(BTN_REFRESH.x, TOOLBAR_Y, 1, TOOLBAR_H as u32, COLOR_CARD_BORDER)?; // Разделитель 1

        self.draw_toolbar_button(w, BTN_REFRESH, "Refresh", self.hovered_target == HoverTarget::Refresh)?;
        let path_x = BTN_REFRESH.x + BTN_REFRESH.w;
        w.fill_rect(path_x, TOOLBAR_Y, 1, TOOLBAR_H as u32, COLOR_CARD_BORDER)?; // Разделитель 2

        w.draw_text(path_x + 12, TOOLBAR_Y + 9, self.current_path.as_str(), COLOR_MUTED)
    }


    fn render_card(&self, w: &Window, index: usize, x: i32, y: i32) -> rsbr::Result<()> {
        let hovered = self.hovered_target == HoverTarget::Item(index);
        let selected = self.selected_item == Some(index);

        let card_color = if hovered {
            COLOR_CARD_HOVER
        } else if selected {
            COLOR_CARD_SELECTED
        } else {
            COLOR_CARD
        };
        let border_color = if selected { COLOR_ACCENT } else { COLOR_CARD_BORDER };

        w.fill_rect(x, y, CARD_WIDTH as u32, CARD_HEIGHT as u32, card_color)?;
        w.draw_rect(x, y, CARD_WIDTH as u32, CARD_HEIGHT as u32, border_color)?;

        match self.items[index].kind {
            ItemKind::Directory => Self::draw_folder_icon(w, x + 31, y + 13)?,
            ItemKind::File | ItemKind::Other => Self::draw_file_icon(w, x + 37, y + 11)?,
        }

        let label = Self::short_label(self.items[index].name_str());
        let label_width = (label.len() as i32 * 8).min(CARD_WIDTH - 8);
        let label_x = x + ((CARD_WIDTH - label_width) / 2).max(4);
        let label_color = if hovered || selected { COLOR_ACCENT } else { COLOR_TEXT };

        w.draw_text(label_x, y + 78, label, label_color)
    }

    fn render_grid(&self, w: &Window, width: i32, grid_bottom: i32) -> rsbr::Result<()> {
        let columns = self.grid_columns(width).max(1);
        let first_row = self.scroll_row;
        let last_row = (first_row + self.visible_rows()).min(self.total_rows());

        for index in 0..self.item_count {
            let row = index / columns;
            if row < first_row || row >= last_row {
                continue;
            }

            let column = index % columns;
            let x = SIDEBAR_WIDTH + 20 + column as i32 * (CARD_WIDTH + CARD_GAP);
            let y = GRID_ORIGIN_Y + (row - first_row) as i32 * (CARD_HEIGHT + CARD_GAP);

            if y >= GRID_ORIGIN_Y && y + CARD_HEIGHT <= grid_bottom {
                self.render_card(w, index, x, y)?;
            }
        }
        Ok(())
    }

    fn render_footer(&self, w: &Window, width: i32, main_width: u32, grid_bottom: i32) -> rsbr::Result<()> {
        w.fill_rect(SIDEBAR_WIDTH, grid_bottom, main_width, 1, COLOR_DIVIDER)?;
        w.draw_text(SIDEBAR_WIDTH + 20, grid_bottom + 9, self.status, COLOR_MUTED)?;
        w.draw_text(
            (width - 154).max(SIDEBAR_WIDTH + 20),
            grid_bottom + 9,
            "Wheel: scroll",
            COLOR_MUTED,
        )
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
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let window = match Window::create(
        WINDOW_X,
        WINDOW_Y,
        WINDOW_WIDTH as u32,
        WINDOW_HEIGHT as u32,
        Some("Files"),
        FLAG_HAS_TITLE | FLAG_HAS_CLOSE | FLAG_HAS_MINMAX | FLAG_RESIZABLE | FLAG_MOVABLE,
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