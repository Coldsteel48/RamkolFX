// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "fd_wakeup.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace ramkolfx::gui
{

FdWakeup::FdWakeup(int InFd, Uint32 InEventType) : Fd(InFd), EventType(InEventType)
{
    PollFd = fcntl(Fd, F_DUPFD_CLOEXEC, 0);
    if (PollFd < 0)
    {
        // Nothing to poll - IsWatching() stays false, so the owner services
        // the fd unprompted.
        fprintf(stderr, "[ramkolfx-gui] couldn't duplicate fd %d: %s\n", Fd, std::strerror(errno));
        return;
    }
    // Non-blocking both ends: the thread drains the read end without
    // stalling, and a write that finds the pipe full can be dropped - a
    // wakeup is already pending then.
    if (pipe2(WakePipe, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        // Without it there'd be no way to interrupt the thread's poll() on
        // shutdown - don't start one. IsWatching() stays false, so the
        // owner services the fd unprompted.
        fprintf(stderr, "[ramkolfx-gui] pipe2 failed: %s\n", std::strerror(errno));
        WakePipe[0] = WakePipe[1] = -1;
        return;
    }
    // The thread inherits the creating thread's signal mask: start it with
    // every signal blocked, so the kernel always delivers process-directed
    // ones (SIGTERM at logout, SIGINT) to the main thread. Delivered here,
    // SDL's handler would only set its quit flag - this thread's poll()
    // would restart on EINTR and the main thread's SDL_WaitEventTimeout
    // would sleep on, up to its full fallback timeout, before noticing.
    sigset_t AllSignals;
    sigset_t PreviousMask;
    sigfillset(&AllSignals);
    const bool bMaskChanged = pthread_sigmask(SIG_SETMASK, &AllSignals, &PreviousMask) == 0;
    bWatching.store(true, std::memory_order_release);
    try
    {
        Thread = std::thread(&FdWakeup::Run, this);
    }
    catch (const std::system_error& Error)
    {
        // Out of threads, most likely. Not fatal: IsWatching() turns false,
        // so the owner services the fd unprompted.
        fprintf(stderr, "[ramkolfx-gui] couldn't start the fd wakeup thread: %s\n", Error.what());
        bWatching.store(false, std::memory_order_release);
    }
    // Restored either way - left blocked, SIGTERM and SIGINT could never
    // stop the main thread.
    if (bMaskChanged)
    {
        pthread_sigmask(SIG_SETMASK, &PreviousMask, nullptr);
    }
}

FdWakeup::~FdWakeup()
{
    if (Thread.joinable())
    {
        {
            const std::lock_guard Lock(Mutex);
            bStopping = true;
        }
        Condition.notify_one();
        InterruptPoll();
        Thread.join();
    }
    for (const int OwnFd : {WakePipe[0], WakePipe[1], PollFd})
    {
        if (OwnFd >= 0)
        {
            close(OwnFd);
        }
    }
}

void FdWakeup::Arm(bool bInWantWritable)
{
    bool bWasArmed = false;
    {
        const std::lock_guard Lock(Mutex);
        if (bArmed && bWantWritable == bInWantWritable)
        {
            return;
        }
        bWasArmed = bArmed;
        bArmed = true;
        bWantWritable = bInWantWritable;
    }
    Condition.notify_one();
    if (bWasArmed)
    {
        // A poll() may already be running with the old events - restart it.
        InterruptPoll();
    }
}

void FdWakeup::InterruptPoll()
{
    const char Byte = 0;
    // EAGAIN means the pipe is full: a wakeup is pending already.
    while (write(WakePipe[1], &Byte, 1) < 0 && errno == EINTR)
    {
    }
}

void FdWakeup::Run()
{
    Watch();
    bWatching.store(false, std::memory_order_release);
    {
        const std::lock_guard Lock(Mutex);
        if (bStopping)
        {
            return;
        }
    }
    // Gave up: tell the owner, which may be waiting on this with nothing
    // else to wake it soon, and from now on has to service the fd without
    // being told (see IsWatching).
    SDL_Event Event{};
    Event.type = EventType;
    SDL_PushEvent(&Event);
}

void FdWakeup::Watch()
{
    while (true)
    {
        short Events = POLLIN;
        {
            std::unique_lock Lock(Mutex);
            Condition.wait(Lock, [this] { return bArmed || bStopping; });
            if (bStopping)
            {
                return;
            }
            if (bWantWritable)
            {
                Events |= POLLOUT;
            }
        }

        pollfd Fds[2] = {{PollFd, Events, 0}, {WakePipe[0], POLLIN, 0}};
        if (poll(Fds, 2, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return; // see Run
        }
        if (Fds[1].revents & (POLLERR | POLLNVAL))
        {
            return; // can't be interrupted any more - see Run
        }
        if (Fds[1].revents != 0)
        {
            // Stopping, or re-armed with other events: drain the pipe and
            // start over from the current state.
            char Buffer[64];
            while (read(WakePipe[0], Buffer, sizeof(Buffer)) > 0)
            {
            }
            continue;
        }
        if (Fds[0].revents != 0)
        {
            {
                const std::lock_guard Lock(Mutex);
                bArmed = false;
            }
            if (Fds[0].revents & (POLLNVAL | POLLERR | POLLHUP))
            {
                return; // dead socket: it'd only wake again at once - Run tells the owner
            }
            SDL_Event Event{};
            Event.type = EventType;
            SDL_PushEvent(&Event);
        }
    }
}

} // namespace ramkolfx::gui
