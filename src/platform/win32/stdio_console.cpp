/*
 * Host console: the Windows console, or standard input and output
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
#include <string.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>

#include <windows.h>

#include "console_escape.h"
#include "platform_backends.h"
#include "run_control.h"
#include "wait_set.h"

/* How much piped input is read ahead of the guest. */
#define PIPE_BUFFER_MAX 65536


static RunControl *sCtrlRunControl;

/* Ctrl + C, when it is not the guest's, and Ctrl + Break. A second one
   while the machine is shutting down ends the process at once. */
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    if (sCtrlRunControl == nullptr || sCtrlRunControl->ShutdownRequested())
        return FALSE;
    sCtrlRunControl->RequestShutdown(0);
    return TRUE;
}


/* The escape sequence a VT100 sends for a key that has no character, for a
   console that does not make them itself. */
static const char *vt_key_sequence(WORD vk)
{
    switch (vk) {
    case VK_UP:     return "\x1b[A";
    case VK_DOWN:   return "\x1b[B";
    case VK_RIGHT:  return "\x1b[C";
    case VK_LEFT:   return "\x1b[D";
    case VK_HOME:   return "\x1b[H";
    case VK_END:    return "\x1b[F";
    case VK_INSERT: return "\x1b[2~";
    case VK_DELETE: return "\x1b[3~";
    case VK_PRIOR:  return "\x1b[5~";
    case VK_NEXT:   return "\x1b[6~";
    case VK_F1:     return "\x1bOP";
    case VK_F2:     return "\x1bOQ";
    case VK_F3:     return "\x1bOR";
    case VK_F4:     return "\x1bOS";
    case VK_F5:     return "\x1b[15~";
    case VK_F6:     return "\x1b[17~";
    case VK_F7:     return "\x1b[18~";
    case VK_F8:     return "\x1b[19~";
    case VK_F9:     return "\x1b[20~";
    case VK_F10:    return "\x1b[21~";
    case VK_F11:    return "\x1b[23~";
    case VK_F12:    return "\x1b[24~";
    }
    return nullptr;
}


/* Keys typed in a console window, or bytes from a pipe or a file. */
class StdioConsole final: public HostConsole, public PollSource {
private:
    EventLoop &fLoop;
    RunControl &fRunControl;
    ConsoleEscape fEscape;
    ConsoleTarget *fTarget = nullptr;

    HANDLE fInput = nullptr;
    HANDLE fOutput = nullptr;
    bool fInputIsConsole = false;
    bool fOutputIsConsole = false;
    DWORD fOldInputMode = 0;
    DWORD fOldOutputMode = 0;
    UINT fOldOutputCodePage = 0;
    /* the console turns keys without a character into escape sequences */
    bool fVtInput = false;
    bool fResizePending = true;
    /* half of a character outside the basic multilingual plane */
    wchar_t fHighSurrogate = 0;

    /* input read but not yet taken by the target */
    std::vector<uint8_t> fPending;
    size_t fPendingPos = 0;
    /* the handle the last Prepare() watched, if any */
    HANDLE fWatched = nullptr;
    /* the target had no room at the last Prepare() */
    bool fBlocked = false;
    bool fEndSeen = false;

    /* Piped input is read on a thread of its own, since a pipe cannot be
       waited on. It signals fDataEvent while fPipeData or the end of the
       input waits to be taken. */
    HANDLE fReader = nullptr;
    HANDLE fDataEvent = nullptr;
    std::mutex fPipeMutex;
    std::condition_variable fPipeCond;
    std::vector<uint8_t> fPipeData;
    bool fPipeEnd = false;
    bool fPipeStop = false;

    static DWORD WINAPI ReaderThread(LPVOID param);
    void ReadPipe();
    void StopReader();

    void AppendChar(std::vector<uint8_t> &out, wchar_t c);
    void ReadConsoleKeys();
    void TakePipeData();
    void Deliver();
    void SendSize();

public:
    StdioConsole(EventLoop &loop, RunControl &run_control, bool allow_ctrlc);
    ~StdioConsole() override;

    /* HostConsole */
    void WriteData(const uint8_t *buf, int len) override;
    void SetTarget(ConsoleTarget *target) override {fTarget = target;}
    void TargetReady() override;

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
};


