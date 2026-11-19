// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "x11_window.hpp"

#include <SDL.h>

// SDL_syswm.h includes Xlib's headers for its X11 members, so it only
// builds where those are installed - without them (a minimal build root),
// this compiles to a no-op rather than failing the build.
#if __has_include(<X11/Xlib.h>)
#include <SDL_syswm.h>
#define RAMKOLFX_HAVE_X11_HEADERS 1
#endif

#include <type_traits>

#include <dlfcn.h>

namespace ramkolfx::gui
{

#if defined(RAMKOLFX_HAVE_X11_HEADERS) && defined(SDL_VIDEO_DRIVER_X11) && !defined(SDL2COMPAT_DISABLE_X11)
#define RAMKOLFX_X11_WINDOW_SUPPORTED 1

namespace
{

// The libX11 entry points used here, from the copy SDL's X11 backend
// already loaded (RTLD_NOLOAD: never load a second one). Resolved once, on
// first use - which only happens once a window is known to be on X11, so
// libX11 is loaded by then. The handle is kept for the process lifetime.
struct X11Functions
{
    using InternAtomFn = Atom (*)(Display*, const char*, Bool);
    using SendEventFn = int (*)(Display*, ::Window, Bool, long, XEvent*); // returns Xlib's Status

    int (*MapWindow)(Display*, ::Window) = nullptr;
    InternAtomFn InternAtom = nullptr;
    SendEventFn SendEvent = nullptr;
    ::Window (*RootOf)(Display*) = nullptr;
    int (*Flush)(Display*) = nullptr;

    static const X11Functions* Get()
    {
        static const X11Functions Functions = Resolve();
        return Functions.Flush ? &Functions : nullptr;
    }

private:
    static X11Functions Resolve()
    {
        X11Functions Result;
        void* const Lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
        if (!Lib)
        {
            return Result;
        }
        const auto Load = [Lib](auto& Fn, const char* Name)
        { Fn = reinterpret_cast<std::remove_reference_t<decltype(Fn)>>(dlsym(Lib, Name)); };
        Load(Result.MapWindow, "XMapWindow");
        Load(Result.InternAtom, "XInternAtom");
        Load(Result.SendEvent, "XSendEvent");
        Load(Result.RootOf, "XDefaultRootWindow");
        Load(Result.Flush, "XFlush");
        if (!Result.MapWindow || !Result.InternAtom || !Result.SendEvent || !Result.RootOf || !Result.Flush)
        {
            Result.Flush = nullptr; // marks the set unusable
        }
        return Result;
    }
};

// The window's X11 handles plus the functions to use on them - empty
// (false) when the window isn't on X11.
struct X11Target
{
    explicit X11Target(SDL_Window* SdlWindow)
    {
        SDL_SysWMinfo Info{};
        SDL_VERSION(&Info.version);
        if (SDL_GetWindowWMInfo(SdlWindow, &Info) && Info.subsystem == SDL_SYSWM_X11)
        {
            X = X11Functions::Get();
            Dpy = Info.info.x11.display;
            Win = Info.info.x11.window;
        }
    }
    explicit operator bool() const { return X != nullptr; }

    // An EWMH client message about Win, sent to the root window where the
    // window manager listens for them.
    void SendToRoot(const char* MessageType, long Data0, long Data1, long Data2, long Data3) const
    {
        XEvent Event{};
        Event.xclient.type = ClientMessage;
        Event.xclient.window = Win;
        Event.xclient.message_type = X->InternAtom(Dpy, MessageType, False);
        Event.xclient.format = 32;
        Event.xclient.data.l[0] = Data0;
        Event.xclient.data.l[1] = Data1;
        Event.xclient.data.l[2] = Data2;
        Event.xclient.data.l[3] = Data3;
        X->SendEvent(Dpy, X->RootOf(Dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &Event);
    }

    long AtomOf(const char* Name) const { return static_cast<long>(X->InternAtom(Dpy, Name, False)); }

    const X11Functions* X = nullptr;
    Display* Dpy = nullptr;
    ::Window Win = 0;
};

// EWMH source indications.
constexpr long SourceApplication = 1;
constexpr long SourcePager = 2; // a direct user request

} // namespace
#endif

bool SetX11WindowSkipTaskbar(SDL_Window* SdlWindow, bool bSkip)
{
#ifdef RAMKOLFX_X11_WINDOW_SUPPORTED
    const X11Target Target(SdlWindow);
    if (!Target)
    {
        return false;
    }
    // _NET_WM_STATE takes at most two states per message.
    const long Action = bSkip ? 1 : 0; // _NET_WM_STATE_ADD / _REMOVE
    Target.SendToRoot("_NET_WM_STATE", Action, Target.AtomOf("_NET_WM_STATE_SKIP_TASKBAR"),
                      Target.AtomOf("_NET_WM_STATE_SKIP_PAGER"), SourceApplication);
    Target.SendToRoot("_NET_WM_STATE", Action, Target.AtomOf("_KDE_NET_WM_STATE_SKIP_SWITCHER"), 0, SourceApplication);
    Target.X->Flush(Target.Dpy);
    return true;
#else
    (void)SdlWindow;
    (void)bSkip;
    return false;
#endif
}

bool DeiconifyX11Window(SDL_Window* SdlWindow)
{
#ifdef RAMKOLFX_X11_WINDOW_SUPPORTED
    const X11Target Target(SdlWindow);
    if (!Target)
    {
        return false;
    }
    Target.X->MapWindow(Target.Dpy, Target.Win);
    Target.SendToRoot("_NET_ACTIVE_WINDOW", SourcePager, CurrentTime, 0, 0);
    Target.X->Flush(Target.Dpy);
    return true;
#else
    (void)SdlWindow;
    return false;
#endif
}

bool IsX11WindowControlBuilt()
{
#ifdef RAMKOLFX_X11_WINDOW_SUPPORTED
    return true;
#else
    return false;
#endif
}

} // namespace ramkolfx::gui
