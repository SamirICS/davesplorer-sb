// Dragging files out on Wayland: a wl_data_source handed to
// wl_data_device.start_drag.
//
// SDL owns the seat, the pointer and the data device and exposes none of
// them, so this binds its own on a private event queue. start_drag must
// quote the serial of the button press that began the implicit grab, and
// only a pointer that was listening at the time of the press knows it; that
// is why the pointer is bound at startup rather than when a drag begins.
//
// The data device lives only as long as a drag. Mutter delivers an incoming
// drag to one data device per client, the newest, and a permanent second
// device would take every drop meant for SDL's.

#include "platform/linux/DragOut.h"

#include <SDL3/SDL.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <unistd.h>

namespace platform::wldnd
{
namespace
{
struct State
{
    wl_display*             display = nullptr;
    wl_surface*             surface = nullptr;   // SDL's window
    wl_event_queue*         queue = nullptr;
    wl_registry*            registry = nullptr;
    wl_seat*                seat = nullptr;
    wl_pointer*             pointer = nullptr;
    wl_data_device_manager* manager = nullptr;

    bool     overSurface = false;
    bool     leftDown = false;
    uint32_t pressSerial = 0;

    // The drag in flight.
    wl_data_device* device = nullptr;
    wl_data_source* source = nullptr;
    std::string     uris;
    bool            active = false;
    bool            pointerLeft = false;   // the compositor took the pointer: the drag really started
    bool            accepted = false;
    bool            dropped = false;
    uint32_t        action = 0;
    unsigned long long droppedAt = 0;

    bool        haveOutcome = false;
    DragOutcome outcome = DragOutcome::Cancelled;
};
State s;

void End(DragOutcome result)
{
    if (s.source) wl_data_source_destroy(s.source);
    if (s.device) wl_data_device_release(s.device);
    s.source = nullptr;
    s.device = nullptr;
    s.uris.clear();
    s.active = false;
    s.accepted = false;
    s.dropped = false;
    s.action = 0;
    s.haveOutcome = true;
    s.outcome = result;
    wl_display_flush(s.display);
}

// The receiving program reads the other end of the pipe. If it closes it
// early the write must fail with EPIPE rather than kill the process, without
// changing how the rest of the program handles SIGPIPE.
void WriteAll(int fd, const std::string& data)
{
    sigset_t pipeSet, previous;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, &previous);
    bool broken = false;
    size_t done = 0;
    while (done < data.size())
        {
        const ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else
            {
            broken = n < 0 && errno == EPIPE;
            break;
            }
        }
    if (broken)
        {
        const timespec zero{};
        sigtimedwait(&pipeSet, nullptr, &zero);
        }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

// --- pointer: only the press serial and where the pointer is ---------------

void PointerEnter(void*, wl_pointer*, uint32_t, wl_surface* surface, wl_fixed_t, wl_fixed_t)
{
    s.overSurface = surface != nullptr && surface == s.surface;
}
void PointerLeave(void*, wl_pointer*, uint32_t, wl_surface*)
{
    s.overSurface = false;
    s.leftDown = false;   // the implicit grab, if any, is over
    if (s.active) s.pointerLeft = true;
}
void PointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void PointerButton(void*, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
{
    if (button != BTN_LEFT) return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        {
        s.leftDown = true;
        if (s.overSurface) s.pressSerial = serial;
        return;
        }
    s.leftDown = false;
    // A release that still reaches this surface means the compositor never
    // took the drag over (a refused serial is ignored without an error).
    if (s.active && !s.pointerLeft) End(DragOutcome::Cancelled);
}
void PointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
const wl_pointer_listener pointerListener = {PointerEnter, PointerLeave, PointerMotion, PointerButton, PointerAxis,
                                             nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

void SeatCapabilities(void*, wl_seat* seat, uint32_t caps)
{
    const bool hasPointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (hasPointer && !s.pointer)
        {
        s.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(s.pointer, &pointerListener, nullptr);
        }
    else if (!hasPointer && s.pointer)
        {
        wl_pointer_destroy(s.pointer);
        s.pointer = nullptr;
        s.overSurface = s.leftDown = false;
        }
}
void SeatName(void*, wl_seat*, const char*) {}
const wl_seat_listener seatListener = {SeatCapabilities, SeatName};

void RegistryGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    // Version 1 of the seat is enough for a pointer's enter, leave and
    // button, and keeps the listener above complete.
    if (std::strcmp(interface, wl_seat_interface.name) == 0 && !s.seat)
        {
        s.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        wl_seat_add_listener(s.seat, &seatListener, nullptr);
        }
    // Version 3 brings drag actions and dnd_finished.
    else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0 && version >= 3 && !s.manager)
        s.manager = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
}
void RegistryGlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registryListener = {RegistryGlobal, RegistryGlobalRemove};

// --- the data device: a source only, every offer is declined ---------------

void DeviceDataOffer(void*, wl_data_device*, wl_data_offer* offer) { wl_data_offer_destroy(offer); }
void DeviceEnter(void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) {}
void DeviceLeave(void*, wl_data_device*) {}
void DeviceMotion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void DeviceDrop(void*, wl_data_device*) {}
void DeviceSelection(void*, wl_data_device*, wl_data_offer*) {}
const wl_data_device_listener deviceListener = {DeviceDataOffer, DeviceEnter, DeviceLeave,
                                                DeviceMotion,    DeviceDrop,  DeviceSelection};

// --- the source -------------------------------------------------------------

void SourceTarget(void*, wl_data_source*, const char* mime) { s.accepted = mime != nullptr; }
void SourceSend(void*, wl_data_source*, const char* mime, int32_t fd)
{
    if (mime && std::strcmp(mime, kUriListMime) == 0) WriteAll(fd, s.uris);
    close(fd);
}
void SourceCancelled(void*, wl_data_source*) { End(DragOutcome::Cancelled); }
void SourceDropPerformed(void*, wl_data_source*)
{
    s.dropped = true;
    s.droppedAt = SDL_GetTicks();
}
void SourceFinished(void*, wl_data_source*)
{
    End(s.action & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE ? DragOutcome::Moved : DragOutcome::Copied);
}
void SourceAction(void*, wl_data_source*, uint32_t action) { s.action = action; }
const wl_data_source_listener sourceListener = {SourceTarget,        SourceSend,     SourceCancelled,
                                                SourceDropPerformed, SourceFinished, SourceAction};

void Dispatch()
{
    if (!s.queue) return;
    wl_display_dispatch_queue_pending(s.display, s.queue);
}
} // namespace

bool Init(SDL_Window* window)
{
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    s = State{};
    s.display = static_cast<wl_display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    s.surface = static_cast<wl_surface*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (!s.display || !s.surface) return false;

    // Everything bound here, and every object made from it, lands on the
    // private queue: SDL dispatches only its own, so nothing of ours reaches
    // SDL's handlers, and ours run only when Dispatch() says so.
    s.queue = wl_display_create_queue(s.display);
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(s.display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), s.queue);
    s.registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(s.registry, &registryListener, nullptr);
    wl_display_roundtrip_queue(s.display, s.queue);   // the globals
    wl_display_roundtrip_queue(s.display, s.queue);   // the seat's capabilities
    return true;
}

