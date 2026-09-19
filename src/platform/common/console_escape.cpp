/*
 * Host console: the emulator's own keys
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
#include "console_escape.h"

#include <stdio.h>

#include "run_control.h"


int ConsoleEscape::Filter(uint8_t *buf, int len)
{
    int j = 0;

    for (int i = 0; i < len; i++) {
        uint8_t ch = buf[i];
        if (fEscape) {
            fEscape = false;
            switch (ch) {
            case 'x':
                printf("Terminated\n");
                fflush(stdout);
                fRunControl.RequestShutdown(0);
                return j;
            case 'h':
                printf("\n"
                       "C-a h   print this help\n"
                       "C-a x   exit emulator\n"
                       "C-a C-a send C-a\n");
                fflush(stdout);
                break;
            case 1:
                buf[j++] = ch;
                break;
            default:
                break;
            }
        } else if (ch == 1) {
            fEscape = true;
        } else {
            buf[j++] = ch;
        }
    }
    return j;
}
