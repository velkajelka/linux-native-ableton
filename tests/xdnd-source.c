// A file drag from outside Live, as a Wayland file manager's drag reaches
// Live through Xwayland: XdndEnter (version 5, the file manager's types in
// XdndTypeList), XdndPosition with CurrentTime and XdndActionCopy at each
// step, then XdndDrop or XdndLeave. It serves text/uri-list (and text/plain,
// UTF8_STRING, TARGETS) from XdndSelection and logs each request. Before
// the first step and after the last it runs `import` to save the screen as
// <prefix>-before.png and <prefix>-during.png, serving requests meanwhile.
// For tests/drag-preview.sh.
//
// usage: xdnd-source <window> <file-uri> <x0> <y0> <x1> <y1> <steps> <shot-prefix> drop|leave
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static Display *dpy;
static Window src, target;
static const char *uri;
static int requests;
static double t0;
static Atom aEnter, aPos, aStatus, aLeave, aDrop, aFinished, aSel, aTypeList, aCopy, aUriList, aTargets;

static double now()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6 - t0;
}

static void sendMsg(Atom type, long d1, long d2, long d3, long d4)
{
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = target;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = src;
    e.xclient.data.l[1] = d1;
    e.xclient.data.l[2] = d2;
    e.xclient.data.l[3] = d3;
    e.xclient.data.l[4] = d4;
    XSendEvent(dpy, target, False, NoEventMask, &e);
    XFlush(dpy);
}

static void serve(XSelectionRequestEvent *r)
{
    char *t = XGetAtomName(dpy, r->target);
    XEvent n;
    memset(&n, 0, sizeof n);
    n.xselection.type = SelectionNotify;
    n.xselection.requestor = r->requestor;
    n.xselection.selection = r->selection;
    n.xselection.target = r->target;
    n.xselection.time = r->time;
    n.xselection.property = None;
    requests++;
    if (r->target == aTargets) {
        Atom list[] = {aUriList};
        XChangeProperty(dpy, r->requestor, r->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 1);
        n.xselection.property = r->property;
    } else if (r->target == aUriList || !strcmp(t, "text/plain") || !strcmp(t, "UTF8_STRING")) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s\r\n", uri);
        XChangeProperty(dpy, r->requestor, r->property, r->target, 8, PropModeReplace, (unsigned char *)buf,
                        strlen(buf));
        n.xselection.property = r->property;
    }
    printf("%7.3f request #%d %s -> %s\n", now(), requests, t, n.xselection.property ? "served" : "refused");
    fflush(stdout);
    XFree(t);
    XSendEvent(dpy, r->requestor, False, NoEventMask, &n);
    XFlush(dpy);
}

// Handles events for `ms` milliseconds or until a message of type `want`.
static int pump(int ms, Atom want, long *flags)
{
    double end = now() + ms / 1000.0;
    while (now() < end) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == SelectionRequest)
                serve(&e.xselectionrequest);
            else if (e.type == ClientMessage && e.xclient.message_type == want && want != None) {
                if (flags)
                    *flags = e.xclient.data.l[1];
                return 1;
            }
        }
        usleep(2000);
    }
    return 0;
}

static int shot(const char *prefix, const char *tag)
{
    char cmd[4200];
    pump(1500, None, 0); // let Live repaint, serving requests meanwhile
    snprintf(cmd, sizeof cmd, "import -window root '%s-%s.png' 2>/dev/null", prefix, tag);
    if (system(cmd) != 0) {
        fprintf(stderr, "screenshot failed\n");
        return 0;
    }
    printf("%7.3f shot %s\n", now(), tag);
    fflush(stdout);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 10) {
        fprintf(stderr, "usage: xdnd-source <window> <file-uri> <x0> <y0> <x1> <y1> <steps> <shot-prefix> drop|leave\n");
        return 2;
    }
    t0 = now();
    dpy = XOpenDisplay(0);
    if (!dpy)
        return 2;
    target = strtoul(argv[1], 0, 0);
    uri = argv[2];
    int x0 = atoi(argv[3]), y0 = atoi(argv[4]), x1 = atoi(argv[5]), y1 = atoi(argv[6]), steps = atoi(argv[7]);
    const char *prefix = argv[8];
    int drop = !strcmp(argv[9], "drop");
    if (steps < 1)
        steps = 1;

    aEnter = XInternAtom(dpy, "XdndEnter", 0);
    aPos = XInternAtom(dpy, "XdndPosition", 0);
    aStatus = XInternAtom(dpy, "XdndStatus", 0);
    aLeave = XInternAtom(dpy, "XdndLeave", 0);
    aDrop = XInternAtom(dpy, "XdndDrop", 0);
    aFinished = XInternAtom(dpy, "XdndFinished", 0);
    aSel = XInternAtom(dpy, "XdndSelection", 0);
    aTypeList = XInternAtom(dpy, "XdndTypeList", 0);
    aCopy = XInternAtom(dpy, "XdndActionCopy", 0);
    aUriList = XInternAtom(dpy, "text/uri-list", 0);
    aTargets = XInternAtom(dpy, "TARGETS", 0);

    src = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, 0);
    XSetSelectionOwner(dpy, aSel, src, CurrentTime);
    const char *types[] = {"text/uri-list", "application/x-kde4-urilist", "text/x-moz-url", "text/plain",
                           "UTF8_STRING"};
    Atom list[5];
    for (int i = 0; i < 5; ++i)
        list[i] = XInternAtom(dpy, types[i], 0);
    XChangeProperty(dpy, src, aTypeList, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 5);
    XFlush(dpy);

    if (!shot(prefix, "before"))
        return 2;
    sendMsg(aEnter, (5L << 24) | 1, list[0], list[1], list[2]);
    int accepted = 0;
    for (int i = 0; i <= steps; ++i) {
        int x = x0 + (x1 - x0) * i / steps, y = y0 + (y1 - y0) * i / steps;
        long flags = 0;
        sendMsg(aPos, 0, ((long)x << 16) | y, CurrentTime, aCopy);
        if (!pump(3000, aStatus, &flags)) {
            printf("%7.3f no XdndStatus at %d,%d\n", now(), x, y);
            return 2;
        }
        accepted += flags & 1;
        pump(100, None, 0);
    }
    printf("%7.3f moves accepted %d of %d\n", now(), accepted, steps + 1);
    if (!shot(prefix, "during"))
        return 2;
    if (drop) {
        sendMsg(aDrop, 0, CurrentTime, 0, 0);
        if (!pump(5000, aFinished, 0))
            printf("%7.3f no XdndFinished\n", now());
        else
            printf("%7.3f dropped\n", now());
    } else
        sendMsg(aLeave, 0, 0, 0, 0);
    pump(1500, None, 0);
    printf("%7.3f requests %d\n", now(), requests);
    return 0;
}
