// Dragging files out on X11: the source side of XDND
// (freedesktop.org/wiki/Specifications/XDND), version 5.
//
// No pointer grab is taken. The drag starts with the left button still held
// from the in-app drag, so the window's implicit grab already delivers every
// motion and the release, wherever the pointer is; SDL's event hook sees
// them on the way to SDL. A grab of our own would fail anyway: SDL reads the
// mouse through XInput2, and the server refuses a core grab while an XInput2
// implicit grab is active.
//
// The implicit grab has to survive the drag, though. The ImGui backend turns
// SDL's mouse auto-capture off and releases its own capture as soon as the
// in-app drag is cleared, and SDL releases a capture with XUngrabPointer,
// which ends the implicit grab too: the release then goes to whatever window
// is under the pointer, and SDL is left believing the button is still down.
// Auto-capture is switched back on for the length of the drag, which keeps
// SDL's capture, and so the grab, until SDL itself sees the button go up.
//
// Under XWayland the same messages reach Wayland programs: Mutter notices
// the XdndSelection owner change, carries the drag across, and stands in as
// the XDND target over any Wayland window.

#include "platform/linux/DragOut.h"

#include <SDL3/SDL.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/keysym.h>

namespace platform::x11dnd
{
namespace
{
constexpr long kXdndVersion = 5;
constexpr unsigned long long kStatusTimeoutMs = 5000;

struct Atoms
{
    Atom aware, proxy, selection, typeList, targets, uriList;
    Atom enter, position, status, leave, drop, finished;
    Atom actionCopy, actionMove, actionLink;
};

struct Drag
{
    bool        active = false;
    std::string uris;

    Window target = None;     // the XdndAware window under the pointer
    Window sendTo = None;     // where its messages go: itself, or its XdndProxy
    long   version = 0;

    bool accepted = false;    // the target's last XdndStatus
    Atom action = None;

    // One XdndPosition may be outstanding at a time; later motion waits.
    bool awaitingStatus = false;
    bool havePending = false;
    int  pendingX = 0, pendingY = 0;
    Time pendingTime = CurrentTime;
    unsigned pendingMods = 0;

    bool released = false;
    bool dropOnStatus = false;   // released while a position was outstanding
    Time releaseTime = CurrentTime;
    unsigned long long releasedAt = 0;
    bool dropSent = false;
    unsigned long long droppedAt = 0;
};

Display* display = nullptr;
Window   source = None;   // owns XdndSelection and receives the replies
int      xiOpcode = -1;
KeyCode  escapeKey = 0;
Atoms    atoms{};
Drag     drag;
bool        haveOutcome = false;
DragOutcome outcome = DragOutcome::Cancelled;

// The auto-capture hint as it was before the drag, while the drag holds it.
bool        captureHeld = false;
std::string captureHint;

void HoldCapture()
{
    if (captureHeld) return;
    const char* hint = SDL_GetHint(SDL_HINT_MOUSE_AUTO_CAPTURE);
    captureHint = hint ? hint : "";
    captureHeld = true;
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "1");
}

// From the main loop only: restoring the hint can release SDL's capture,
// which must not happen inside SDL's own event dispatch. Nor while SDL still
// has the button down (Escape ends the drag before the release): that would
// end the implicit grab before the release reaches SDL.
void ReleaseCapture()
{
    if (!captureHeld || (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)) return;
    captureHeld = false;
    if (captureHint.empty()) SDL_ResetHint(SDL_HINT_MOUSE_AUTO_CAPTURE);
    else SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, captureHint.c_str());
}

// Xlib's default error handler exits the process, and the windows a drag
// talks to belong to other programs and can vanish at any moment. Every
// request naming one runs under this trap and is synced, so its error
// arrives here rather than later in SDL's event loop.
bool trappedError = false;
int OnXError(Display*, XErrorEvent*)
{
    trappedError = true;
    return 0;
}

template <class F> bool Trapped(F&& f)
{
    trappedError = false;
    XErrorHandler previous = XSetErrorHandler(OnXError);
    f();
    XSync(display, False);
    XSetErrorHandler(previous);
    return !trappedError;
}

