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
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#ifndef _WIN32
#include <termios.h>
#include <sys/ioctl.h>
#include <net/if.h>
#ifndef __HAIKU__
#include <linux/if_tun.h>
#endif
#endif
#include <sys/stat.h>
#include <signal.h>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"
#ifdef CONFIG_FS_NET
#include "fs_utils.h"
#include "fs_wget.h"
#endif
#ifdef CONFIG_SLIRP
#include "slirp/libslirp.h"
#endif

#ifndef _WIN32

class STDIODevice final: public CharacterDevice {
public:
    int stdin_fd = 0;
    int console_esc_state = 0;
    bool resize_pending = false;

    void WriteData(const uint8_t *buf, int len) override;
    int ReadData(uint8_t *buf, int len) override;
};

static struct termios oldtty;
static int old_fd0_flags;
static STDIODevice *global_stdio_device;

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

void STDIODevice::WriteData(const uint8_t *buf, int len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}

int STDIODevice::ReadData(uint8_t *buf, int len)
{
    STDIODevice *s = this;
    int ret, i, j;
    uint8_t ch;
    
    if (len <= 0)
        return 0;

    ret = read(s->stdin_fd, buf, len);
    if (ret < 0)
        return 0;
    if (ret == 0) {
        /* EOF */
        exit(1);
    }

    j = 0;
    for(i = 0; i < ret; i++) {
        ch = buf[i];
        if (s->console_esc_state) {
            s->console_esc_state = 0;
            switch(ch) {
            case 'x':
                printf("Terminated\n");
                exit(0);
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
                s->console_esc_state = 1;
            } else {
            output_char:
                buf[j++] = ch;
            }
        }
    }
    return j;
}

static void term_resize_handler(int sig)
{
    if (global_stdio_device)
        global_stdio_device->resize_pending = true;
}

static void console_get_size(STDIODevice *s, int *pw, int *ph)
{
    struct winsize ws;
    int width, height;
    /* default values */
    width = 80;
    height = 25;
    if (ioctl(s->stdin_fd, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col >= 4 && ws.ws_row >= 4) {
        width = ws.ws_col;
        height = ws.ws_row;
    }
    *pw = width;
    *ph = height;
}

CharacterDevice *console_init(bool allow_ctrlc)
{
    STDIODevice *s;
    struct sigaction sig;

    term_init(allow_ctrlc);

    s = new STDIODevice();
    s->stdin_fd = 0;
    /* Note: the glibc does not properly tests the return value of
       write() in printf, so some messages on stdout may be lost */
    fcntl(s->stdin_fd, F_SETFL, O_NONBLOCK);

    s->resize_pending = true;
    global_stdio_device = s;
    
    /* use a signal to get the host terminal resize events */
    sig.sa_handler = term_resize_handler;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = 0;
    sigaction(SIGWINCH, &sig, NULL);
    
    return s;
}

#endif /* !_WIN32 */

typedef enum {
    BF_MODE_RO,
    BF_MODE_RW,
    BF_MODE_SNAPSHOT,
} BlockDeviceModeEnum;

#define SECTOR_SIZE 512

class BlockDeviceFile final: public BlockDevice {
public:
    FILE *f = nullptr;
    int64_t nb_sectors = 0;
    BlockDeviceModeEnum mode {};
    uint8_t **sector_table = nullptr;

    int64_t SectorCount() override {return nb_sectors;}
    int ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                  BlockDeviceCompletion *completion) override;
    int WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                   BlockDeviceCompletion *completion) override;
};

//#define DUMP_BLOCK_READ

int BlockDeviceFile::ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                               BlockDeviceCompletion *completion)
{
    (void)completion;
    BlockDeviceFile *bf = this;
    //    printf("bf_read_async: sector_num=%" PRId64 " n=%d\n", sector_num, n);
#ifdef DUMP_BLOCK_READ
    {
        static FILE *f;
        if (!f)
            f = fopen("/tmp/read_sect.txt", "wb");
        fprintf(f, "%" PRId64 " %d\n", sector_num, n);
    }
#endif
    if (!bf->f)
        return -1;
    if (bf->mode == BF_MODE_SNAPSHOT) {
        int i;
        for(i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num]) {
                fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
                fread(buf, 1, SECTOR_SIZE, bf->f);
            } else {
                memcpy(buf, bf->sector_table[sector_num], SECTOR_SIZE);
            }
            sector_num++;
            buf += SECTOR_SIZE;
        }
    } else {
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fread(buf, 1, n * SECTOR_SIZE, bf->f);
    }
    /* synchronous read */
    return 0;
}

