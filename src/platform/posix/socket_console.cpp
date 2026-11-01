/*
 * Host console: a UNIX domain socket
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
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "platform_backends.h"
#include "run_control.h"
#include "wait_set.h"

/* How often a wait for the client looks for a shutdown. */
#define SOCKET_RETRY_US 100000
#define SOCKET_BUFFER_SIZE 4096

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif


/* As a server it takes one client at a time, turns away any other while
   that one is connected, and waits for the next once it goes; as a client
   it is done when the server goes. What the guest sends while nothing is
   connected is dropped. */
class SocketConsole final: public HostConsole, public PollSource {
private:
    EventLoop &fLoop;
    RunControl &fRunControl;
    ConsoleTarget *fTarget = nullptr;
    std::string fPath;
    bool fServer = false;
    /* the listening socket, for a server */
    int fListen = -1;
    /* the connection, or -1 */
    int fConn = -1;
    /* a send found the other end gone, which the loop has to act on, since
       it may be waiting on the socket */
    std::atomic<bool> fBroken {false};

    /* input read but not yet taken by the target */
    std::vector<uint8_t> fPending;
    size_t fPendingPos = 0;
    /* the target had no room at the last Prepare() */
    bool fBlocked = false;
    bool fWatchListen = false;
    bool fWatchConn = false;

    bool FillAddress(struct sockaddr_un *addr);
    bool Listen();
    void Accept();
    void Disconnected();
    void Deliver();

public:
    SocketConsole(EventLoop &loop, RunControl &run_control, const char *path);
    ~SocketConsole() override;

    bool Open(bool server);

    /* HostConsole */
    void WriteData(const uint8_t *buf, int len) override;
    void SetTarget(ConsoleTarget *target) override {fTarget = target;}
    void TargetReady() override;
    bool WaitConnected() override;

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
};


SocketConsole::SocketConsole(EventLoop &loop, RunControl &run_control,
                             const char *path):
    fLoop(loop),
    fRunControl(run_control),
    fPath(path)
{
    fLoop.Add(this);
}


SocketConsole::~SocketConsole()
{
    fLoop.Remove(this);
    if (fConn >= 0)
        close(fConn);
    if (fListen >= 0) {
        close(fListen);
        unlink(fPath.c_str());
    }
}


bool SocketConsole::FillAddress(struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    if (fPath.size() >= sizeof(addr->sun_path)) {
        fprintf(stderr, "%s: a socket path takes at most %d bytes\n",
                fPath.c_str(), (int)sizeof(addr->sun_path) - 1);
        return false;
    }
    memcpy(addr->sun_path, fPath.c_str(), fPath.size());
    return true;
}


static int new_socket()
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}


/* A socket file nothing listens on any more is left over from an earlier
   run, and is replaced; one something listens on is someone else's. */
bool SocketConsole::Listen()
{
    struct sockaddr_un addr;
    struct stat st;

    if (!FillAddress(&addr))
        return false;
    if (lstat(fPath.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            fprintf(stderr, "%s: not a socket\n", fPath.c_str());
            return false;
        }
        int probe = new_socket();
        bool live = probe >= 0 &&
                    connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
        if (probe >= 0)
            close(probe);
        if (live) {
            fprintf(stderr, "%s: another program listens on it\n",
                    fPath.c_str());
            return false;
        }
        unlink(fPath.c_str());
    }

    fListen = new_socket();
    if (fListen < 0 ||
        bind(fListen, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "%s: %s\n", fPath.c_str(), strerror(errno));
        if (fListen >= 0)
            close(fListen);
        fListen = -1;
        return false;
    }
    /* a client that goes before it is taken must not block accept() */
    fcntl(fListen, F_SETFL, fcntl(fListen, F_GETFL) | O_NONBLOCK);
    if (listen(fListen, 1) < 0) {
        fprintf(stderr, "%s: %s\n", fPath.c_str(), strerror(errno));
        return false;
    }
    return true;
}


