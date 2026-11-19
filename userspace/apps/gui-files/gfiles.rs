
#![no_std]
#![no_main]

extern crate rsbr;

use core::str;

use rsbr::fs::{read_dir, DirEntry};
use rsbr::gui::{
    self, App, Cursor, Event, Flow, Window,
    EVT_CLOSE_REQ, EVT_CONTEXT_MENU, EVT_KEY_DOWN, EVT_MOUSE_DOWN,
    EVT_MOUSE_DBLCLICK, EVT_MOUSE_MOVE, EVT_MOUSE_WHEEL, EVT_RESIZE,
    FLAG_HAS_CLOSE, FLAG_HAS_TITLE, FLAG_HAS_MINMAX, FLAG_MOVABLE,
    FLAG_RESIZABLE,
    KEY_DOWN, KEY_END, KEY_HOME, KEY_LEFT, KEY_RIGHT, KEY_UP,
};
use rsbr::process;

// =============================================================================
// Application configuration
// =============================================================================

const WINDOW_X: i32 = 72;
const WINDOW_Y: i32 = 54;
const WINDOW_WIDTH: i32 = 1120;
const WINDOW_HEIGHT: i32 = 760;

const MAX_ITEMS: usize = 32;
const MAX_NAME_BYTES: usize = 60;
const MAX_PATH_BYTES: usize = 256;

const GEDIT_PATH: &str = "/bin/gedit";

const ROOTS: [(&str, &str); 6] = [
    ("/", "/"),
    ("bin/", "/bin"),
    ("lib/", "/lib"),
    ("rv64/", "/rv64"),
    ("apps/", "/apps"),
    ("demos/", "/demos"),
];

// =============================================================================
// Theme
// =============================================================================

#[derive(Clone, Copy)]
struct Theme {
    background: u32,
    sidebar: u32,
    header: u32,

    card: u32,
    card_hover: u32,
    card_selected: u32,
    card_border: u32,

    divider: u32,

    text: u32,
    muted: u32,

    accent: u32,
    accent_soft: u32,

    folder: u32,
    folder_dark: u32,
    folder_highlight: u32,

    file: u32,
    file_line: u32,
}

impl Theme {
    const LIGHT: Self = Self {
        background: 0x00F5F7FA,
        sidebar: 0x00EDF1F6,
        header: 0x00FFFFFF,

        card: 0x00FFFFFF,
        card_hover: 0x00EAF2FF,
        card_selected: 0x00DCEAFF,
        card_border: 0x00D7DEE8,

        divider: 0x00D7DEE8,

        text: 0x00212B3A,
        muted: 0x006B7788,

        accent: 0x003D78D8,
        accent_soft: 0x00DCEAFF,

        folder: 0x00E9B84C,
        folder_dark: 0x00C89127,
        folder_highlight: 0x00F7D98C,

        file: 0x00FFFFFF,
        file_line: 0x0096A2B2,
    };
}

const THEME: Theme = Theme::LIGHT;

// =============================================================================
// Layout configuration
//
// Edit these values to customize the interface.
// All coordinates are calculated by Layout.
// =============================================================================

#[derive(Clone, Copy)]
struct LayoutConfig {
    sidebar_width: i32,
    header_height: i32,
    footer_height: i32,

    toolbar_left_margin: i32,
    toolbar_right_margin: i32,
    toolbar_top: i32,
    toolbar_height: i32,

    parent_button_width: i32,
    refresh_button_width: i32,

    grid_left_padding: i32,
    grid_right_padding: i32,
    grid_top_padding: i32,

    card_width: i32,
    card_height: i32,
    card_gap: i32,

    sidebar_heading_x: i32,
    sidebar_heading_y: i32,

    sidebar_root_top: i32,
    sidebar_root_step: i32,
    sidebar_root_height: i32,
    sidebar_root_inset: i32,
}

impl LayoutConfig {
    const DEFAULT: Self = Self {
        sidebar_width: 192,
        header_height: 88,
        footer_height: 30,

        toolbar_left_margin: 16,
        toolbar_right_margin: 16,
        toolbar_top: 48,
        toolbar_height: 28,

        parent_button_width: 36,
        refresh_button_width: 36,

        grid_left_padding: 20,
        grid_right_padding: 12,
        grid_top_padding: 20,

        card_width: 108,
        card_height: 106,
        card_gap: 10,

        sidebar_heading_x: 22,
        sidebar_heading_y: 28,

        sidebar_root_top: 106,
        sidebar_root_step: 40,
        sidebar_root_height: 34,
        sidebar_root_inset: 10,
    };
}

const LAYOUT_CONFIG: LayoutConfig = LayoutConfig::DEFAULT;

// =============================================================================
// Geometry
// =============================================================================

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

    fn right(&self) -> i32 {
        self.x.saturating_add(self.w)
    }

    fn bottom(&self) -> i32 {
        self.y.saturating_add(self.h)
    }

    fn contains(&self, px: i32, py: i32) -> bool {
        px >= self.x
            && px < self.right()
            && py >= self.y
            && py < self.bottom()
    }

    fn is_visible(&self) -> bool {
        self.w > 0 && self.h > 0
    }
}

#[derive(Clone, Copy)]
struct GridLayout {
    bounds: Rect,

    cell_width: i32,
    cell_height: i32,

    gap_x: i32,
    gap_y: i32,

    columns: usize,
    visible_rows: usize,
}

