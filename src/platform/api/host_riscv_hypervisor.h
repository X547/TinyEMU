/*
 * Host hypervisor running the RISC-V harts
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

#include <memory>

#include "host_hypervisor_ram.h"
#include "riscv_vcpu.h"

class DeviceLock;


/* Implemented once per host; the build picks the file. The harts run in
   hardware, each on a host thread of its own. Sets 'backend' to the
   hypervisor's. Leaves it empty and returns true when the host has none,
   having said why if it should have had one; returns false, having said
   why, when it has one that cannot run 'options'. */
bool host_riscv_hypervisor_open(RISCVCpuTarget &target, DeviceLock &lock,
                                const RISCVCpuOptions &options,
                                std::unique_ptr<RISCVCpuBackend> *backend);
