/*
 * NVM Express controller
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
#pragma once

#include <stdint.h>

#include "device.h"

class HostBlockDevice;

/* As many namespaces as one controller will carry here. Namespace ids are
   1 based, so this is also the largest id. */
#define NVME_MAX_NAMESPACES 8


/* Deviations from the specification that a guest may need, enabled from the
   configuration file. Each of the first three names a behaviour rather than a
   guest, so that a different driver needing one of them does not have to ask
   for the others.

   They exist because Haiku's RISC-V boot loader has a deliberately minimal
   NVMe driver: it never enables the controller, addresses namespace 0, and
   creates its I/O submission queue before the completion queue it names. A
   conformant guest gets a conformant controller unless one of these is asked
   for. */
enum {
    /* Serve the admin queue as soon as AQA, ASQ and ACQ are programmed,
       instead of waiting for CC.EN. */
    NVME_QUIRK_NO_ENABLE_CHECK    = 1 << 0,
    /* Take namespace id 0 to mean namespace 1, for Identify and for I/O. */
    NVME_QUIRK_NSID_ZERO          = 1 << 1,
    /* Accept a queue creation that asks for a discontiguous queue, and an
       I/O submission queue whose completion queue id is 0. */
    NVME_QUIRK_LOOSE_QUEUE_CREATE = 1 << 2,
    /* Never drive the INTx pin, leaving the guest to poll.

       A completion queue interrupt is a level: it is asserted while the queue
       has entries the host has not taken, and it clears when the host rings
       the queue's head doorbell. Haiku's disk driver installs a handler that
       only wakes a thread and returns, without touching the queue, so the
       line is still asserted when the handler returns and it re-enters
       immediately -- a storm the waiting thread never gets to run through.
       Its driver copes with a controller that never interrupts (it waits
       once, then polls), so on a machine with no message signalled
       interrupts that is the way to run it. */
    NVME_QUIRK_POLL_ONLY = 1 << 3,
};

/* The quirks one configuration name asks for, or 0 when the name is not
   known. "haiku-boot-loader" is an alias for the three above, which that one
   driver needs together. */
uint32_t nvme_quirks_from_name(const char *name);

/* nvme.cpp */
Device *nvme_node_create(const char *name, uint32_t quirks);
Device *nvme_namespace_node_create(std::unique_ptr<HostBlockDevice> bs, int nsid);
