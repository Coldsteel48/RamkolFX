// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <SDL.h>

namespace ramkolfx::gui
{

// Lets SDL_WaitEvent also wake on a file descriptor SDL knows nothing about
// (the tray's D-Bus socket), so the hidden-in-tray loop can sleep until
// something actually happens instead of polling on a timer - and lets the
// drawing loop skip the fd's I/O on frames where it has nothing to offer.
// A helper thread poll()s the fd and pushes an SDL event of the given type
// when it turns readable - or writable, when the owner has output it
// couldn't write yet.
//
// One event per Arm(): the fd stays readable until its owner drains it, so
// the thread waits to be re-armed rather than spinning. Call Arm() after
// draining (e.g. after Tray::Dispatch) and before waiting - data arriving
// in between still wakes the wait, since poll() then returns immediately.
// Pass bWantWritable while the owner has queued output (a write that hit
// EAGAIN), so the wait also ends once the socket can take more; re-arming
// with a different value takes effect even while a poll() is running.
//
// The thread polls its own duplicate of the fd, so the owner closing the
// original (say, on losing the bus) can't leave it watching a closed fd
// number - or one already reused for something else. The duplicate keeps
// the socket open until this is destroyed; GetFd() still reports the
// original number, for comparing against the owner's current fd.
class FdWakeup
{
public:
    FdWakeup(int Fd, Uint32 EventType);
    ~FdWakeup();

    FdWakeup(const FdWakeup&) = delete;
    FdWakeup& operator=(const FdWakeup&) = delete;

    int GetFd() const { return Fd; }
    void Arm(bool bWantWritable = false);

    // Whether the thread is running and will push an event once the fd is
    // ready. False if it couldn't be started or has given up (a poll()
    // error, or the fd hung up) - the owner must then service the fd
    // without waiting for an event.
    bool IsWatching() const { return bWatching.load(std::memory_order_acquire); }

private:
    void Run();
    void Watch();
    void InterruptPoll();

    const int Fd;
    const Uint32 EventType;
    // The duplicate of Fd the thread polls (-1 if it couldn't be made).
    int PollFd = -1;
    // Its write end interrupts a poll() in progress, to stop or to pick up
    // a changed bWantWritable.
    int WakePipe[2] = {-1, -1};

    std::mutex Mutex;
    std::condition_variable Condition;
    bool bArmed = false;
    bool bWantWritable = false;
    bool bStopping = false;
    std::atomic<bool> bWatching{false};

    std::thread Thread;
};

} // namespace ramkolfx::gui