impl GridLayout {
    fn new(bounds: Rect, config: LayoutConfig) -> Self {
        let cell_width = config.card_width.max(1);
        let cell_height = config.card_height.max(1);
        let gap_x = config.card_gap.max(0);
        let gap_y = config.card_gap.max(0);

        let stride_x = cell_width.saturating_add(gap_x).max(1);
        let stride_y = cell_height.saturating_add(gap_y).max(1);

        let columns = ((bounds.w.saturating_add(gap_x)) / stride_x)
            .max(1) as usize;

        let visible_rows = if bounds.h < cell_height {
            1
        } else {
            ((bounds.h.saturating_add(gap_y)) / stride_y)
                .max(1) as usize
        };

        Self {
            bounds,
            cell_width,
            cell_height,
            gap_x,
            gap_y,
            columns,
            visible_rows,
        }
    }

    fn total_rows(&self, item_count: usize) -> usize {
        if item_count == 0 {
            return 0;
        }

        (item_count + self.columns - 1) / self.columns
    }

    fn max_scroll_row(&self, item_count: usize) -> usize {
        self.total_rows(item_count)
            .saturating_sub(self.visible_rows)
    }

    fn card_rect(&self, index: usize, scroll_row: usize) -> Option<Rect> {
        let column = index % self.columns;
        let row = index / self.columns;

        if row < scroll_row || row >= scroll_row + self.visible_rows {
            return None;
        }

        let visible_row = row - scroll_row;

        let x = self.bounds.x + column as i32 * (self.cell_width + self.gap_x);
        let y = self.bounds.y + visible_row as i32 * (self.cell_height + self.gap_y);

        let rect = Rect::new(x, y, self.cell_width, self.cell_height);

        if rect.x < self.bounds.x
            || rect.right() > self.bounds.right()
            || rect.y < self.bounds.y
            || rect.bottom() > self.bounds.bottom()
        {
            return None;
        }

        Some(rect)
    }

    fn item_at(
        &self,
        x: i32,
        y: i32,
        scroll_row: usize,
        item_count: usize,
    ) -> Option<usize> {
        if !self.bounds.contains(x, y) {
            return None;
        }

        let relative_x = x - self.bounds.x;
        let relative_y = y - self.bounds.y;

        let stride_x = self.cell_width + self.gap_x;
        let stride_y = self.cell_height + self.gap_y;

        let column = (relative_x / stride_x) as usize;
        let visible_row = (relative_y / stride_y) as usize;

        if column >= self.columns
            || visible_row >= self.visible_rows
            || relative_x % stride_x >= self.cell_width
            || relative_y % stride_y >= self.cell_height
        {
            return None;
        }

        let row = scroll_row + visible_row;
        let index = row * self.columns + column;

        if index >= item_count {
            return None;
        }

        self.card_rect(index, scroll_row)
            .filter(|rect| rect.contains(x, y))
            .map(|_| index)
    }
}

// =============================================================================
// Complete window layout
// =============================================================================

#[derive(Clone, Copy)]
struct Layout {
    window: Rect,

    sidebar: Rect,
    header: Rect,
    toolbar: Rect,

    parent_button: Rect,
    refresh_button: Rect,
    path_area: Rect,

    grid: GridLayout,
    footer: Rect,

    config: LayoutConfig,
}

impl Layout {
    fn calculate(width: i32, height: i32, config: LayoutConfig) -> Self {
        let width = width.max(1);
        let height = height.max(1);

        let window = Rect::new(0, 0, width, height);

        let sidebar_width = config.sidebar_width.max(0).min(width);
        let header_height = config.header_height.max(0).min(height);
        let footer_height = config.footer_height.max(0).min(height);

        let sidebar = Rect::new(
            0,
            0,
            sidebar_width,
            height,
        );

        let main_x = sidebar_width;
        let main_width = width - sidebar_width;

        let header = Rect::new(
            main_x,
            0,
            main_width,
            header_height,
        );

        let toolbar_height = config.toolbar_height.max(0).min(header_height);

        let toolbar_y = config.toolbar_top
            .min(header_height.saturating_sub(toolbar_height).max(0));

        let toolbar_x = main_x + config.toolbar_left_margin.max(0);

        let toolbar_width = (
            main_width
                - config.toolbar_left_margin.max(0)
                - config.toolbar_right_margin.max(0)
        ).max(0);

        let toolbar = Rect::new(
            toolbar_x,
            toolbar_y,
            toolbar_width,
            toolbar_height,
        );

        let parent_width = config.parent_button_width
            .max(0)
            .min(toolbar_width);

        let refresh_width = config.refresh_button_width
            .max(0)
            .min(toolbar_width - parent_width);

        let refresh_button = Rect::new(
            toolbar.right() - refresh_width,
            toolbar.y,
            refresh_width,
            toolbar.h,
        );

        let parent_button = Rect::new(
            refresh_button.x - parent_width,
            toolbar.y,
            parent_width,
            toolbar.h,
        );

        let path_area = Rect::new(
            toolbar.x,
            toolbar.y,
            (parent_button.x - toolbar.x).max(0),
            toolbar.h,
        );

        let footer_y = height - footer_height;

        let footer = Rect::new(
            main_x,
            footer_y,
            main_width,
            footer_height,
        );

        let grid_x = main_x + config.grid_left_padding.max(0);
        let grid_y = header.bottom() + config.grid_top_padding.max(0);

        let grid_width = (
            width
                - grid_x
                - config.grid_right_padding.max(0)
        ).max(0);

        let grid_height = (footer_y - grid_y).max(0);

        let grid_bounds = Rect::new(
            grid_x,
            grid_y,
            grid_width,
            grid_height,
        );

        let grid = GridLayout::new(grid_bounds, config);

        Self {
            window,
            sidebar,
            header,
            toolbar,
            parent_button,
            refresh_button,
            path_area,
            grid,
            footer,
            config,
        }
    }

