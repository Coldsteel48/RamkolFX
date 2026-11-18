// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "control_server.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace ramkolfx
{

ControlServer::ControlServer(std::string InSocketPath) : SocketPath(std::move(InSocketPath))
{
}

ControlServer::~ControlServer()
{
    Stop();
}

void ControlServer::SetCommandHandler(CommandHandler InHandler)
{
    Handler = std::move(InHandler);
}

bool ControlServer::Start()
{
    ListenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ListenFd < 0)
    {
        fprintf(stderr, "[ramkolfxd] failed to create control socket: %s\n", strerror(errno));
        return false;
    }

    sockaddr_un Addr{};
    Addr.sun_family = AF_UNIX;
    if (SocketPath.size() >= sizeof(Addr.sun_path))
    {
        fprintf(stderr, "[ramkolfxd] control socket path too long: %s\n", SocketPath.c_str());
        close(ListenFd);
        ListenFd = -1;
        return false;
    }
    std::strncpy(Addr.sun_path, SocketPath.c_str(), sizeof(Addr.sun_path) - 1);

    unlink(SocketPath.c_str()); // remove a stale socket from a previous run

    if (bind(ListenFd, reinterpret_cast<sockaddr*>(&Addr), sizeof(Addr)) < 0)
    {
        fprintf(stderr, "[ramkolfxd] failed to bind control socket %s: %s\n", SocketPath.c_str(),
                strerror(errno));
        close(ListenFd);
        ListenFd = -1;
        return false;
    }

    if (listen(ListenFd, 4) < 0)
    {
        fprintf(stderr, "[ramkolfxd] failed to listen on control socket: %s\n", strerror(errno));
        close(ListenFd);
        ListenFd = -1;
        return false;
    }

    bRunning.store(true, std::memory_order_relaxed);
    AcceptThread = std::thread(&ControlServer::AcceptLoop, this);

    fprintf(stderr, "[ramkolfxd] control socket listening at %s\n", SocketPath.c_str());
    return true;
}

void ControlServer::Stop()
{
    if (!bRunning.exchange(false, std::memory_order_relaxed))
    {
        return;
    }
    if (ListenFd >= 0)
    {
        shutdown(ListenFd, SHUT_RDWR);
        close(ListenFd);
        ListenFd = -1;
    }
    if (AcceptThread.joinable())
    {
        AcceptThread.join();
    }

    // No new clients can appear now (the accept thread is gone). Wake
    // every client thread blocked in recv() - shutdown() makes it return
    // 0 - then wait for each to finish whatever command it's running.
    std::lock_guard<std::mutex> Lock(ClientsMutex);
    for (const std::unique_ptr<Client>& Entry : Clients)
    {
        shutdown(Entry->Fd, SHUT_RDWR);
    }
    for (const std::unique_ptr<Client>& Entry : Clients)
    {
        Entry->Thread.join();
        close(Entry->Fd);
    }
    Clients.clear();
    unlink(SocketPath.c_str());
}

void ControlServer::ReapFinishedClients()
{
    for (auto It = Clients.begin(); It != Clients.end();)
    {
        if ((*It)->bDone.load(std::memory_order_acquire))
        {
            (*It)->Thread.join();
            close((*It)->Fd);
            It = Clients.erase(It);
        }
        else
        {
            ++It;
        }
    }
}

