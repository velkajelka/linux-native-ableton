// Mouse wheel / trackpad scroll over Max for Live dials, sliders and number boxes
// (LIVE_WHEEL_VALUES=0 turns it off; installed from maxviews.cpp, Live 12 only).
//
// A Max device draws in the Max runtime's own X window. The wheel reaches it
// (JUCE: BoxComponent::mouseWheelMove), but the box only acts on it if its object has a
// "mousewheel" method, which live.dial, live.slider and live.numbox do not: the event
// falls through to the parent component and nothing changes. For those objects the wheel
// is turned into the drag the user would make: a mouse press, a vertical move and a release,
// sent to the Max window under the pointer with XSendEvent, so the real pointer never
// moves and the device handles it as its own drag (undo, automation, modulation). Buttons,
// toggles and menus are left alone: a press on them would click them.
//
// The vtable of BoxComponent is patched at run time (the Max runtime is loaded with the
// first Max device); the box object is at +0x188 of the component (checked on Max 9, Live
// 12.4.7). Fractions of a pixel are carried over so touchpads scroll smoothly.
#include <QGuiApplication>
#include <X11/Xlib.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr int kBoxObject = 0x188;
constexpr int kDragThreshold = 3; // Max ignores a drag of up to 3 px: the move starts past it
constexpr double kPixelsPerStep = 8, kFinePixelsPerStep = 2; // per JUCE wheel delta of 1.0 (a notch is about 0.2)

using WheelFn = void (*)(void *self, const void *event, const void *details);
using ZGetFn = void *(*)(void *object, void *symbol);
using GenSymFn = void *(*)(const char *name);
using ClassNameFn = void *(*)(void *object);

WheelFn originalWheel;
ZGetFn zgetfn;
GenSymFn gensym;
ClassNameFn objectClassName;

bool valueBox(void *object)
{
    // A t_symbol starts with its name.
    auto *symbol = static_cast<const char **>(objectClassName(object));
    const char *name = symbol ? *symbol : nullptr;
    if (!name)
        return false;
    static const char *const known[] = {"live.dial", "live.slider", "live.numbox", "live.gain~",
                                        "dial", "slider", "number", "flonum"};
    for (const char *k : known)
        if (!strcmp(name, k))
            return true;
    return false;
}

// The deepest X window under the pointer, and the pointer in it.
bool windowUnderPointer(Display *d, Window &target, int &wx, int &wy, int &rx, int &ry)
{
    const Window root = DefaultRootWindow(d);
    Window ignoredRoot, child;
    int x, y;
    unsigned mask;
    if (!XQueryPointer(d, root, &ignoredRoot, &child, &rx, &ry, &x, &y, &mask))
        return false;
    target = root;
    wx = rx;
    wy = ry;
    while (child) {
        int nx, ny;
        Window next;
        if (!XTranslateCoordinates(d, root, child, rx, ry, &nx, &ny, &next))
            break;
        target = child;
        wx = nx;
        wy = ny;
        child = next;
    }
    return target != root;
}

void sendDrag(int dy)
{
    static Display *d = XOpenDisplay(nullptr);
    Window target;
    int wx, wy, rx, ry;
    if (!d || !windowUnderPointer(d, target, wx, wy, rx, ry))
        return;
    auto button = [&](int type, int y, unsigned state, long mask) {
        XEvent e;
        memset(&e, 0, sizeof e);
        e.xbutton.type = type;
        e.xbutton.display = d;
        e.xbutton.window = target;
        e.xbutton.root = DefaultRootWindow(d);
        e.xbutton.time = CurrentTime;
        e.xbutton.x = wx;
        e.xbutton.y = y;
        e.xbutton.x_root = rx;
        e.xbutton.y_root = ry + (y - wy);
        e.xbutton.state = state;
        e.xbutton.button = Button1;
        e.xbutton.same_screen = True;
        XSendEvent(d, target, True, mask, &e);
    };
    button(ButtonPress, wy, 0, ButtonPressMask);
    XEvent m;
    memset(&m, 0, sizeof m);
    m.xmotion.type = MotionNotify;
    m.xmotion.display = d;
    m.xmotion.window = target;
    m.xmotion.root = DefaultRootWindow(d);
    m.xmotion.time = CurrentTime;
    m.xmotion.x = wx;
    m.xmotion.y = wy + dy;
    m.xmotion.x_root = rx;
    m.xmotion.y_root = ry + dy;
    m.xmotion.state = Button1Mask;
    m.xmotion.same_screen = True;
    XSendEvent(d, target, True, ButtonMotionMask | PointerMotionMask, &m);
    button(ButtonRelease, wy + dy, Button1Mask, ButtonReleaseMask);
    XFlush(d);
}

void boxWheel(void *self, const void *event, const void *details)
{
    void *object = *reinterpret_cast<void **>(static_cast<char *>(self) + kBoxObject);
    static void *const mousewheel = gensym("mousewheel");
    if (object && !zgetfn(object, mousewheel) && valueBox(object)) {
        static double carry;
        const float deltaY = static_cast<const float *>(details)[1]; // juce::MouseWheelDetails: deltaX, deltaY
        const bool fine = QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        carry += -deltaY * (fine ? kFinePixelsPerStep : kPixelsPerStep);
        const int dy = int(carry);
        if (dy) {
            carry -= dy;
            sendDrag(dy + (dy > 0 ? kDragThreshold : -kDragThreshold));
        }
        return;
    }
    originalWheel(self, event, details);
}

} // namespace

// True once the Max runtime is loaded and its box component is patched.
bool maxWheelInstall()
{
    static bool done;
    if (done)
        return true;
    const char *off = getenv("LIVE_WHEEL_VALUES");
    if (off && *off == '0')
        return done = true;
    auto **vt = static_cast<void **>(dlsym(RTLD_DEFAULT, "_ZTV12BoxComponent"));
    void *wheel = dlsym(RTLD_DEFAULT, "_ZN12BoxComponent14mouseWheelMoveERKN4juce10MouseEventERKNS0_17MouseWheelDetailsE");
    zgetfn = reinterpret_cast<ZGetFn>(dlsym(RTLD_DEFAULT, "zgetfn"));
    gensym = reinterpret_cast<GenSymFn>(dlsym(RTLD_DEFAULT, "gensym"));
    objectClassName = reinterpret_cast<ClassNameFn>(dlsym(RTLD_DEFAULT, "object_classname"));
    if (!vt || !wheel || !zgetfn || !gensym || !objectClassName)
        return false; // the Max runtime is not loaded yet
    for (int i = 0; i < 64; ++i) {
        if (vt[i] != wheel)
            continue;
        const long page = sysconf(_SC_PAGESIZE);
        void *start = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&vt[i]) & ~uintptr_t(page - 1));
        if (mprotect(start, page, PROT_READ | PROT_WRITE))
            break;
        originalWheel = reinterpret_cast<WheelFn>(wheel);
        vt[i] = reinterpret_cast<void *>(&boxWheel);
        break;
    }
    return done = true;
}