    fn root_item_rect(&self, index: usize) -> Rect {
        let config = self.config;

        let inset = config.sidebar_root_inset.max(0);

        let width = (self.sidebar.w - inset * 2).max(0);

        Rect::new(
            self.sidebar.x + inset,
            config.sidebar_root_top
                + index as i32 * config.sidebar_root_step,
            width,
            config.sidebar_root_height.max(0),
        )
    }
}

// =============================================================================
// Paths
// =============================================================================

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

        if bytes.is_empty()
            || bytes[0] != b'/'
            || bytes.len() >= MAX_PATH_BYTES
        {
            return false;
        }

        self.bytes = [0; MAX_PATH_BYTES];
        self.bytes[..bytes.len()].copy_from_slice(bytes);

        self.len = bytes.len();

        true
    }

    fn as_str(&self) -> &str {
        str::from_utf8(&self.bytes[..self.len])
            .unwrap_or("/")
    }

    fn push_name(&mut self, name: &str) -> bool {
        if name.is_empty()
            || name == "."
            || name == ".."
            || name.as_bytes().contains(&b'/')
        {
            return false;
        }

        let needs_slash = self.len > 1
            && self.bytes[self.len - 1] != b'/';

        let slash_len = if needs_slash { 1 } else { 0 };

        let required = self.len + slash_len + name.len();

        if required >= MAX_PATH_BYTES {
            return false;
        }

        let mut offset = self.len;

        if needs_slash {
            self.bytes[offset] = b'/';
            offset += 1;
        }

        self.bytes[offset..offset + name.len()]
            .copy_from_slice(name.as_bytes());

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

// =============================================================================
// Filesystem data
// =============================================================================

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum ItemKind {
    Directory,
    File,
    Other,
}

#[derive(Clone, Copy)]
struct FileItem {
    name: [u8; MAX_NAME_BYTES],
    name_len: u8,
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
        str::from_utf8(&self.name[..self.name_len as usize])
            .unwrap_or("<invalid name>")
    }
}

struct FileList {
    items: [FileItem; MAX_ITEMS],
    count: usize,
}

impl FileList {
    const fn empty() -> Self {
        Self {
            items: [FileItem::empty(); MAX_ITEMS],
            count: 0,
        }
    }

    fn read(path: &str) -> Result<Self, &'static str> {
        let mut raw_entries = [DirEntry::empty(); MAX_ITEMS];

        let count = read_dir(path, &mut raw_entries)
            .map_err(|_| "Unable to read directory")?;

        let mut result = Self::empty();

        for entry in raw_entries.iter().take(count.min(MAX_ITEMS)) {
            let name = match entry.name_str() {
                Some(name) => name,
                None => continue,
            };

            if name.is_empty()
                || name == "."
                || name == ".."
            {
                continue;
            }

            let name_bytes = name.as_bytes();

            if name_bytes.len() >= MAX_NAME_BYTES
                || result.count >= MAX_ITEMS
            {
                continue;
            }

            let kind = if entry.is_dir() {
                ItemKind::Directory
            } else if entry.is_file() {
                ItemKind::File
            } else {
                ItemKind::Other
            };

            let item = &mut result.items[result.count];

            item.name[..name_bytes.len()]
                .copy_from_slice(name_bytes);

            item.name[name_bytes.len()] = 0;
            item.name_len = name_bytes.len() as u8;
            item.kind = kind;

            result.count += 1;
        }

        Ok(result)
    }

    fn get(&self, index: usize) -> Option<&FileItem> {
        if index < self.count {
            Some(&self.items[index])
        } else {
            None
        }
    }
}

// =============================================================================
// Filesystem
// =============================================================================

struct FileSystem {
    current_path: PathBuffer,
    files: FileList,
}

impl FileSystem {
    const fn empty() -> Self {
        Self {
            current_path: PathBuffer::empty(),
            files: FileList::empty(),
        }
    }

    fn load_directory(&mut self, path: &str) -> Result<(), &'static str> {
        let mut next_path = PathBuffer::empty();

        if !next_path.set(path) {
            return Err("Path is too long");
        }

        // Load into temporary storage first.
        // The current directory remains intact if reading fails.
        let next_files = FileList::read(path)?;

        self.current_path = next_path;
        self.files = next_files;

        Ok(())
    }

    fn item_path(&self, index: usize) -> Option<PathBuffer> {
        let item = self.files.get(index)?;

        let mut path = self.current_path;

        if !path.push_name(item.name_str()) {
            return None;
        }

        Some(path)
    }

    fn item(&self, index: usize) -> Option<&FileItem> {
        self.files.get(index)
    }

    fn count(&self) -> usize {
        self.files.count
    }

    fn path(&self) -> &str {
        self.current_path.as_str()
    }
}

// =============================================================================
// Navigation
// =============================================================================