bool ReadWindowProperty(Window w, Atom property, Atom type, unsigned long& value)
{
    Atom actualType = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    bool ok = false;
    Trapped([&] {
        if (XGetWindowProperty(display, w, property, 0, 1, False, type, &actualType, &format, &count, &after, &data) ==
                Success &&
            actualType == type && format == 32 && count >= 1)
            {
            value = reinterpret_cast<unsigned long*>(data)[0];
            ok = true;
            }
    });
    if (data) XFree(data);
    return ok;
}

// The innermost XdndAware window at a root position. Children are walked
// from the root down, so a window manager's frame is passed through to the
// client inside it.
bool FindTarget(int x, int y, Window& target, Window& sendTo, long& version)
{
    const Window root = DefaultRootWindow(display);
    Window w = root;
    for (int depth = 0; depth < 32; ++depth)
        {
        Window child = None;
        int cx = 0, cy = 0;
        bool ok = false;
        Trapped([&] { ok = XTranslateCoordinates(display, root, w, x, y, &cx, &cy, &child); });
        if (!ok || child == None || child == source) return false;
        w = child;
        unsigned long aware = 0;
        if (!ReadWindowProperty(w, atoms.aware, XA_ATOM, aware)) continue;
        target = w;
        version = (long)aware;
        // A proxy counts only when it names itself as its own proxy.
        sendTo = w;
        unsigned long proxy = 0, proxyOfProxy = 0;
        if (ReadWindowProperty(w, atoms.proxy, XA_WINDOW, proxy) &&
            ReadWindowProperty((Window)proxy, atoms.proxy, XA_WINDOW, proxyOfProxy) && proxy == proxyOfProxy)
            sendTo = (Window)proxy;
        return true;
        }
    return false;
}

void SendToTarget(Atom type, long l1, long l2, long l3, long l4)
{
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.display = display;
    e.xclient.window = drag.target;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = (long)source;
    e.xclient.data.l[1] = l1;
    e.xclient.data.l[2] = l2;
    e.xclient.data.l[3] = l3;
    e.xclient.data.l[4] = l4;
    Trapped([&] { XSendEvent(display, drag.sendTo, False, NoEventMask, &e); });
}

void End(DragOutcome result)
{
    drag = Drag{};
    Trapped([&] {
        if (XGetSelectionOwner(display, atoms.selection) == source)
            XSetSelectionOwner(display, atoms.selection, None, CurrentTime);
    });
    haveOutcome = true;
    outcome = result;
}

void SendEnter()
{
    const long version = drag.version < kXdndVersion ? drag.version : kXdndVersion;
    SendToTarget(atoms.enter, version << 24, (long)atoms.uriList, None, None);
}

void SendLeave()
{
    if (drag.target != None) SendToTarget(atoms.leave, 0, 0, 0, 0);
    drag.target = None;
}

// Shift asks for a move, as in every file manager; anything else offers a
// copy, which is all a browser or a chat window can take anyway.
void SendPosition(int x, int y, Time time, unsigned mods)
{
    const Atom action = (mods & ShiftMask) && !(mods & ControlMask) ? atoms.actionMove : atoms.actionCopy;
    SendToTarget(atoms.position, 0, ((long)x << 16) | ((long)y & 0xffff), (long)time, (long)action);
    drag.awaitingStatus = true;
    drag.havePending = false;
}

void SendDrop(Time time)
{
    SendToTarget(atoms.drop, 0, (long)time, 0, 0);
    drag.dropSent = true;
    drag.droppedAt = SDL_GetTicks();
}

void OnMotion(int x, int y, Time time, unsigned mods)
{
    if (drag.released) return;
    Window target = None, sendTo = None;
    long version = 0;
    if (!FindTarget(x, y, target, sendTo, version)) target = None;
    if (target != drag.target)
        {
        SendLeave();
        drag.accepted = false;
        drag.action = None;
        drag.awaitingStatus = false;
        drag.havePending = false;
        if (target != None)
            {
            drag.target = target;
            drag.sendTo = sendTo;
            drag.version = version;
            SendEnter();
            }
        }
    if (drag.target == None) return;
    if (drag.awaitingStatus)
        {
        drag.havePending = true;
        drag.pendingX = x;
        drag.pendingY = y;
        drag.pendingTime = time;
        drag.pendingMods = mods;
        return;
        }
    SendPosition(x, y, time, mods);
}

