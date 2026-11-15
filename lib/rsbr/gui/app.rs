//! Application loop: the framework owns the dirty flag.

use crate::gui::{Window, Event};

/// What `update` decided.
pub enum Flow {
    /// Event consumed, nothing visible changed.
    Ignore,
    /// Event consumed, visible state changed.
    Changed,
    /// Event consumed, visible state changed, and the app wants to quit.
    Quit,
}

impl Flow {
    pub fn changed(self) -> bool {
        matches!(self, Flow::Changed | Flow::Quit)
    }
    pub fn quit(self) -> bool {
        matches!(self, Flow::Quit)
    }
}

/// One GUI application.
///
/// `update` may mutate `self`; `render` may not.  That is enforced by
/// the trait: `render` takes `&self`, so the borrow checker rejects
/// any attempt to mutate application state from the draw path.
pub trait App {
    /// Handle one event.  Return [`Flow`] describing what changed.
    ///
    /// **Ignore `EVT_MOUSE_MOVE` unless hover actually changed.**  If
    /// you return `Flow::Changed` on every move, the framework will
    /// repaint on every move -- which is the bug the whole dirty-flag
    /// machinery exists to prevent.  The framework cannot detect this
    /// for you; it can only enforce that you *chose*.
    fn update(&mut self, ev: &Event) -> Flow;

    /// Draw the current state.  Called by the framework only after
    /// `update` returned `Flow::Changed` or `Flow::Quit`, and on the
    /// first frame.
    fn render(&self, w: &Window) -> crate::Result<()>;
}

/// Run the app loop.  Blocks until the window is closed, `update`
/// returns [`Flow::Quit`], or the window is destroyed.
pub fn run<A: App>(mut app: A, w: Window) -> crate::Result<()> {
    render_frame(&app, &w)?;

    loop {
        let ev = match w.wait() {
            Ok(e) => e,
            Err(_) => break,   // window destroyed / closed
        };

        match app.update(&ev) {
            Flow::Ignore => {}
            Flow::Changed => {
                render_frame(&app, &w)?;
            }
            Flow::Quit => {
                render_frame(&app, &w)?;
                break;
            }
        }
    }
    Ok(())
}

fn render_frame<A: App>(app: &A, w: &Window) -> crate::Result<()> {
    w.frame_begin()?;
    let render_result = app.render(w);
    let end_result = w.frame_end();
    render_result?;
    end_result?;
    w.invalidate()?;
    Ok(())
}