void ControlServer::AcceptLoop()
{
    using Clock = std::chrono::steady_clock;
    constexpr auto RetryDelay = std::chrono::milliseconds(100);
    constexpr auto StillFailingLogInterval = std::chrono::seconds(30);

    // Copied once: Stop() resets the member to -1 from another thread.
    const int Listener = ListenFd;

    // Failure log throttling: a lasting failure retries every RetryDelay,
    // so it's logged when it starts, as a summary every
    // StillFailingLogInterval, and when it clears - not on every attempt.
    int FailingErrno = 0; // 0 = not currently failing
    unsigned long FailuresTotal = 0;
    unsigned long FailuresSinceLog = 0;
    Clock::time_point LastFailureLog;

    while (bRunning.load(std::memory_order_relaxed))
    {
        int ClientFd = accept4(Listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (ClientFd < 0)
        {
            const int Error = errno;
            if (!bRunning.load(std::memory_order_relaxed))
            {
                break; // Stop() shut the listen socket down
            }
            if (Error == EINTR || Error == ECONNABORTED)
            {
                continue; // harmless: retry right away
            }
            // Anything else (typically EMFILE/ENFILE - out of fds) is
            // usually temporary, and giving up here would leave the daemon
            // running but unreachable for good. Free what we can, back
            // off, and keep accepting.
            ++FailuresTotal;
            ++FailuresSinceLog;
            const Clock::time_point Now = Clock::now();
            if (Error != FailingErrno)
            {
                fprintf(stderr, "[ramkolfxd] control socket accept failed: %s (retrying)\n", strerror(Error));
                FailingErrno = Error;
                FailuresSinceLog = 0;
                LastFailureLog = Now;
            }
            else if (Now - LastFailureLog >= StillFailingLogInterval)
            {
                fprintf(stderr, "[ramkolfxd] control socket accept still failing: %s (%lu attempts in the last %llds)\n",
                        strerror(Error), FailuresSinceLog,
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::seconds>(Now - LastFailureLog).count()));
                FailuresSinceLog = 0;
                LastFailureLog = Now;
            }
            {
                std::lock_guard<std::mutex> Lock(ClientsMutex);
                ReapFinishedClients();
            }
            std::this_thread::sleep_for(RetryDelay);
            continue;
        }

        if (FailingErrno != 0)
        {
            fprintf(stderr, "[ramkolfxd] control socket accept recovered after %lu failed attempts\n",
                    FailuresTotal);
            FailingErrno = 0;
            FailuresTotal = 0;
        }

        std::lock_guard<std::mutex> Lock(ClientsMutex);
        // Each GUI reconnect is a new client, so finished ones are cleaned
        // up here rather than piling up until Stop().
        ReapFinishedClients();
        auto NewClient = std::make_unique<Client>();
        NewClient->Fd = ClientFd;
        Client& Ref = *NewClient;
        Clients.push_back(std::move(NewClient));
        Ref.Thread = std::thread(&ControlServer::HandleClient, this, std::ref(Ref));
    }
}

void ControlServer::HandleClient(Client& InClient)
{
    const int ClientFd = InClient.Fd;
    std::vector<uint8> Buffer;
    uint8 Chunk[512];

    for (;;)
    {
        ssize_t BytesRead = recv(ClientFd, Chunk, sizeof(Chunk), 0);
        if (BytesRead <= 0)
        {
            break; // client closed or error
        }
        Buffer.insert(Buffer.end(), Chunk, Chunk + BytesRead);

        for (;;)
        {
            auto Header = TryReadHeader(Buffer.data(), Buffer.size());
            if (!Header)
            {
                break; // need more bytes for even the header
            }
            const size_t Total = HeaderSize + Header->PayloadLength;
            if (Buffer.size() < Total)
            {
                break; // need more bytes for the full payload
            }

            const uint8* Payload = Buffer.data() + HeaderSize;
            std::vector<uint8> Response;

            auto DecodedCommand = DecodeCommand(Header->MessageOpcode, Payload, Header->PayloadLength);
            if (!DecodedCommand)
            {
                Response = EncodeErrorResponse("malformed or unknown command");
            }
            else if (Handler)
            {
                Response = Handler(*DecodedCommand);
            }
            else
            {
                Response = EncodeErrorResponse("no command handler registered");
            }

            send(ClientFd, Response.data(), Response.size(), MSG_NOSIGNAL);
            Buffer.erase(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Total));
        }
    }

    // Not closed here - see Client. Joined and closed by
    // ReapFinishedClients() or Stop().
    InClient.bDone.store(true, std::memory_order_release);
}

} // namespace ramkolfx
