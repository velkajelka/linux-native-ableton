// Plug-in editor title bars in the plug-in's own colour (editorcolour.cpp).
#pragma once
// Same types as Xlib, without its macros (they clash with Qt headers).
typedef struct _XDisplay Display;
typedef unsigned long Window;
class QString;

// A new plug-in editor window that Live owns (window.cpp liveOwnPluginWindow).
void editorColourTrack(Window editor);
// Live named the editor (XStoreName): the plug-in's name is the title up to
// the first "/"; a colour remembered for it is given at once.
void editorColourNamed(Window editor, const char *title);
// The editor was mapped / resized: its colour is read again a little later.
// Windows not tracked are ignored. GUI thread only.
void editorColourMapped(Window editor);
void editorColourResized(Window editor);
// A ConfigureNotify for the editor (from the window manager too): a read
// after it when the size differs from the last one seen.
void editorColourConfigured(Window editor, int width, int height);
// The editor is gone: pending reads for it do nothing.
void editorColourForget(Window editor);
// Reads the colour along the top edge of the editor's plug-in view and gives
// the editor a title bar in it. False, changing nothing, when the editor is
// not shown, is gone or shows nothing but black there (not drawn yet).
bool editorColourApply(Display *dpy, Window editor);
// The plug-in's name for a tracked editor (the title up to the first "/");
// empty when the editor is not tracked or not named yet. GUI thread only.
QString editorPluginName(Window editor);
// True for an editor window.cpp tracks (from its creation until it is destroyed). GUI thread only.
bool editorTracked(Window editor);
