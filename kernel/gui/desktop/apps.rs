//! Built-in application catalogue and launcher.

use core::ffi::c_char;

#[derive(Copy, Clone, PartialEq, Eq)]
enum AppGroup {
    Applications,
    Office,
    System,
}

#[derive(Copy, Clone)]
struct AppEntry {
    name: &'static str,
    path: &'static [u8],
    icon_id: i32,
    group: AppGroup,
}

const APPS: [AppEntry; 11] = [
    AppEntry { name: "Calculator",     path: b"/apps/gcalc\0",    icon_id: 4,  group: AppGroup::Applications },
    AppEntry { name: "Text Editor",    path: b"/apps/gedit\0",    icon_id: 3,  group: AppGroup::Applications },
    AppEntry { name: "File Manager",   path: b"/apps/gfiles\0",   icon_id: 2,  group: AppGroup::Applications },
    AppEntry { name: "Terminal",       path: b"/apps/gterm\0",    icon_id: 1,  group: AppGroup::Applications },
    AppEntry { name: "Web Browser",    path: b"/apps/gbrowser\0", icon_id: 9,  group: AppGroup::Applications },
    AppEntry { name: "Music Player",   path: b"/apps/gaudio\0",   icon_id: 8,  group: AppGroup::Applications },
    AppEntry { name: "Office",         path: b"/apps/aoffice\0",  icon_id: 11, group: AppGroup::Office },
    AppEntry { name: "System Monitor", path: b"/apps/gsysmon\0",  icon_id: 5,  group: AppGroup::System },
    AppEntry { name: "Task Manager",   path: b"/apps/gtaskmgr\0", icon_id: 7,  group: AppGroup::System },
    AppEntry { name: "USB Manager",    path: b"/apps/gusb\0",     icon_id: 10, group: AppGroup::System },
    AppEntry { name: "About",          path: b"/apps/gabout\0",   icon_id: 6,  group: AppGroup::System },
];

const GROUPS: [(AppGroup, &str); 3] = [
    (AppGroup::Applications, "Applications"),
    (AppGroup::Office, "Office"),
    (AppGroup::System, "System"),
];

#[derive(Copy, Clone)]
pub(crate) enum MenuRow {
    Header(&'static str),
    App(usize),
}

pub(crate) fn menu_row_count() -> usize {
    let mut count = 0;
    for (group, _) in GROUPS {
        let in_group = APPS.iter().filter(|app| app.group == group).count();
        if in_group != 0 {
            count += in_group + 1;
        }
    }
    count
}

pub(crate) fn menu_row(row: usize) -> Option<MenuRow> {
    let mut current = 0usize;
    for (group, title) in GROUPS {
        let mut first = true;
        for (index, app) in APPS.iter().enumerate() {
            if app.group != group {
                continue;
            }
            if first {
                if current == row {
                    return Some(MenuRow::Header(title));
                }
                current += 1;
                first = false;
            }
            if current == row {
                return Some(MenuRow::App(index));
            }
            current += 1;
        }
    }
    None
}

pub(crate) fn app_name(index: usize) -> Option<&'static str> {
    APPS.get(index).map(|app| app.name)
}

pub(crate) fn app_by_icon_id(icon_id: i32) -> Option<usize> {
    APPS.iter().position(|app| app.icon_id == icon_id)
}

pub(crate) fn launch_index(index: usize) {
    let Some(app) = APPS.get(index) else { return; };
    launch(app);
}

pub(crate) fn launch_by_icon(icon_id: i32) {
    if let Some(index) = app_by_icon_id(icon_id) {
        launch_index(index);
    }
}

fn launch(app: &AppEntry) {
    extern "C" {
        fn process_spawn(path: *const c_char) -> i64;
    }

    let result = unsafe { process_spawn(app.path.as_ptr().cast::<c_char>()) };
    if result < 0 {
        super::notifications::notify_text("Launch failed", 0x00c0_4040, 2500);
    }
}
