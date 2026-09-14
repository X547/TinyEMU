/*
 * Host console: the controlling terminal
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "cutils.h"
#include "platform_backends.h"
#include "wait_set.h"


class StdioConsole final: public HostConsole, public PollSource {
private:
    EventLoop &fLoop;
    ConsoleTarget *fTarget = nullptr;
    int fStdinFd = 0;
    int fEscState = 0;
    bool fWatching = false;

    int ReadData(uint8_t *buf, int len);
    void GetSize(int *pw, int *ph);

public:
    StdioConsole(EventLoop &loop);
    ~StdioConsole() override;

    /* HostConsole */
    void WriteData(const uint8_t *buf, int len) override;
    void SetTarget(ConsoleTarget *target) override {fTarget = target;}

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
};

static struct termios oldtty;
static int old_fd0_flags;
/* set from the SIGWINCH handler; the first size is sent unasked */
static volatile sig_atomic_t sResizePending = 1;

static void term_exit(void)
{
    tcsetattr (0, TCSANOW, &oldtty);
    fcntl(0, F_SETFL, old_fd0_flags);
}

static void term_init(bool allow_ctrlc)
{
    struct termios tty;

    memset(&tty, 0, sizeof(tty));
    tcgetattr (0, &tty);
    oldtty = tty;
    old_fd0_flags = fcntl(0, F_GETFL);

    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                          |INLCR|IGNCR|ICRNL|IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
    if (!allow_ctrlc)
        tty.c_lflag &= ~ISIG;
    tty.c_cflag &= ~(CSIZE|PARENB);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    tcsetattr (0, TCSANOW, &tty);

    atexit(term_exit);
}

static void term_resize_handler(int sig)
{
    (void)sig;
    sResizePending = 1;
}


StdioConsole::StdioConsole(EventLoop &loop):
    fLoop(loop)
{
    fLoop.Add(this);
}


StdioConsole::~StdioConsole()
{
    fLoop.Remove(this);
}


void StdioConsole::WriteData(const uint8_t *buf, int len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}


int StdioConsole::ReadData(uint8_t *buf, int len)
{
    int ret, i, j;
    uint8_t ch;

    if (len <= 0)
        return 0;

    ret = read(fStdinFd, buf, len);
    if (ret < 0)
        return 0;
    if (ret == 0) {
        /* EOF */
        fLoop.RequestQuit(1);
        return 0;
    }

    j = 0;
    for(i = 0; i < ret; i++) {
        ch = buf[i];
        if (fEscState) {
            fEscState = 0;
            switch(ch) {
            case 'x':
                printf("Terminated\n");
                fLoop.RequestQuit(0);
                return j;
            case 'h':
                printf("\n"
                       "C-a h   print this help\n"
                       "C-a x   exit emulator\n"
                       "C-a C-a send C-a\n"
                       );
                break;
            case 1:
                goto output_char;
            default:
                break;
            }
        } else {
            if (ch == 1) {
                fEscState = 1;
            } else {
            output_char:
                buf[j++] = ch;
            }
        }
    }
    return j;
}


void StdioConsole::GetSize(int *pw, int *ph)
{
    struct winsize ws;
    int width, height;
    /* default values */
    width = 80;
    height = 25;
    if (ioctl(fStdinFd, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col >= 4 && ws.ws_row >= 4) {
        width = ws.ws_col;
        height = ws.ws_row;
    }
    *pw = width;
    *ph = height;
}


void StdioConsole::Prepare(WaitSet &ws)
{
    fWatching = false;
    if (fTarget == nullptr || fTarget->ReceiveRoom() <= 0)
        return;
    if (sResizePending) {
        int width, height;
        sResizePending = 0;
        GetSize(&width, &height);
        fTarget->Resize(width, height);
    }
    ws.WatchRead(fStdinFd);
    fWatching = true;
}


void StdioConsole::Dispatch(WaitSet &ws)
{
    uint8_t buf[128];
    int len;

    if (!fWatching || fTarget == nullptr || !ws.IsReadable(fStdinFd))
        return;
    len = min_int(fTarget->ReceiveRoom(), sizeof(buf));
    len = ReadData(buf, len);
    if (len > 0)
        fTarget->Receive(buf, len);
}


std::unique_ptr<HostConsole> host_console_create(EventLoop &loop,
                                                 bool allow_ctrlc)
{
    struct sigaction sig;

    term_init(allow_ctrlc);

    auto s = std::make_unique<StdioConsole>(loop);
    /* Note: the glibc does not properly tests the return value of
       write() in printf, so some messages on stdout may be lost */
    fcntl(0, F_SETFL, O_NONBLOCK);

    /* use a signal to get the host terminal resize events */
    sig.sa_handler = term_resize_handler;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = 0;
    sigaction(SIGWINCH, &sig, NULL);

    return s;
}