#[derive(Clone, Copy, PartialEq, Eq)]
enum HoverTarget {
    None,
    Root(usize),
    Parent,
    Refresh,
    Item(usize),
}

#[derive(Clone, Copy)]
struct NavigationState {
    selected: Option<usize>,
    hovered: HoverTarget,

    scroll_row: usize,

    ignore_next_double_click: bool,
}

impl NavigationState {
    const fn new() -> Self {
        Self {
            selected: None,
            hovered: HoverTarget::None,

            scroll_row: 0,

            ignore_next_double_click: false,
        }
    }

    fn reset(&mut self) {
        *self = Self::new();
    }
}

// =============================================================================
// Viewport
// =============================================================================

#[derive(Clone, Copy)]
struct Viewport {
    width: i32,
    height: i32,
}

impl Viewport {
    const fn new(width: i32, height: i32) -> Self {
        Self {
            width,
            height,
        }
    }

    fn layout(&self) -> Layout {
        Layout::calculate(
            self.width,
            self.height,
            LAYOUT_CONFIG,
        )
    }
}

// =============================================================================
// Application
// =============================================================================

struct FileManagerApp {
    filesystem: FileSystem,
    navigation: NavigationState,

    viewport: Viewport,

    hand_cursor: bool,

    status: &'static str,
}

impl FileManagerApp {
    fn new() -> Self {
        let mut app = Self {
            filesystem: FileSystem::empty(),
            navigation: NavigationState::new(),

            viewport: Viewport::new(
                WINDOW_WIDTH,
                WINDOW_HEIGHT,
            ),

            hand_cursor: false,

            status: "Ready",
        };

        let _ = app.navigate_to("/");

        app
    }

    // -------------------------------------------------------------------------
    // Navigation and filesystem
    // -------------------------------------------------------------------------

    fn navigate_to(&mut self, path: &str) -> bool {
        match self.filesystem.load_directory(path) {
            Ok(()) => {
                self.navigation.reset();

                self.set_hand_cursor(false);

                self.status = "Directory loaded";

                true
            }
            Err(error) => {
                self.status = error;

                false
            }
        }
    }

