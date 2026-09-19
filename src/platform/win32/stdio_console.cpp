/*
 * Host console: standard output
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
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include "platform_backends.h"


/* Guest output only; nothing is read from the keyboard yet. */
class StdioConsole final: public HostConsole {
private:
    HANDLE fOutput;
    DWORD fOldMode = 0;
    UINT fOldCodePage = 0;
    bool fIsConsole = false;

public:
    StdioConsole();
    ~StdioConsole() override;

    void WriteData(const uint8_t *buf, int len) override;
    void SetTarget(ConsoleTarget *target) override {(void)target;}
};


StdioConsole::StdioConsole()
{
    /* The guest ends its own lines. */
    _setmode(_fileno(stdout), _O_BINARY);

    fOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    fIsConsole = GetConsoleMode(fOutput, &fOldMode);
    if (fIsConsole) {
        /* The guest drives a VT100 and speaks UTF-8. */
        SetConsoleMode(fOutput, fOldMode | ENABLE_PROCESSED_OUTPUT |
                                    ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                    DISABLE_NEWLINE_AUTO_RETURN);
        fOldCodePage = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
    }
}


StdioConsole::~StdioConsole()
{
    fflush(stdout);
    if (fIsConsole) {
        SetConsoleMode(fOutput, fOldMode);
        SetConsoleOutputCP(fOldCodePage);
    }
}


void StdioConsole::WriteData(const uint8_t *buf, int len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}


std::unique_ptr<HostConsole> host_console_create(EventLoop &loop,
                                                 RunControl &run_control,
                                                 bool allow_ctrlc)
{
    (void)loop;
    (void)run_control;
    (void)allow_ctrlc;
    return std::make_unique<StdioConsole>();
}