StdioConsole::StdioConsole(EventLoop &loop, RunControl &run_control,
                           bool allow_ctrlc):
    fLoop(loop),
    fRunControl(run_control),
    fEscape(run_control)
{
    /* The guest ends its own lines. */
    _setmode(_fileno(stdout), _O_BINARY);

    fOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    fOutputIsConsole = GetConsoleMode(fOutput, &fOldOutputMode);
    if (fOutputIsConsole) {
        /* The guest drives a VT100 and speaks UTF-8. A line feed also
           returns the carriage, as a terminal does for the emulator's own
           messages on other hosts. */
        SetConsoleMode(fOutput, fOldOutputMode | ENABLE_PROCESSED_OUTPUT |
                                    ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        fOldOutputCodePage = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
    }

    fInput = GetStdHandle(STD_INPUT_HANDLE);
    if (fInput == nullptr || fInput == INVALID_HANDLE_VALUE) {
        fInput = nullptr;
    } else if (GetConsoleMode(fInput, &fOldInputMode)) {
        fInputIsConsole = true;
        /* Keys one at a time and not echoed; C-c is the guest's unless it
           was asked to stop the emulator. */
        DWORD mode = ENABLE_WINDOW_INPUT;
        if (allow_ctrlc)
            mode |= ENABLE_PROCESSED_INPUT;
        fVtInput = SetConsoleMode(fInput,
                                  mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
        if (!fVtInput)
            SetConsoleMode(fInput, mode);
    } else {
        fDataEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        fReader = CreateThread(nullptr, 0, ReaderThread, this, 0, nullptr);
    }

    sCtrlRunControl = &fRunControl;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    fLoop.Add(this);
}


StdioConsole::~StdioConsole()
{
    fLoop.Remove(this);
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    sCtrlRunControl = nullptr;
    StopReader();
    fflush(stdout);
    if (fInputIsConsole)
        SetConsoleMode(fInput, fOldInputMode);
    if (fOutputIsConsole) {
        SetConsoleMode(fOutput, fOldOutputMode);
        SetConsoleOutputCP(fOldOutputCodePage);
    }
}


void StdioConsole::WriteData(const uint8_t *buf, int len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}


void StdioConsole::TargetReady()
{
    if (fBlocked) {
        fBlocked = false;
        fLoop.Wake();
    }
}


//#pragma mark - console input

/* A UTF-16 unit of a key, as UTF-8. */
void StdioConsole::AppendChar(std::vector<uint8_t> &out, wchar_t c)
{
    uint32_t cp = c;

    if (c >= 0xd800 && c < 0xdc00) {
        fHighSurrogate = c;
        return;
    }
    if (c >= 0xdc00 && c < 0xe000) {
        if (fHighSurrogate == 0)
            return;
        cp = 0x10000 + ((fHighSurrogate - 0xd800) << 10) + (c - 0xdc00);
    }
    fHighSurrogate = 0;

    if (cp < 0x80) {
        out.push_back(cp);
    } else if (cp < 0x800) {
        out.push_back(0xc0 | (cp >> 6));
        out.push_back(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out.push_back(0xe0 | (cp >> 12));
        out.push_back(0x80 | ((cp >> 6) & 0x3f));
        out.push_back(0x80 | (cp & 0x3f));
    } else {
        out.push_back(0xf0 | (cp >> 18));
        out.push_back(0x80 | ((cp >> 12) & 0x3f));
        out.push_back(0x80 | ((cp >> 6) & 0x3f));
        out.push_back(0x80 | (cp & 0x3f));
    }
}


/* Everything in the console's input buffer, which is also what keeps its
   handle signalled. */
void StdioConsole::ReadConsoleKeys()
{
    INPUT_RECORD records[64];
    DWORD count = 0;
    std::vector<uint8_t> bytes;

    if (!GetNumberOfConsoleInputEvents(fInput, &count) || count == 0)
        return;
    if (!ReadConsoleInputW(fInput, records, std::min<DWORD>(count, 64),
                           &count))
        return;

    for (DWORD i = 0; i < count; i++) {
        const INPUT_RECORD &r = records[i];
        if (r.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            fResizePending = true;
            continue;
        }
        if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown)
            continue;
        const KEY_EVENT_RECORD &k = r.Event.KeyEvent;
        for (WORD n = 0; n < std::max<WORD>(k.wRepeatCount, 1); n++) {
            wchar_t c = k.uChar.UnicodeChar;
            if (c != 0) {
                /* a VT100's backspace */
                if (!fVtInput && k.wVirtualKeyCode == VK_BACK)
                    c = 0x7f;
                AppendChar(bytes, c);
            } else if (!fVtInput) {
                const char *seq = vt_key_sequence(k.wVirtualKeyCode);
                if (seq != nullptr)
                    bytes.insert(bytes.end(), seq, seq + strlen(seq));
            }
        }
    }

    int len = fEscape.Filter(bytes.data(), bytes.size());
    fPending.insert(fPending.end(), bytes.begin(), bytes.begin() + len);
}


/* The size of the console window, in characters. */
void StdioConsole::SendSize()
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    int width = 80, height = 25;

    if (GetConsoleScreenBufferInfo(fOutput, &info)) {
        width = info.srWindow.Right - info.srWindow.Left + 1;
        height = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
    if (width >= 4 && height >= 4)
        fTarget->Resize(width, height);
}


//#pragma mark - piped input

DWORD WINAPI StdioConsole::ReaderThread(LPVOID param)
{
    static_cast<StdioConsole *>(param)->ReadPipe();
    return 0;
}


void StdioConsole::ReadPipe()
{
    uint8_t buf[4096];

    for (;;) {
        {
            std::unique_lock<std::mutex> locker(fPipeMutex);
            fPipeCond.wait(locker, [this]() {
                return fPipeStop || fPipeData.size() < PIPE_BUFFER_MAX;
            });
            if (fPipeStop)
                return;
        }
        DWORD got = 0;
        BOOL ok = ReadFile(fInput, buf, sizeof(buf), &got, nullptr);
        std::lock_guard<std::mutex> locker(fPipeMutex);
        if (!ok || got == 0) {
            /* the end of the input, or StopReader() cutting it short */
            fPipeEnd = true;
            SetEvent(fDataEvent);
            return;
        }
        fPipeData.insert(fPipeData.end(), buf, buf + got);
        SetEvent(fDataEvent);
    }
}


void StdioConsole::StopReader()
{
    if (fReader == nullptr)
        return;
    {
        std::lock_guard<std::mutex> locker(fPipeMutex);
        fPipeStop = true;
    }
    fPipeCond.notify_all();
    /* A read that has not started yet is not cancelled, so keep trying
       until the thread is gone. */
    while (WaitForSingleObject(fReader, 10) == WAIT_TIMEOUT)
        CancelSynchronousIo(fReader);
    CloseHandle(fReader);
    CloseHandle(fDataEvent);
    fReader = nullptr;
}


void StdioConsole::TakePipeData()
{
    std::vector<uint8_t> bytes;
    bool end;

    {
        std::lock_guard<std::mutex> locker(fPipeMutex);
        bytes.swap(fPipeData);
        end = fPipeEnd;
        if (!end)
            ResetEvent(fDataEvent);
    }
    fPipeCond.notify_all();

    int len = fEscape.Filter(bytes.data(), bytes.size());
    fPending.insert(fPending.end(), bytes.begin(), bytes.begin() + len);
    if (end && !fEndSeen) {
        fEndSeen = true;
        fRunControl.RequestShutdown(1);
    }
}


//#pragma mark - the event loop

/* As much of what was read as the target takes. */
void StdioConsole::Deliver()
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


void StdioConsole::Prepare(WaitSet &ws)
{
    fWatched = nullptr;
    if (fTarget == nullptr)
        return;
    if (fTarget->ReceiveRoom() <= 0) {
        fBlocked = true;
        return;
    }
    if (fInputIsConsole && fResizePending) {
        fResizePending = false;
        SendSize();
    }
    if (!fPending.empty()) {
        ws.LimitTimeout(0);
        return;
    }
    /* Nothing more is read until what was read has gone. */
    if (fInputIsConsole)
        fWatched = fInput;
    else if (fDataEvent != nullptr && !fEndSeen)
        fWatched = fDataEvent;
    if (fWatched != nullptr)
        ws.WatchHandle(fWatched);
}


void StdioConsole::Dispatch(WaitSet &ws)
{
    if (fTarget == nullptr)
        return;
    if (fWatched != nullptr && ws.IsSignaled(fWatched)) {
        if (fInputIsConsole)
            ReadConsoleKeys();
        else
            TakePipeData();
    }
    Deliver();
}


std::unique_ptr<HostConsole> host_console_create(EventLoop &loop,
                                                 RunControl &run_control,
                                                 bool allow_ctrlc)
{
    return std::make_unique<StdioConsole>(loop, run_control, allow_ctrlc);
}