    fn open_item(&mut self, index: usize) {
        let item = match self.filesystem.item(index) {
            Some(item) => *item,
            None => return,
        };

        let path = match self.filesystem.item_path(index) {
            Some(path) => path,
            None => {
                self.status = "Selected path is too long";
                return;
            }
        };

        match item.kind {
            ItemKind::Directory => {
                let _ = self.navigate_to(path.as_str());
            }

            ItemKind::File => {
                let name = item.name_str();
                let bytes = name.as_bytes();

                if bytes.len() >= 4
                    && bytes[bytes.len() - 4..]
                        .eq_ignore_ascii_case(b".txt")
                {
                    match process::spawn(
                        GEDIT_PATH,
                        &[path.as_str()],
                    ) {
                        Ok(_) => {
                            self.status = "Opened in gedit";
                        }
                        Err(_) => {
                            self.status = "Failed to start gedit";
                        }
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
        let mut parent = self.filesystem.current_path;

        if parent.parent() {
            let _ = self.navigate_to(parent.as_str());
        } else {
            self.status = "Already at filesystem root";
        }
    }

    fn refresh(&mut self) {
        let path = self.filesystem.current_path;

        let _ = self.navigate_to(path.as_str());
    }

    fn is_root_active(&self, root_path: &str) -> bool {
        let current = self.filesystem.path();

        if root_path == "/" {
            return current == "/";
        }

        current == root_path
            || (current.starts_with(root_path)
                && current.as_bytes().get(root_path.len()) == Some(&b'/'))
    }

    // -------------------------------------------------------------------------
    // Cursor
    // -------------------------------------------------------------------------

    fn set_hand_cursor(&mut self, enabled: bool) {
        if self.hand_cursor == enabled {
            return;
        }

        self.hand_cursor = enabled;

        let cursor = if enabled {
            Cursor::Hand
        } else {
            Cursor::Arrow
        };

        let _ = gui::set_cursor(cursor);
    }

    // -------------------------------------------------------------------------
    // Layout helpers
    // -------------------------------------------------------------------------

    fn layout(&self) -> Layout {
        self.viewport.layout()
    }

    fn root_at(&self, x: i32, y: i32, layout: &Layout) -> Option<usize> {
        for index in 0..ROOTS.len() {
            if layout.root_item_rect(index).contains(x, y) {
                return Some(index);
            }
        }

        None
    }

    fn target_at(&self, x: i32, y: i32, layout: &Layout) -> HoverTarget {
        if let Some(index) = self.root_at(x, y, layout) {
            return HoverTarget::Root(index);
        }

        if layout.parent_button.contains(x, y) {
            return HoverTarget::Parent;
        }

        if layout.refresh_button.contains(x, y) {
            return HoverTarget::Refresh;
        }

        match layout.grid.item_at(
            x,
            y,
            self.navigation.scroll_row,
            self.filesystem.count(),
        ) {
            Some(index) => HoverTarget::Item(index),
            None => HoverTarget::None,
        }
    }

    fn clamp_scroll(&mut self, layout: &Layout) {
        self.navigation.scroll_row = self.navigation.scroll_row
            .min(layout.grid.max_scroll_row(self.filesystem.count()));
    }

    // -------------------------------------------------------------------------
    // Pointer input
    // -------------------------------------------------------------------------

    fn pointer_move(&mut self, x: i32, y: i32) -> Flow {
        let layout = self.layout();

        let target = self.target_at(x, y, &layout);

        if target == self.navigation.hovered {
            return Flow::Ignore;
        }

        self.navigation.hovered = target;

        self.set_hand_cursor(target != HoverTarget::None);

        Flow::Changed
    }

    fn pointer_click(&mut self, x: i32, y: i32) -> Flow {
        self.navigation.ignore_next_double_click = false;

        let layout = self.layout();

        if let Some(index) = self.root_at(x, y, &layout) {
            let (_, path) = ROOTS[index];

            let _ = self.navigate_to(path);

            return Flow::Changed;
        }

        if layout.parent_button.contains(x, y) {
            self.open_parent();
            return Flow::Changed;
        }

        if layout.refresh_button.contains(x, y) {
            self.refresh();
            return Flow::Changed;
        }

        if let Some(index) = layout.grid.item_at(
            x,
            y,
            self.navigation.scroll_row,
            self.filesystem.count(),
        ) {
            self.navigation.selected = Some(index);

            let kind = self.filesystem
                .item(index)
                .map(|item| item.kind);

            if kind == Some(ItemKind::Directory) {
                self.open_item(index);

                // The next double-click event may refer to an item
                // at the same screen position in the newly loaded directory.
                self.navigation.ignore_next_double_click = true;
            } else {
                self.status = "Item selected - double-click to open";
            }

            return Flow::Changed;
        }

        if self.navigation.selected.take().is_some() {
            Flow::Changed
        } else {
            Flow::Ignore
        }
    }

    fn pointer_double_click(&mut self, x: i32, y: i32) -> Flow {
        if self.navigation.ignore_next_double_click {
            self.navigation.ignore_next_double_click = false;
            return Flow::Ignore;
        }

        let layout = self.layout();

        if let Some(index) = layout.grid.item_at(
            x,
            y,
            self.navigation.scroll_row,
            self.filesystem.count(),
        ) {
            self.navigation.selected = Some(index);

            self.open_item(index);

            return Flow::Changed;
        }

        Flow::Ignore
    }

    // -------------------------------------------------------------------------
    // Scrolling
    // -------------------------------------------------------------------------

    fn scroll_wheel(&mut self, x: i32, y: i32, delta: i32) -> Flow {
        if delta == 0 {
            return Flow::Ignore;
        }

        let layout = self.layout();

        if !layout.grid.bounds.contains(x, y) {
            return Flow::Ignore;
        }

        let previous = self.navigation.scroll_row;

        if delta > 0 {
            self.navigation.scroll_row =
                self.navigation.scroll_row.saturating_sub(1);
        } else {
            self.navigation.scroll_row = (
                self.navigation.scroll_row + 1
            ).min(layout.grid.max_scroll_row(self.filesystem.count()));
        }

        if previous != self.navigation.scroll_row {
            Flow::Changed
        } else {
            Flow::Ignore
        }
    }

    // -------------------------------------------------------------------------
    // Keyboard selection
    // -------------------------------------------------------------------------

    fn select_item(&mut self, index: usize) -> Flow {
        if index >= self.filesystem.count()
            || self.navigation.selected == Some(index)
        {
            return Flow::Ignore;
        }

        self.navigation.selected = Some(index);

        let layout = self.layout();
        let grid = layout.grid;

        let row = index / grid.columns;
        let visible_rows = grid.visible_rows.max(1);

        if row < self.navigation.scroll_row {
            self.navigation.scroll_row = row;
        } else if row >= self.navigation.scroll_row + visible_rows {
            self.navigation.scroll_row = row + 1 - visible_rows;
        }

        self.clamp_scroll(&layout);

        self.status = "Item selected";

        Flow::Changed
    }

    fn move_selection(&mut self, key: u32) -> Flow {
        let count = self.filesystem.count();

        if count == 0 {
            return Flow::Ignore;
        }

        let columns = self.layout().grid.columns.max(1);

        let current = self.navigation.selected.unwrap_or(0);

        let next = match key {
            KEY_LEFT => current.saturating_sub(1),
            KEY_RIGHT => (current + 1).min(count - 1),
            KEY_UP => current.saturating_sub(columns),
            KEY_DOWN => (current + columns).min(count - 1),
            KEY_HOME => 0,
            KEY_END => count - 1,

            _ => return Flow::Ignore,
        };

        self.select_item(next)
    }

    fn open_selected(&mut self) -> Flow {
        match self.navigation.selected {
            Some(index) if index < self.filesystem.count() => {
                self.open_item(index);
                Flow::Changed
            }

            _ => Flow::Ignore,
        }
    }
}

// =============================================================================
// RSBR application interface
// =============================================================================

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

                KEY_LEFT
                | KEY_RIGHT
                | KEY_UP
                | KEY_DOWN
                | KEY_HOME
                | KEY_END => self.move_selection(ev.key),

                _ => Flow::Ignore,
            },

            EVT_MOUSE_MOVE => {
                self.pointer_move(ev.x, ev.y)
            }

            EVT_MOUSE_DOWN => {
                self.pointer_click(ev.x, ev.y)
            }

            EVT_MOUSE_DBLCLICK => {
                self.pointer_double_click(ev.x, ev.y)
            }

            EVT_MOUSE_WHEEL => {
                self.scroll_wheel(ev.x, ev.y, ev.key as i32)
            }

            EVT_CONTEXT_MENU => {
                let layout = self.layout();

                if let Some(index) = layout.grid.item_at(
                    ev.x,
                    ev.y,
                    self.navigation.scroll_row,
                    self.filesystem.count(),
                ) {
                    self.navigation.selected = Some(index);
                    self.status = "Context menu is not implemented";

                    Flow::Changed
                } else {
                    Flow::Ignore
                }
            }

            EVT_RESIZE => {
                self.viewport = Viewport::new(
                    ev.x.max(1),
                    ev.y.max(1),
                );

                let layout = self.layout();

                self.clamp_scroll(&layout);

                // gui::run repaints after EVT_RESIZE.
                Flow::Ignore
            }

            _ => Flow::Ignore,
        }
    }