int BlockDeviceFile::WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                                BlockDeviceCompletion *completion)
{
    (void)completion;
    BlockDeviceFile *bf = this;
    int ret;

    switch(bf->mode) {
    case BF_MODE_RO:
        ret = -1; /* error */
        break;
    case BF_MODE_RW:
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fwrite(buf, 1, n * SECTOR_SIZE, bf->f);
        ret = 0;
        break;
    case BF_MODE_SNAPSHOT:
        {
            int i;
            if ((sector_num + n) > bf->nb_sectors)
                return -1;
            for(i = 0; i < n; i++) {
                if (!bf->sector_table[sector_num]) {
                    bf->sector_table[sector_num] = static_cast<uint8_t *>(malloc(SECTOR_SIZE));
                }
                memcpy(bf->sector_table[sector_num], buf, SECTOR_SIZE);
                sector_num++;
                buf += SECTOR_SIZE;
            }
            ret = 0;
        }
        break;
    default:
        abort();
    }

    return ret;
}

static BlockDevice *block_device_init(const char *filename,
                                      BlockDeviceModeEnum mode)
{
    BlockDeviceFile *bf;
    int64_t file_size;
    FILE *f;
    const char *mode_str;

    if (mode == BF_MODE_RW) {
        mode_str = "r+b";
    } else {
        mode_str = "rb";
    }
    
    f = fopen(filename, mode_str);
    if (!f) {
        perror(filename);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    file_size = ftello(f);

    bf = new BlockDeviceFile();

    bf->mode = mode;
    bf->nb_sectors = file_size / 512;
    bf->f = f;

    if (mode == BF_MODE_SNAPSHOT) {
        bf->sector_table = static_cast<uint8_t **>(mallocz(sizeof(bf->sector_table[0]) *
                                   bf->nb_sectors));
    }
    
    return bf;
}

#if !defined(_WIN32) && !defined(__HAIKU__)

class TunState final: public EthernetDevice {
public:
    int fd;
    bool select_filled;

    void WritePacket(const uint8_t *buf, int len) override;
    void SelectFill(int *pfd_max, fd_set *rfds, fd_set *wfds,
                    fd_set *efds, int *pdelay) override;
    void SelectPoll(fd_set *rfds, fd_set *wfds, fd_set *efds,
                    int select_ret) override;
};

void TunState::WritePacket(const uint8_t *buf, int len)
{
    TunState *s = this;
    write(s->fd, buf, len);
}

void TunState::SelectFill(int *pfd_max, fd_set *rfds, fd_set *wfds,
                          fd_set *efds, int *pdelay)
{
    (void)wfds;
    (void)efds;
    (void)pdelay;
    TunState *s = this;
    int net_fd = s->fd;

    s->select_filled = target->CanWritePacket();
    if (s->select_filled) {
        FD_SET(net_fd, rfds);
        *pfd_max = max_int(*pfd_max, net_fd);
    }
}

void TunState::SelectPoll(fd_set *rfds, fd_set *wfds, fd_set *efds,
                          int select_ret)
{
    (void)wfds;
    (void)efds;
    TunState *s = this;
    int net_fd = s->fd;
    uint8_t buf[2048];
    int ret;
    
    if (select_ret <= 0)
        return;
    if (s->select_filled && FD_ISSET(net_fd, rfds)) {
        ret = read(net_fd, buf, sizeof(buf));
        if (ret > 0)
            target->WritePacket(buf, ret);
    }
    
}

/* configure with:
# bridge configuration (connect tap0 to bridge interface br0)
   ip link add br0 type bridge
   ip tuntap add dev tap0 mode tap [user x] [group x]
   ip link set tap0 master br0
   ip link set dev br0 up
   ip link set dev tap0 up

# NAT configuration (eth1 is the interface connected to internet)
   ifconfig br0 192.168.3.1
   echo 1 > /proc/sys/net/ipv4/ip_forward
   iptables -D FORWARD 1
   iptables -t nat -A POSTROUTING -o eth1 -j MASQUERADE

   In the VM:
   ifconfig eth0 192.168.3.2
   route add -net 0.0.0.0 netmask 0.0.0.0 gw 192.168.3.1
*/
static EthernetDevice *tun_open(const char *ifname)
{
    struct ifreq ifr;
    int fd, ret;
    TunState *s;
    
    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: could not open /dev/net/tun\n");
        return NULL;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    pstrcpy(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
    ret = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (ret != 0) {
        fprintf(stderr, "Error: could not configure /dev/net/tun\n");
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    s = new TunState();
    s->fd = fd;
    s->mac_addr[0] = 0x02;
    s->mac_addr[1] = 0x00;
    s->mac_addr[2] = 0x00;
    s->mac_addr[3] = 0x00;
    s->mac_addr[4] = 0x00;
    s->mac_addr[5] = 0x01;
    return s;
}

#endif /* !_WIN32 */

#ifdef CONFIG_SLIRP

/*******************************************************/
/* slirp */

static Slirp *slirp_state;

static void HexDump(const uint8_t *data, size_t size)
{
	size_t i;
	for (i = 0; i < size; i++) {
		printf(" %02x", data[i]);
		if (i % 0x10 == 0xf)
			printf("\n");
	}
	if (i % 0x10 != 0)
		printf("\n");
}

class SlirpEthernetDevice final: public EthernetDevice {
public:
    Slirp *state = nullptr;

    void WritePacket(const uint8_t *buf, int len) override
    {
        slirp_input(state, buf, len);
    }

    void SelectFill(int *pfd_max, fd_set *rfds, fd_set *wfds, fd_set *efds,
                    int *pdelay) override
    {
        (void)pdelay;
        slirp_select_fill(state, pfd_max, rfds, wfds, efds);
    }

    void SelectPoll(fd_set *rfds, fd_set *wfds, fd_set *efds,
                    int select_ret) override
    {
        slirp_select_poll(state, rfds, wfds, efds, (select_ret <= 0));
    }
};

/* Provided to slirp, which is compiled as C. */
extern "C" int slirp_can_output(void *opaque)
{
    EthernetDevice *net = static_cast<EthernetDevice *>(opaque);
    return net->target->CanWritePacket();
}

extern "C" void slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
    EthernetDevice *net = static_cast<EthernetDevice *>(opaque);
    net->target->WritePacket(pkt, pkt_len);
}


static EthernetDevice *slirp_open(void)
{
    SlirpEthernetDevice *net;
    struct in_addr net_addr  = { .s_addr = htonl(0x0a000200) }; /* 10.0.2.0 */
    struct in_addr mask = { .s_addr = htonl(0xffffff00) }; /* 255.255.255.0 */
    struct in_addr host = { .s_addr = htonl(0x0a000202) }; /* 10.0.2.2 */
    struct in_addr dhcp = { .s_addr = htonl(0x0a00020f) }; /* 10.0.2.15 */
    struct in_addr dns  = { .s_addr = htonl(0x0a000203) }; /* 10.0.2.3 */
    const char *bootfile = NULL;
    const char *vhostname = NULL;
    int restricted = 0;
    
    if (slirp_state) {
        fprintf(stderr, "Only a single slirp instance is allowed\n");
        return NULL;
    }
    net = new SlirpEthernetDevice();

    slirp_state = slirp_init(restricted, net_addr, mask, host, vhostname,
                             "", bootfile, dhcp, dns, net);
    net->state = slirp_state;
    
    net->mac_addr[0] = 0x02;
    net->mac_addr[1] = 0x00;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x00;
    net->mac_addr[4] = 0x00;
    net->mac_addr[5] = 0x01;

    return net;
}

#endif /* CONFIG_SLIRP */

#define MAX_EXEC_CYCLE 500000
#define MAX_SLEEP_TIME 10 /* in ms */

void virt_machine_run(VirtMachine *m)
{
    fd_set rfds, wfds, efds;
    int fd_max, ret, delay;
    struct timeval tv;
#ifndef _WIN32
    int stdin_fd = -1;
#endif
    
    delay = m->GetSleepDuration(MAX_SLEEP_TIME);
    
    /* wait for an event */
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    fd_max = -1;
#ifndef _WIN32
    if (m->console_dev && virtio_console_can_write_data(m->console_dev)) {
        STDIODevice *s = static_cast<STDIODevice *>(m->console);
        stdin_fd = s->stdin_fd;
        FD_SET(stdin_fd, &rfds);
        fd_max = stdin_fd;

        if (s->resize_pending) {
            int width, height;
            console_get_size(s, &width, &height);
            virtio_console_resize_event(m->console_dev, width, height);
            s->resize_pending = false;
        }
    }
#endif
    if (m->net) {
        m->net->SelectFill(&fd_max, &rfds, &wfds, &efds, &delay);
    }
#ifdef CONFIG_FS_NET
    fs_net_set_fdset(&fd_max, &rfds, &wfds, &efds, &delay);
#endif
    tv.tv_sec = delay / 1000;
    tv.tv_usec = (delay % 1000) * 1000;
    ret = select(fd_max + 1, &rfds, &wfds, &efds, &tv);
    if (m->net) {
        m->net->SelectPoll(&rfds, &wfds, &efds, ret);
    }
    if (ret > 0) {
#ifndef _WIN32
        if (m->console_dev && stdin_fd >= 0 && FD_ISSET(stdin_fd, &rfds)) {
            uint8_t buf[128];
            int ret, len;
            len = virtio_console_get_write_len(m->console_dev);
            len = min_int(len, sizeof(buf));
            ret = m->console->ReadData(buf, len);
            if (ret > 0) {
                virtio_console_write_data(m->console_dev, buf, ret);
            }
        }
#endif
    }

#ifdef CONFIG_SDL
    sdl_refresh(m);
#endif
    
    m->Interp(MAX_EXEC_CYCLE);
}

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

#ifdef CONFIG_FS_NET
static bool net_completed;

class NetStartCallback final: public StartCallback {
public:
    void Start() override {net_completed = true;}
};

static NetStartCallback sNetStartCallback;

class NetPollCompletion final: public FSNetEventLoopCompletion {
public:
    bool IsCompleted() override {return net_completed;}
};

static NetPollCompletion sNetPollCompletion;

#endif

/* Opening a back end needs things the configuration parser has no business
   knowing about (block device modes, the network event loop), so it happens
   here, once, over the same tree the machine will be built from. */
struct BackendOpenState {
    VirtMachineParams *p;
    BlockDeviceModeEnum drive_mode;
    const char *build_preload_file;
};

static void open_block_backend(VMDeviceNode *node, BackendOpenState *st)
{
    char *fname;

    if (node->filename == nullptr) {
        fprintf(stderr, "%s: expecting a 'file' property\n", node->type);
        exit(1);
    }
    fname = get_file_path(st->p->cfg_filename, node->filename);
#ifdef CONFIG_FS_NET
    if (is_url(fname)) {
        net_completed = false;
        node->block_dev = block_device_init_http(fname, 128 * 1024,
                                                 &sNetStartCallback);
        /* wait until the drive is initialized */
        fs_net_event_loop(&sNetPollCompletion);
    } else
#endif
    {
        node->block_dev = block_device_init(fname, st->drive_mode);
    }
    free(fname);
    if (node->block_dev == nullptr) {
        fprintf(stderr, "%s: could not open\n", node->filename);
        exit(1);
    }
}

static void open_fs_backend(VMDeviceNode *node, BackendOpenState *st)
{
    const char *path = node->filename;

    if (path == nullptr) {
        fprintf(stderr, "%s: expecting a 'file' property\n", node->type);
        exit(1);
    }
#ifdef CONFIG_FS_NET
    if (is_url(path)) {
        node->fs_dev = fs_net_init(path, nullptr);
        if (!node->fs_dev)
            exit(1);
        if (st->build_preload_file)
            fs_dump_cache_load(node->fs_dev, st->build_preload_file);
        fs_net_event_loop(nullptr);
        return;
    }
#endif
#if defined(_WIN32)
    fprintf(stderr, "Filesystem access not supported yet\n");
    exit(1);
#else
    {
        char *fname = get_file_path(st->p->cfg_filename, path);
        node->fs_dev = fs_disk_init(fname);
        if (!node->fs_dev) {
            fprintf(stderr, "%s: must be a directory\n", fname);
            exit(1);
        }
        free(fname);
    }
#endif
}

static void open_net_backend(VMDeviceNode *node, BackendOpenState *st)
{
    const char *driver, *ifname;

    (void)st;
    if (vm_get_str(node->props, "driver", &driver) < 0)
        exit(1);
#ifdef CONFIG_SLIRP
    if (!strcmp(driver, "user")) {
        node->net = slirp_open();
        if (!node->net)
            exit(1);
        return;
    }
#endif
#if !defined(_WIN32) && !defined(__HAIKU__)
    if (!strcmp(driver, "tap")) {
        if (vm_get_str(node->props, "ifname", &ifname) < 0)
            exit(1);
        node->net = tun_open(ifname);
        if (!node->net)
            exit(1);
        return;
    }
#else
    (void)ifname;
#endif
    fprintf(stderr, "Unsupported network driver '%s'\n", driver);
    exit(1);
}

static void open_device_backend(VMDeviceNode *node, void *opaque)
{
    BackendOpenState *st = static_cast<BackendOpenState *>(opaque);

    if (!strcmp(node->type, "virtio-block") || !strcmp(node->type, "ide")) {
        open_block_backend(node, st);
    } else if (!strcmp(node->type, "virtio-9p")) {
        open_fs_backend(node, st);
    } else if (!strcmp(node->type, "virtio-net")) {
        open_net_backend(node, st);
    }
}

int main(int argc, char **argv)
{
    VirtMachine *s;
    const char *path, *cmdline, *build_preload_file;
    int c, option_index, ram_size, accel_enable;
    bool allow_ctrlc;
    BlockDeviceModeEnum drive_mode;
    VirtMachineParams p_s, *p = &p_s;

    ram_size = -1;
    allow_ctrlc = false;
    (void)allow_ctrlc;
    drive_mode = BF_MODE_SNAPSHOT;
    accel_enable = -1;
    cmdline = NULL;
    build_preload_file = NULL;
    for(;;) {
        c = getopt_long_only(argc, argv, "hm:", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 0:
            switch(option_index) {
            case 1: /* ctrlc */
                allow_ctrlc = true;
                break;
            case 2: /* rw */
                drive_mode = BF_MODE_RW;
                break;
            case 3: /* ro */
                drive_mode = BF_MODE_RO;
                break;
            case 4: /* append */
                cmdline = optarg;
                break;
            case 5: /* no-accel */
                accel_enable = false;
                break;
            case 6: /* build-preload */
                build_preload_file = optarg;
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

    virt_machine_set_defaults(p);
#ifdef CONFIG_FS_NET
    fs_wget_init();
#endif
    virt_machine_load_config_file(p, path, nullptr);
#ifdef CONFIG_FS_NET
    fs_net_event_loop(nullptr);
#endif

    /* override some config parameters */

    if (ram_size > 0) {
        p->ram_size = (uint64_t)ram_size << 20;
    }
    if (accel_enable != -1)
        p->accel_enable = accel_enable;
    if (cmdline) {
        vm_add_cmdline(p, cmdline);
    }
    
    /* open the files & devices */
    {
        BackendOpenState st;
        st.p = p;
        st.drive_mode = drive_mode;
        st.build_preload_file = build_preload_file;
        vm_walk_devices(p->root_devices, open_device_backend, &st);
    }

#ifdef CONFIG_SDL
    if (p->display_device) {
        sdl_init(p->width, p->height);
    }
#endif
#ifdef _WIN32
    fprintf(stderr, "Console not supported yet\n");
    exit(1);
#else
    p->console = console_init(allow_ctrlc);
#endif
    p->rtc_real_time = true;

    s = virt_machine_init(p);
    if (!s)
        exit(1);
    
    virt_machine_free_config(p);

    if (s->net != nullptr && s->net->target != nullptr) {
        s->net->target->SetCarrier(true);
    }
    
    for(;;) {
        virt_machine_run(s);
    }
    delete s;
    return 0;
}
