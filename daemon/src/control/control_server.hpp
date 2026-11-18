// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ramkolfx/protocol.hpp"
#include "ramkolfx/types.hpp"

namespace ramkolfx
{

// Unix domain socket server for the daemon's control protocol (see
// ramkolfx/protocol.hpp for the wire format). Accepts multiple concurrent
// clients (GUI, CLI tools), each on its own thread.
class ControlServer
{
public:
    // Returns the fully encoded response message (header + payload) ready
    // to write to the socket; the handler picks the response opcode
    // (StatusResponse, DeviceListResponse, ...) so ControlServer itself
    // stays agnostic to what any given command returns.
    using CommandHandler = std::function<std::vector<uint8>(const Command&)>;

    explicit ControlServer(std::string InSocketPath);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    void SetCommandHandler(CommandHandler InHandler);

    // Binds the socket and spawns the accept-loop thread. Returns false on
    // failure.
    bool Start();

    // Stops accepting new connections, disconnects every client, and
    // joins all threads - so once it returns, no command handler is
    // running or can run again. A command already in progress finishes
    // first (its reply is dropped if the client is gone), so the handler
    // must not wait on anything the caller of Stop() is blocking.
    void Stop();

private:
    // One connected client. Its thread never closes Fd: the fd stays
    // valid (not reused by the kernel) until the thread is joined, so
    // Stop() can shutdown() it safely at any time.
    struct Client
    {
        int Fd = -1;
        std::thread Thread;
        std::atomic<bool> bDone{false};
    };

    void AcceptLoop();
    void HandleClient(Client& InClient);

    // Joins and closes clients whose thread has finished. Caller holds
    // ClientsMutex.
    void ReapFinishedClients();

    std::string SocketPath;
    int ListenFd = -1;
    std::thread AcceptThread;
    std::atomic<bool> bRunning{false};
    CommandHandler Handler;

    std::mutex ClientsMutex;
    std::list<std::unique_ptr<Client>> Clients;
};

} // namespace ramkolfx