    fn render(&self, window: &Window) -> rsbr::Result<()> {
        let (width, height) = window.size()?;

        let layout = Layout::calculate(
            width as i32,
            height as i32,
            LAYOUT_CONFIG,
        );

        Renderer::render(window, self, &layout)
    }
}

// =============================================================================
// Renderer
// =============================================================================

struct Renderer;

impl Renderer {
    // -------------------------------------------------------------------------
    // Main rendering
    // -------------------------------------------------------------------------

    fn render(
        window: &Window,
        app: &FileManagerApp,
        layout: &Layout,
    ) -> rsbr::Result<()> {
        window.clear(THEME.background)?;

        Self::draw_sidebar(window, app, layout)?;
        Self::draw_header(window, app, layout)?;
        Self::draw_grid(window, app, layout)?;
        Self::draw_footer(window, app, layout)?;

        Ok(())
    }

    // -------------------------------------------------------------------------
    // Primitive drawing
    // -------------------------------------------------------------------------

    fn fill_rect(
        window: &Window,
        rect: Rect,
        color: u32,
    ) -> rsbr::Result<()> {
        if !rect.is_visible() {
            return Ok(());
        }

        window.fill_rect(
            rect.x,
            rect.y,
            rect.w as u32,
            rect.h as u32,
            color,
        )
    }

    fn draw_icon_button(
        window: &Window,
        rect: Rect,
        hovered: bool,
    ) -> rsbr::Result<()> {
        if !rect.is_visible() {
            return Ok(());
        }

        if hovered {
            Self::fill_rect(
                window,
                Rect::new(
                    rect.x + 2,
                    rect.y + 2,
                    (rect.w - 4).max(0),
                    (rect.h - 4).max(0),
                ),
                THEME.accent_soft,
            )?;
        }

        Ok(())
    }

    fn draw_parent_icon(
        window: &Window,
        rect: Rect,
        color: u32,
    ) -> rsbr::Result<()> {
        let center_x = rect.x + rect.w / 2;
        let center_y = rect.y + rect.h / 2;

        // Vertical shaft.
        window.draw_line(
            center_x,
            center_y + 7,
            center_x,
            center_y - 5,
            color,
        )?;

        // Arrow head.
        window.draw_line(
            center_x,
            center_y - 5,
            center_x - 6,
            center_y + 1,
            color,
        )?;

        window.draw_line(
            center_x,
            center_y - 5,
            center_x + 6,
            center_y + 1,
            color,
        )?;

        Ok(())
    }

    fn draw_refresh_icon(
        window: &Window,
        rect: Rect,
        color: u32,
    ) -> rsbr::Result<()> {
        let cx = rect.x + rect.w / 2;
        let cy = rect.y + rect.h / 2;

        // Radius is ~35% of the smaller rectangle side.
        let min_side = if rect.w < rect.h { rect.w } else { rect.h };
        let r = (min_side * 35) / 100;
        if r < 3 {
            return Ok(());
        }

        // Precomputed (cos * 1000, sin * 1000) values for angles from 45° to 315° in 22.5° steps.
        // The Y axis points downward, so the arc goes clockwise.
        const ARC_POINTS: [(i32, i32); 13] = [
            (707, 707),   // 45° (bottom-right, arc start)
            (383, 924),   // 67.5°
            (0, 1000),    // 90° (bottom)
            (-383, 924),  // 112.5°
            (-707, 707),  // 135° (bottom-left)
            (-924, 383),  // 157.5°
            (-1000, 0),   // 180° (left)
            (-924, -383), // 202.5°
            (-707, -707), // 225° (top-left)
            (-383, -924), // 247.5°
            (0, -1000),   // 270° (top)
            (383, -924),  // 292.5°
            (707, -707),  // 315° (top-right, arc end)
        ];

        // Helper closure to scale a coordinate with proper integer rounding.
        let scale = |val: i32| -> i32 {
            if val >= 0 {
                (val * r + 500) / 1000
            } else {
                (val * r - 500) / 1000
            }
        };

        let mut prev_x = cx + scale(ARC_POINTS[0].0);
        let mut prev_y = cy + scale(ARC_POINTS[0].1);

        // Draw the arc segment by segment.
        let mut i = 1;
        while i < ARC_POINTS.len() {
            let curr_x = cx + scale(ARC_POINTS[i].0);
            let curr_y = cy + scale(ARC_POINTS[i].1);

            window.draw_line(prev_x, prev_y, curr_x, curr_y, color)?;

            prev_x = curr_x;
            prev_y = curr_y;
            i += 1;
        }

        // Coordinates of the arc tip (315°).
        let tip_x = prev_x;
        let tip_y = prev_y;

        // Arrow wing length (~45% of radius, minimum 3 pixels).
        let arrow_len = if (r * 45) / 100 < 3 { 3 } else { (r * 45) / 100 };

        // Arrow wings angled at ~35° from the tangent (tangent at 315° points toward 45°):
        // 1) First wing points almost left (190°): cos = -985, sin = -174
        // 2) Second wing points almost down (260°): cos = -174, sin = 985
        let scale_arrow = |val: i32| -> i32 {
            if val >= 0 {
                (val * arrow_len + 500) / 1000
            } else {
                (val * arrow_len - 500) / 1000
            }
        };

        let w1_x = tip_x + scale_arrow(-985);
        let w1_y = tip_y + scale_arrow(-174);

        let w2_x = tip_x + scale_arrow(-174);
        let w2_y = tip_y + scale_arrow(985);

        // Draw the arrowhead.
        window.draw_line(tip_x, tip_y, w1_x, w1_y, color)?;
        window.draw_line(tip_x, tip_y, w2_x, w2_y, color)?;

        Ok(())
    }



