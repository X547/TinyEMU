/*
 * TinyEMU
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <getopt.h>
#include <memory>
#include <vector>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"
#include "host_platform.h"

#define MAX_EXEC_CYCLE 500000
#define MAX_SLEEP_TIME 10 /* in ms */

/*******************************************************/

static struct option options[] = {
    { "help", no_argument, NULL, 'h' },
    { "ctrlc", no_argument },
    { "rw", no_argument },
    { "ro", no_argument },
    { "append", required_argument },
    { "no-accel", no_argument },
    { "build-preload", required_argument },
    { NULL },
};

void help(void)
{
    printf("temu version " CONFIG_VERSION ", Copyright (c) 2016-2018 Fabrice Bellard\n"
           "usage: riscvemu [options] config_file\n"
           "options are:\n"
           "-m ram_size       set the RAM size in MB\n"
           "-rw               allow write access to the disk image (default=snapshot)\n"
           "-ctrlc            the C-c key stops the emulator instead of being sent to the\n"
           "                  emulated software\n"
           "-append cmdline   append cmdline to the kernel command line\n"
           "-no-accel         disable VM acceleration (KVM, x86 machine only)\n"
           "\n"
           "Console keys:\n"
           "Press C-a x to exit the emulator, C-a h to get some help.\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *path, *cmdline;
    int c, option_index, ram_size, accel_enable;
    VirtMachineParams p_s, *p = &p_s;
    PlatformOptions platform_options;

    ram_size = -1;
    accel_enable = -1;
    cmdline = NULL;
    for(;;) {
        c = getopt_long_only(argc, argv, "hm:", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 0:
            switch(option_index) {
            case 1: /* ctrlc */
                platform_options.allow_ctrlc = true;
                break;
            case 2: /* rw */
                platform_options.block_mode = BLOCK_MODE_RW;
                break;
            case 3: /* ro */
                platform_options.block_mode = BLOCK_MODE_RO;
                break;
            case 4: /* append */
                cmdline = optarg;
                break;
            case 5: /* no-accel */
                accel_enable = false;
                break;
            case 6: /* build-preload */
                platform_options.build_preload_file = optarg;
                break;
            default:
                fprintf(stderr, "unknown option index: %d\n", option_index);
                exit(1);
            }
            break;
        case 'h':
            help();
            break;
        case 'm':
            ram_size = strtoul(optarg, NULL, 0);
            break;
        default:
            exit(1);
        }
    }

    if (optind >= argc) {
        help();
    }

    path = argv[optind++];

    /* declared in this order so that the machine goes first */
    EventLoop loop;
    HostPlatform platform(loop, platform_options);
    std::unique_ptr<VirtMachine> s;

    virt_machine_set_defaults(p);
    virt_machine_load_config_file(p, path, nullptr);
    loop.RunUntilIdle();

    /* override some config parameters */

    if (ram_size > 0) {
        p->ram_size = (uint64_t)ram_size << 20;
    }
    if (accel_enable != -1)
        p->accel_enable = accel_enable;
    if (cmdline) {
        vm_add_cmdline(p, cmdline);
    }

    p->platform = &platform;
    p->rtc_real_time = true;

    s = virt_machine_init(p);
    if (!s)
        exit(1);

    virt_machine_free_config(p);

    while (!s->shutdown_requested) {
        loop.Wait(s->GetSleepDuration(MAX_SLEEP_TIME));
        if (loop.QuitRequested())
            s->RequestShutdown(loop.ExitCode());
        if (!s->shutdown_requested)
            s->Interp(MAX_EXEC_CYCLE);
    }
    return s->exit_code;
}
