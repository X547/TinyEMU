/*
 * The context devices are created in, and their connection to the host
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
#include "devices.h"


void device_context_connect(DeviceContext *ctx)
{
    Platform *platform = ctx->platform;
    HostConsole *console = platform->Console();

    if (console != nullptr) {
        console->SetTarget(ctx->console_input != nullptr ? ctx->console_input
                                                         : ctx->serial_input);
    }
    HostScreen *screen = platform->Screen();
    if (screen != nullptr && ctx->screen != nullptr) {
        screen->SetSource(ctx->screen, ctx->screen_width, ctx->screen_height);
        ctx->screen->SetScreen(screen);
        /* only a frame buffer the guest writes directly has to be polled */
        FBDevice *fb = dynamic_cast<FBDevice *>(ctx->screen);
        if (fb != nullptr) {
            ctx->machine->SetDisplay(screen, fb);
        }
    }
    if (platform->Keyboard() != nullptr) {
        platform->Keyboard()->SetTarget(ctx->keyboard);
    }
    if (platform->Pointer() != nullptr) {
        platform->Pointer()->SetTarget(ctx->mouse);
    }
    for (HostEthernet *net : ctx->ethernet) {
        if (net->target != nullptr) {
            net->target->SetCarrier(true);
        }
    }
}