void Shutdown()
{
    if (!s.display) return;
    if (s.active) End(DragOutcome::Cancelled);
    if (s.pointer) wl_pointer_destroy(s.pointer);
    if (s.seat) wl_seat_destroy(s.seat);
    if (s.manager) wl_data_device_manager_destroy(s.manager);
    if (s.registry) wl_registry_destroy(s.registry);
    wl_display_flush(s.display);
    if (s.queue) wl_event_queue_destroy(s.queue);
    s = State{};
}

bool Start(const std::string& uris, std::string& error)
{
    Dispatch();   // the press may still be waiting in the queue
    if (s.active)
        {
        error = "A drag to another application is already in progress.";
        return false;
        }
    if (!s.manager || !s.seat || !s.pointer)
        {
        error = "This Wayland compositor does not support dragging to other applications. Use Copy and Paste instead.";
        return false;
        }
    // The user let go in the frame it took to get here: nothing to drag.
    if (!s.leftDown || s.pressSerial == 0)
        {
        s.haveOutcome = true;
        s.outcome = DragOutcome::Cancelled;
        return true;
        }

    s.uris = uris;
    s.device = wl_data_device_manager_get_data_device(s.manager, s.seat);
    wl_data_device_add_listener(s.device, &deviceListener, nullptr);
    s.source = wl_data_device_manager_create_data_source(s.manager);
    wl_data_source_add_listener(s.source, &sourceListener, nullptr);
    wl_data_source_offer(s.source, kUriListMime);
    wl_data_source_set_actions(s.source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    wl_data_device_start_drag(s.device, s.source, s.surface, nullptr, s.pressSerial);
    wl_display_flush(s.display);
    s.active = true;
    s.pointerLeft = false;
    s.accepted = false;
    s.dropped = false;
    s.action = 0;
    return true;
}

bool InProgress(bool& accepted)
{
    accepted = s.active && s.accepted;
    return s.active;
}

bool Poll(DragOutcome& result)
{
    Dispatch();
    // A target that never says it has finished with the drop is not waited
    // on for ever.
    if (s.active && s.dropped && SDL_GetTicks() - s.droppedAt > kDropFinishTimeoutMs) End(DragOutcome::Cancelled);
    if (!s.haveOutcome) return false;
    s.haveOutcome = false;
    result = s.outcome;
    return true;
}

} // namespace platform::wldnd