    // -------------------------------------------------------------------------
    // Sidebar
    // -------------------------------------------------------------------------

    fn draw_sidebar(
        window: &Window,
        app: &FileManagerApp,
        layout: &Layout,
    ) -> rsbr::Result<()> {
        Self::fill_rect(
            window,
            layout.sidebar,
            THEME.sidebar,
        )?;

        Self::fill_rect(
            window,
            Rect::new(
                layout.sidebar.right(),
                0,
                1,
                layout.window.h,
            ),
            THEME.divider,
        )?;

        let config = layout.config;

        window.draw_text(
            config.sidebar_heading_x,
            config.sidebar_heading_y,
            "LOCATIONS",
            THEME.muted,
        )?;

        for (index, (label, path)) in ROOTS.iter().enumerate() {
            let rect = layout.root_item_rect(index);

            let active = app.is_root_active(path);
            let hovered = app.navigation.hovered == HoverTarget::Root(index);

            if active || hovered {
                let color = if active {
                    THEME.accent_soft
                } else {
                    THEME.card
                };

                Self::fill_rect(window, rect, color)?;

                if active {
                    Self::fill_rect(
                        window,
                        Rect::new(
                            rect.x,
                            rect.y + 5,
                            3,
                            (rect.h - 10).max(0),
                        ),
                        THEME.accent,
                    )?;
                }
            }

            let text_color = if active || hovered {
                THEME.accent
            } else {
                THEME.text
            };

            window.draw_text(
                rect.x + 18,
                rect.y + 11,
                label,
                text_color,
            )?;
        }

        Ok(())
    }

    // -------------------------------------------------------------------------
    // Header and toolbar
    // -------------------------------------------------------------------------

    fn draw_header(
        window: &Window,
        app: &FileManagerApp,
        layout: &Layout,
    ) -> rsbr::Result<()> {
        Self::fill_rect(
            window,
            layout.header,
            THEME.header,
        )?;

        Self::fill_rect(
            window,
            Rect::new(
                layout.header.x,
                layout.header.bottom() - 1,
                layout.header.w,
                1,
            ),
            THEME.divider,
        )?;

        window.draw_text(
            layout.header.x + 20,
            18,
            "FILES",
            THEME.text,
        )?;

        Self::draw_toolbar(window, app, layout)?;

        Ok(())
    }

    fn draw_toolbar(
        window: &Window,
        app: &FileManagerApp,
        layout: &Layout,
    ) -> rsbr::Result<()> {
        Self::fill_rect(
            window,
            layout.toolbar,
            THEME.sidebar,
        )?;

        if layout.toolbar.is_visible() {
            window.draw_rect(
                layout.toolbar.x,
                layout.toolbar.y,
                layout.toolbar.w as u32,
                layout.toolbar.h as u32,
                THEME.card_border,
            )?;
        }

        // Current directory path.
        if layout.path_area.w >= 8 && layout.path_area.h > 0 {
            window.draw_text(
                layout.path_area.x + 12,
                layout.path_area.y + 9,
                app.filesystem.path(),
                THEME.muted,
            )?;
        }

        Self::fill_rect(
            window,
            Rect::new(
                layout.path_area.right(),
                layout.toolbar.y,
                1,
                layout.toolbar.h,
            ),
            THEME.card_border,
        )?;

        // Parent button.
        let parent_hovered =
            app.navigation.hovered == HoverTarget::Parent;

        Self::draw_icon_button(
            window,
            layout.parent_button,
            parent_hovered,
        )?;

        let parent_color = if parent_hovered {
            THEME.accent
        } else {
            THEME.text
        };

        Self::draw_parent_icon(
            window,
            layout.parent_button,
            parent_color,
        )?;

        // Divider between buttons.
        Self::fill_rect(
            window,
            Rect::new(
                layout.parent_button.right(),
                layout.toolbar.y,
                1,
                layout.toolbar.h,
            ),
            THEME.card_border,
        )?;

        // Refresh button.
        let refresh_hovered =
            app.navigation.hovered == HoverTarget::Refresh;

        Self::draw_icon_button(
            window,
            layout.refresh_button,
            refresh_hovered,
        )?;

        let refresh_color = if refresh_hovered {
            THEME.accent
        } else {
            THEME.text
        };

        Self::draw_refresh_icon(
            window,
            layout.refresh_button,
            refresh_color,
        )?;

        Ok(())
    }

