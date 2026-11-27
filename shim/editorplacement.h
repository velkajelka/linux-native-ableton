// Plug-in editor windows open centred over Live's main window the first time,
// then where that plug-in's window was last left (editorplacement.cpp).
#pragma once
// Same types as Xlib, without its macros (they clash with Qt headers).
typedef struct _XDisplay Display;
typedef unsigned long Window;

// Live is about to show the editor: it is moved to the place remembered for
// its plug-in, else centred over the window it belongs to, kept on the
// screen. Windows not tracked or not named yet, and Live 11's, are left
// alone. GUI thread only.
void editorPlacementMapping(Display *dpy, Window editor);
// Live moves and resizes a placed editor (the plug-in resized its view) with
// the position it last knew: x and y become where the editor is now, or,
// while it is not shown, where it was placed or last written down.
void editorPlacementResizing(Display *dpy, Window editor, int &x, int &y);
// A ConfigureNotify for the editor: where it is gets written down once it has
// stayed there a moment.
void editorPlacementConfigured(Window editor);
// The editor is being hidden or closed: where it is gets written down now.
void editorPlacementSave(Display *dpy, Window editor);
// The editor is gone: nothing more is written down for it.
void editorPlacementForget(Window editor);