bool SocketConsole::Open(bool server)
{
    struct sockaddr_un addr;

#ifndef MSG_NOSIGNAL
    /* a client going away fails the send instead of ending the emulator */
    signal(SIGPIPE, SIG_IGN);
#endif
    fServer = server;
    if (server)
        return Listen();

    if (!FillAddress(&addr))
        return false;
    fConn = new_socket();
    if (fConn < 0 ||
        connect(fConn, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "%s: %s\n", fPath.c_str(), strerror(errno));
        return false;
    }
    return true;
}


/* The next client, if one is waiting; any other while one is connected is
   turned away. */
void SocketConsole::Accept()
{
    int fd = accept(fListen, nullptr, nullptr);
    if (fd < 0)
        return;
    if (fConn >= 0) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    /* the guest waits for a slow client, as it does for the terminal */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    fConn = fd;
}


void SocketConsole::Disconnected()
{
    close(fConn);
    fConn = -1;
    fBroken = false;
    fPending.clear();
    fPendingPos = 0;
}


bool SocketConsole::WaitConnected()
{
    if (!fServer || fConn >= 0)
        return true;
    fprintf(stderr, "waiting for a client on %s\n", fPath.c_str());
    while (fConn < 0) {
        fd_set rfds;
        struct timeval tv = {0, SOCKET_RETRY_US};

        if (fRunControl.ShutdownRequested())
            return false;
        FD_ZERO(&rfds);
        FD_SET(fListen, &rfds);
        if (select(fListen + 1, &rfds, nullptr, nullptr, &tv) > 0)
            Accept();
    }
    return true;
}


/* With the device lock held, like the loop's side. */
void SocketConsole::WriteData(const uint8_t *buf, int len)
{
    if (fConn < 0 || fBroken)
        return;
    while (len > 0) {
        ssize_t n = send(fConn, buf, len, SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fBroken = true;
            fLoop.Wake();
            return;
        }
        buf += n;
        len -= n;
    }
}


void SocketConsole::TargetReady()
{
    if (fBlocked) {
        fBlocked = false;
        fLoop.Wake();
    }
}


/* As much of what was read as the target takes. */
void SocketConsole::Deliver()
{
    int len = std::min<int>(fTarget->ReceiveRoom(),
                            fPending.size() - fPendingPos);
    if (len <= 0)
        return;
    fTarget->Receive(fPending.data() + fPendingPos, len);
    fPendingPos += len;
    if (fPendingPos == fPending.size()) {
        fPending.clear();
        fPendingPos = 0;
    }
}


void SocketConsole::Prepare(WaitSet &ws)
{
    fWatchListen = false;
    fWatchConn = false;
    if (fBroken && fConn >= 0)
        Disconnected();
    if (fTarget != nullptr && !fPending.empty()) {
        if (fTarget->ReceiveRoom() <= 0)
            fBlocked = true;
        else
            ws.LimitTimeout(0);
    }
    /* The connection is watched, which is also how a client that went is
       noticed, until what was read has gone. */
    if (fConn >= 0 && fPending.empty()) {
        ws.WatchRead(fConn);
        fWatchConn = true;
    }
    if (fListen >= 0) {
        ws.WatchRead(fListen);
        fWatchListen = true;
    }
}


void SocketConsole::Dispatch(WaitSet &ws)
{
    if (fWatchConn && fConn >= 0 && ws.IsReadable(fConn)) {
        uint8_t buf[SOCKET_BUFFER_SIZE];
        ssize_t n = recv(fConn, buf, sizeof(buf), 0);
        if (n > 0) {
            fPending.insert(fPending.end(), buf, buf + n);
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
            Disconnected();
        }
    }
    if (fWatchListen && ws.IsReadable(fListen))
        Accept();
    if (fTarget != nullptr)
        Deliver();
}


std::unique_ptr<HostConsole> socket_console_open(EventLoop &loop,
                                                 RunControl &run_control,
                                                 const char *path,
                                                 bool server)
{
    auto s = std::make_unique<SocketConsole>(loop, run_control, path);
    if (!s->Open(server))
        return nullptr;
    return s;
}