    // -------------------------------------------------------------------------
    // File grid
    // -------------------------------------------------------------------------

    fn draw_grid(
        window: &Window,
        app: &FileManagerApp,
        layout: &Layout,
    ) -> rsbr::Result<()> {
        let grid = &layout.grid;

        let columns = grid.columns;
        let first_row = app.navigation.scroll_row;

        let total_rows = grid.total_rows(app.filesystem.count());

        let last_row = (first_row + grid.visible_rows)
            .min(total_rows);

        for row in first_row..last_row {
            for column in 0..columns {
                let index = row * columns + column;

                if index >= app.filesystem.count() {
                    break;
                }

                let rect = match grid.card_rect(
                    index,
                    app.navigation.scroll_row,
                ) {
                    Some(rect) => rect,
                    None => continue,
                };

                Self::draw_card(window, app, index, rect)?;
            }
        }

        Ok(())
    }

    fn draw_card(
        window: &Window,
        app: &FileManagerApp,
        index: usize,
        rect: Rect,
    ) -> rsbr::Result<()> {
        let item = match app.filesystem.item(index) {
            Some(item) => item,
            None => return Ok(()),
        };

        let hovered = app.navigation.hovered == HoverTarget::Item(index);
        let selected = app.navigation.selected == Some(index);

        let background = if hovered {
            THEME.card_hover
        } else if selected {
            THEME.card_selected
        } else {
            THEME.card
        };

        let border = if selected {
            THEME.accent
        } else {
            THEME.card_border
        };

        Self::fill_rect(window, rect, background)?;

        window.draw_rect(
            rect.x,
            rect.y,
            rect.w as u32,
            rect.h as u32,
            border,
        )?;

        match item.kind {
            ItemKind::Directory => {
                Self::draw_folder_icon(
                    window,
                    rect.x + 31,
                    rect.y + 13,
                )?;
            }

            ItemKind::File | ItemKind::Other => {
                Self::draw_file_icon(
                    window,
                    rect.x + 37,
                    rect.y + 11,
                )?;
            }
        }

        let label = Self::short_label(item.name_str());

        let label_width = (label.len() as i32 * 8)
            .min(rect.w - 8);

        let label_x = rect.x
            + ((rect.w - label_width) / 2).max(4);

        let label_color = if hovered || selected {
            THEME.accent
        } else {
            THEME.text
        };

        window.draw_text(
            label_x,
            rect.y + 78,
            label,
            label_color,
        )
    }

    // -------------------------------------------------------------------------
    // Footer
    // -------------------------------------------------------------------------

    fn draw_footer(
        window: &Window,
        app: &FileManagerApp,
        layout: &Layout,
    ) -> rsbr::Result<()> {
        if !layout.footer.is_visible() {
            return Ok(());
        }

        Self::fill_rect(
            window,
            Rect::new(
                layout.footer.x,
                layout.footer.y,
                layout.footer.w,
                1,
            ),
            THEME.divider,
        )?;

        let text_y = layout.footer.y + 9;

        window.draw_text(
            layout.footer.x + 20,
            text_y,
            app.status,
            THEME.muted,
        )?;

        window.draw_text(
            (layout.window.w - 154).max(layout.sidebar.right() + 20),
            text_y,
            "Wheel: scroll",
            THEME.muted,
        )
    }

    // -------------------------------------------------------------------------
    // Icons
    // -------------------------------------------------------------------------

    fn draw_folder_icon(
        window: &Window,
        x: i32,
        y: i32,
    ) -> rsbr::Result<()> {
        window.fill_rect(
            x + 3,
            y + 4,
            21,
            9,
            THEME.folder_dark,
        )?;

        window.fill_rect(
            x,
            y + 10,
            46,
            29,
            THEME.folder,
        )?;

        window.draw_rect(
            x,
            y + 10,
            46,
            29,
            THEME.folder_dark,
        )?;

        window.draw_line(
            x + 3,
            y + 14,
            x + 42,
            y + 14,
            THEME.folder_highlight,
        )?;

        Ok(())
    }

    fn draw_file_icon(
        window: &Window,
        x: i32,
        y: i32,
    ) -> rsbr::Result<()> {
        window.fill_rect(
            x + 2,
            y + 1,
            34,
            42,
            THEME.file,
        )?;

        window.draw_rect(
            x + 2,
            y + 1,
            34,
            42,
            THEME.card_border,
        )?;

        window.draw_line(
            x + 9,
            y + 14,
            x + 29,
            y + 14,
            THEME.file_line,
        )?;

        window.draw_line(
            x + 9,
            y + 21,
            x + 29,
            y + 21,
            THEME.file_line,
        )?;

        window.draw_line(
            x + 9,
            y + 28,
            x + 25,
            y + 28,
            THEME.file_line,
        )?;

        Ok(())
    }

    // -------------------------------------------------------------------------
    // Text helpers
    // -------------------------------------------------------------------------

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
}

// =============================================================================
// Entry point
// =============================================================================

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let window = match Window::create(
        WINDOW_X,
        WINDOW_Y,
        WINDOW_WIDTH as u32,
        WINDOW_HEIGHT as u32,
        Some("Files"),
        FLAG_HAS_TITLE
            | FLAG_HAS_CLOSE
            | FLAG_HAS_MINMAX
            | FLAG_RESIZABLE
            | FLAG_MOVABLE,
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