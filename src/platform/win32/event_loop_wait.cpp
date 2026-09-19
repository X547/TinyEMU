/*
 * Event loop wait: I/O completion port
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "event_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <windows.h>
#include <winternl.h>
#include <timeapi.h>

#include "device_lock.h"
#include "wait_set.h"

/* ntdll. A wait completion packet queues a packet on a completion port once
   the object it is associated with is signalled, so that one port can wait
   on anything. */
extern "C" {
NTSTATUS NTAPI NtCreateWaitCompletionPacket(PHANDLE packet,
                                            ACCESS_MASK access,
                                            POBJECT_ATTRIBUTES attributes);
NTSTATUS NTAPI NtAssociateWaitCompletionPacket(HANDLE packet, HANDLE port,
                                               HANDLE target, PVOID key,
                                               PVOID apc_context,
                                               NTSTATUS status,
                                               ULONG_PTR information,
                                               PBOOLEAN already_signaled);
NTSTATUS NTAPI NtCancelWaitCompletionPacket(HANDLE packet,
                                            BOOLEAN remove_signaled);
}

/* How many packets one call takes off the port. */
#define MAX_PACKETS 64

/* Tells a wait packet from the other packets the port receives. */
static char sWaitPacketTag;


/* One handle being waited on. A packet fires once and is associated again
   for the next wait. */
struct HandleWait {
    enum State {
        IDLE,
        ARMED,
        /* cancelled too late: the packet is on its way */
        DRAINING,
    };

    HANDLE handle;
    HANDLE packet = nullptr;
    State state = IDLE;
    /* by the wait in progress */
    bool watched = false;
};


/* The completion port every wait is on, and the handles waited on through
   it. Waking is a packet posted to the port. */
class LoopWaker {
private:
    std::vector<std::unique_ptr<HandleWait>> fWaits;

    HandleWait *Find(HANDLE h);

public:
    HANDLE port;

    LoopWaker();
    ~LoopWaker();

    /* Wait on what 'ws' watches, and on nothing else. */
    void Arm(WaitSet &ws);
    /* Returns true if the loop was woken. */
    bool Collect(WaitSet &ws);
};


LoopWaker::LoopWaker()
{
    /* No limit on how many threads may run: the port counts a thread that
       once waited on it as one of its own for good, so the thread that set
       things up would otherwise keep the loop's thread from being released
       while it goes on to run the window. */
    port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, MAXDWORD);
    if (port == nullptr) {
        fprintf(stderr, "CreateIoCompletionPort: error %lu\n", GetLastError());
        exit(1);
    }
}


LoopWaker::~LoopWaker()
{
    for (auto &w : fWaits) {
        if (w->state != HandleWait::IDLE)
            NtCancelWaitCompletionPacket(w->packet, TRUE);
        CloseHandle(w->packet);
    }
    CloseHandle(port);
}


HandleWait *LoopWaker::Find(HANDLE h)
{
    for (auto &w : fWaits) {
        if (w->handle == h)
            return w.get();
    }
    return nullptr;
}


void LoopWaker::Arm(WaitSet &ws)
{
    NTSTATUS st;

    for (auto &w : fWaits)
        w->watched = false;

    for (HANDLE h : ws.fWatched) {
        HandleWait *w = Find(h);
        if (w == nullptr) {
            auto nw = std::make_unique<HandleWait>();
            nw->handle = h;
            st = NtCreateWaitCompletionPacket(&nw->packet, GENERIC_ALL,
                                              nullptr);
            if (st < 0) {
                fprintf(stderr, "NtCreateWaitCompletionPacket: %#lx\n",
                        (unsigned long)st);
                exit(1);
            }
            w = nw.get();
            fWaits.push_back(std::move(nw));
        }
        w->watched = true;
        if (w->state == HandleWait::IDLE) {
            st = NtAssociateWaitCompletionPacket(w->packet, port, h, w,
                                                 &sWaitPacketTag, 0, 0,
                                                 nullptr);
            if (st < 0) {
                fprintf(stderr, "NtAssociateWaitCompletionPacket: %#lx\n",
                        (unsigned long)st);
                abort();
            }
            w->state = HandleWait::ARMED;
        }
    }

    /* Stop waiting on what no source watches any more. A packet that is
       already queued is left for Collect() to take. */
    for (auto it = fWaits.begin(); it != fWaits.end();) {
        HandleWait *w = it->get();
        if (!w->watched && w->state == HandleWait::ARMED) {
            st = NtCancelWaitCompletionPacket(w->packet, FALSE);
            w->state = st == STATUS_PENDING ? HandleWait::DRAINING
                                            : HandleWait::IDLE;
        }
        if (!w->watched && w->state == HandleWait::IDLE) {
            CloseHandle(w->packet);
            it = fWaits.erase(it);
        } else {
            ++it;
        }
    }
}


bool LoopWaker::Collect(WaitSet &ws)
{
    OVERLAPPED_ENTRY entries[MAX_PACKETS];
    DWORD timeout = ws.timeout_ms;
    bool woken = false;

    for (;;) {
        ULONG n = 0;
        if (!GetQueuedCompletionStatusEx(port, entries, MAX_PACKETS, &n,
                                         timeout, FALSE))
            break;
        for (ULONG i = 0; i < n; i++) {
            const OVERLAPPED_ENTRY &e = entries[i];
            if (e.lpOverlapped == (LPOVERLAPPED)&sWaitPacketTag) {
                HandleWait *w = reinterpret_cast<HandleWait *>(
                    e.lpCompletionKey);
                w->state = HandleWait::IDLE;
                if (w->watched)
                    ws.fSignaled.push_back(w->handle);
            } else {
                woken = true;
            }
        }
        if (n < MAX_PACKETS)
            break;
        timeout = 0;
    }
    return woken;
}


EventLoop::EventLoop():
    fWaker(std::make_unique<LoopWaker>())
{
    /* Every timed wait in the process, the processor thread's too, rounds to
       the system tick otherwise. */
    timeBeginPeriod(1);
}


EventLoop::~EventLoop()
{
    Stop();
    timeEndPeriod(1);
}


void EventLoop::Wake()
{
    if (!fWakePending.exchange(true))
        PostQueuedCompletionStatus(fWaker->port, 0, 0, nullptr);
}


void EventLoop::Wait(int timeout_ms)
{
    WaitSet ws(timeout_ms);

    /* a source may remove itself while being dispatched */
    std::vector<PollSource *> sources = fSources;

    if (fDeviceLock != nullptr)
        fDeviceLock->Lock();
    for (PollSource *source : sources) {
        source->Prepare(ws);
    }
    if (fDeviceLock != nullptr)
        fDeviceLock->Unlock();

    fWaker->Arm(ws);
    if (ws.timeout_ms < 0)
        ws.timeout_ms = 0;
    if (fWaker->Collect(ws)) {
        /* Cleared once the packet is taken: a wake that finds it still set
           needs no packet of its own, because the sources are prepared
           again after this anyway. */
        fWakePending.store(false);
    }

    if (fDeviceLock != nullptr)
        fDeviceLock->Lock();
    for (PollSource *source : sources) {
        source->Dispatch(ws);
    }
    if (fDeviceLock != nullptr)
        fDeviceLock->Unlock();
}