void OnRelease(Time time)
{
    if (drag.released) return;
    drag.released = true;
    drag.releaseTime = time;
    drag.releasedAt = SDL_GetTicks();
    if (drag.target == None) End(DragOutcome::Cancelled);
    else if (drag.awaitingStatus) drag.dropOnStatus = true;   // decided when the answer comes
    else if (drag.accepted) SendDrop(time);
    else
        {
        SendLeave();
        End(DragOutcome::Cancelled);
        }
}

void OnCancel()
{
    if (drag.released) return;
    SendLeave();
    End(DragOutcome::Cancelled);
}

void OnStatus(const XClientMessageEvent& m)
{
    if ((Window)m.data.l[0] != drag.target || drag.dropSent) return;
    drag.awaitingStatus = false;
    drag.accepted = (m.data.l[1] & 1) != 0;
    drag.action = drag.version >= 2 ? (Atom)m.data.l[4] : atoms.actionCopy;
    if (drag.dropOnStatus)
        {
        if (drag.accepted) SendDrop(drag.releaseTime);
        else
            {
            SendLeave();
            End(DragOutcome::Cancelled);
            }
        return;
        }
    if (drag.havePending) SendPosition(drag.pendingX, drag.pendingY, drag.pendingTime, drag.pendingMods);
}

void OnFinished(const XClientMessageEvent& m)
{
    if ((Window)m.data.l[0] != drag.target || !drag.dropSent) return;
    bool success = true;
    Atom action = drag.action;
    if (drag.version >= 5)
        {
        success = (m.data.l[1] & 1) != 0;
        action = (Atom)m.data.l[2];
        }
    if (!success) End(DragOutcome::Cancelled);
    else if (action == atoms.actionMove) End(DragOutcome::Moved);
    else if (action == atoms.actionLink) End(DragOutcome::Linked);
    else End(DragOutcome::Copied);
}

// The target asking for the data, after the drop or (some programs) while
// the drag is still hovering.
void OnSelectionRequest(const XSelectionRequestEvent& r)
{
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = display;
    reply.xselection.requestor = r.requestor;
    reply.xselection.selection = r.selection;
    reply.xselection.target = r.target;
    reply.xselection.property = None;
    reply.xselection.time = r.time;
    // An obsolete requestor leaves the property out and means the target.
    const Atom property = r.property != None ? r.property : r.target;
    Trapped([&] {
        if (r.target == atoms.targets)
            {
            const Atom offered[] = {atoms.targets, atoms.uriList};
            XChangeProperty(display, r.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(offered), 2);
            reply.xselection.property = property;
            }
        else if (r.target == atoms.uriList)
            {
            XChangeProperty(display, r.requestor, property, atoms.uriList, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(drag.uris.data()), (int)drag.uris.size());
            reply.xselection.property = property;
            }
        XSendEvent(display, r.requestor, False, NoEventMask, &reply);
    });
}

// Runs for every X event before SDL sees it. Pointer events are watched and
// passed on, so SDL's own idea of the mouse, the button above all, stays
// true; only the drag's own traffic, and Escape, stop here.
bool SDLCALL EventHook(void*, XEvent* ev)
{
    if (!drag.active) return true;
    switch (ev->type)
        {
        case GenericEvent:
            {
            if (ev->xcookie.extension != xiOpcode || !ev->xcookie.data) return true;
            const auto* de = static_cast<const XIDeviceEvent*>(ev->xcookie.data);
            switch (ev->xcookie.evtype)
                {
                case XI_Motion:
                    OnMotion((int)de->root_x, (int)de->root_y, de->time, (unsigned)de->mods.effective);
                    return true;
                case XI_ButtonRelease:
                    if (de->detail == 1) OnRelease(de->time);
                    return true;
                case XI_KeyPress:
                    if (de->detail != escapeKey) return true;
                    OnCancel();
                    return false;
                default:
                    return true;
                }
            }
        case MotionNotify:
            OnMotion(ev->xmotion.x_root, ev->xmotion.y_root, ev->xmotion.time, ev->xmotion.state);
            return true;
        case ButtonRelease:
            if (ev->xbutton.button == Button1) OnRelease(ev->xbutton.time);
            return true;
        case KeyPress:
            if (ev->xkey.keycode != escapeKey) return true;
            OnCancel();
            return false;
        case ClientMessage:
            if (ev->xclient.window != source) return true;
            if (ev->xclient.message_type == atoms.status) OnStatus(ev->xclient);
            else if (ev->xclient.message_type == atoms.finished) OnFinished(ev->xclient);
            return false;
        case SelectionRequest:
            if (ev->xselectionrequest.owner != source) return true;
            OnSelectionRequest(ev->xselectionrequest);
            return false;
        case SelectionClear:
            // Someone else started a drag: ours is over.
            if (ev->xselectionclear.window != source) return true;
            SendLeave();
            End(DragOutcome::Cancelled);
            return false;
        default:
            return true;
        }
}
} // namespace

bool Init(SDL_Window* window)
{
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    display = static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    if (!display) return false;

    const char* names[] = {"XdndAware",      "XdndProxy",       "XdndSelection",  "XdndTypeList", "TARGETS",
                           kUriListMime,     "XdndEnter",       "XdndPosition",   "XdndStatus",   "XdndLeave",
                           "XdndDrop",       "XdndFinished",    "XdndActionCopy", "XdndActionMove",
                           "XdndActionLink"};
    Atom a[sizeof(names) / sizeof(names[0])];
    XInternAtoms(display, const_cast<char**>(names), (int)(sizeof(names) / sizeof(names[0])), False, a);
    atoms = Atoms{a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14]};

    int event = 0, error = 0;
    if (!XQueryExtension(display, "XInputExtension", &xiOpcode, &event, &error)) xiOpcode = -1;
    escapeKey = XKeysymToKeycode(display, XK_Escape);

    // Never mapped: owning a selection and receiving client messages need
    // only the window id.
    XSetWindowAttributes attrs{};
    source = XCreateWindow(display, DefaultRootWindow(display), -100, -100, 1, 1, 0, 0, InputOnly, CopyFromParent, 0,
                           &attrs);
    const Atom type = atoms.uriList;
    XChangeProperty(display, source, atoms.typeList, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&type), 1);
    XFlush(display);
    SDL_SetX11EventHook(EventHook, nullptr);
    return true;
}

void Shutdown()
{
    if (!display) return;
    SDL_SetX11EventHook(nullptr, nullptr);
    if (drag.active)
        {
        SendLeave();
        End(DragOutcome::Cancelled);
        }
    if (source != None) XDestroyWindow(display, source);
    XFlush(display);
    source = None;
    display = nullptr;
}

bool Start(const std::string& uris, std::string& error)
{
    if (drag.active)
        {
        error = "A drag to another application is already in progress.";
        return false;
        }

    // The drag rides on the held button. Already up means the user let go
    // in the frame it took to get here: nothing to drag.
    Window root = None, child = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned mods = 0;
    XQueryPointer(display, DefaultRootWindow(display), &root, &child, &rx, &ry, &wx, &wy, &mods);
    if (!(mods & Button1Mask))
        {
        haveOutcome = true;
        outcome = DragOutcome::Cancelled;
        return true;
        }

    XSetSelectionOwner(display, atoms.selection, source, CurrentTime);
    if (XGetSelectionOwner(display, atoms.selection) != source)
        {
        error = "Could not start the drag: the X server refused the drag selection.";
        return false;
        }
    HoldCapture();
    drag = Drag{};
    drag.active = true;
    drag.uris = uris;
    OnMotion(rx, ry, CurrentTime, mods);
    XFlush(display);
    return true;
}

bool InProgress(bool& accepted)
{
    accepted = drag.active && drag.target != None && drag.accepted;
    return drag.active;
}

bool Poll(DragOutcome& result)
{
    // A target that went quiet is not waited on for ever: it either never
    // answers the last position after the release, or never says it has
    // finished with a drop it was given.
    if (drag.active)
        {
        const unsigned long long now = SDL_GetTicks();
        if (drag.dropOnStatus && now - drag.releasedAt > kStatusTimeoutMs)
            {
            SendLeave();
            End(DragOutcome::Cancelled);
            }
        else if (drag.dropSent && now - drag.droppedAt > kDropFinishTimeoutMs)
            End(DragOutcome::Cancelled);
        }
    if (!drag.active) ReleaseCapture();
    if (!haveOutcome) return false;
    haveOutcome = false;
    result = outcome;
    return true;
}

} // namespace platform::x11dnd
