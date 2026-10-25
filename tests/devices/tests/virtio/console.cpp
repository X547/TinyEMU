/*
 * virtio-console tests
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
#include "heap.h"
#include "test.h"
#include "virtio.h"

/* The console device of the virtio specification (1.2, 5.3): port 0's
   receive and transmit queues, and the size of the console in its
   configuration when VIRTIO_CONSOLE_F_SIZE is offered. What temu's console
   gets typed is what the receive queue's buffers fill with. */

#define CONSOLE_RX 0
#define CONSOLE_TX 1

#define VIRTIO_CONSOLE_F_SIZE        (1u << 0)
#define VIRTIO_CONSOLE_F_MULTIPORT   (1u << 1)
#define VIRTIO_CONSOLE_F_EMERG_WRITE (1u << 2)

#define QUEUE_SIZE 8

namespace {

VirtioDev sDev;
Virtq sRx;
Virtq sTx;

void cleanup()
{
    virtio_reset(&sDev);
    if (sDev.pci)
        pci_restore_all();
}

/* Both queues set up and DRIVER_OK, nothing in the receive queue. */
void start()
{
    virtio_require(VIRTIO_ID_CONSOLE, &sDev);
    t_on_cleanup(cleanup);
    ASSERT_TRUE(virtio_negotiate(&sDev, VIRTIO_F_VERSION_1 |
                                        VIRTIO_CONSOLE_F_SIZE));
    ASSERT_TRUE(virtq_setup(&sDev, CONSOLE_RX, QUEUE_SIZE, &sRx));
    ASSERT_TRUE(virtq_setup(&sDev, CONSOLE_TX, QUEUE_SIZE, &sTx));
    virtio_set_status(&sDev, virtio_status(&sDev) | VIRTIO_STATUS_DRIVER_OK);
}

char *rx_buffer(uint16_t head, uint32_t len)
{
    char *buf = (char *)heap_alloc(len, 16);
    if (buf == nullptr)
        t_skip("no memory");
    memset(buf, 0, len);
    sRx.desc[head].addr = (uintptr_t)buf;
    sRx.desc[head].len = len;
    sRx.desc[head].flags = VIRTQ_DESC_F_WRITE;
    sRx.desc[head].next = 0;
    virtq_publish(&sRx, head);
    virtq_kick(&sRx);
    return buf;
}

}


/* The console size is offered, the ports and emergency write are not,
   and the configuration holds the columns and rows. */
TEST(virtio_console, features)
{
    virtio_require(VIRTIO_ID_CONSOLE, &sDev);
    uint32_t f = virtio_device_features(&sDev, 0);
    EXPECT_TRUE(f & VIRTIO_CONSOLE_F_SIZE);
    EXPECT_FALSE(f & VIRTIO_CONSOLE_F_MULTIPORT);
    SNAP("emerg_write", "%u", (f & VIRTIO_CONSOLE_F_EMERG_WRITE) != 0);
    SNAP("size", "%ux%u", virtio_config_read(&sDev, 0, 1),
         virtio_config_read(&sDev, 2, 1));
}

/* What is typed fills the receive buffer; its used length is the number
   of bytes written into it. */
TEST(virtio_console, receive)
{
    start();
    char *buf = rx_buffer(2, 32);
    t_host_input("ping", 4);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sRx, &e, 5000000));
    EXPECT_EQ(e.id, 2u);
    EXPECT_LE(e.len, 4u);
    /* the rest may come in another buffer */
    uint32_t got = e.len;
    char all[8] = {};
    memcpy(all, buf, e.len);
    while (got < 4) {
        char *more = rx_buffer(3, 32);
        ASSERT_TRUE(virtq_get_used(&sRx, &e, 5000000));
        memcpy(all + got, more, min(e.len, 4u - got));
        got += e.len;
    }
    EXPECT_EQ(got, 4u);
    EXPECT_EQ(memcmp(all, "ping", 4), 0);
}

/* A receive chain that loops makes the device ask for a reset when input
   comes; what was typed waits for a buffer it can use. */
TEST(virtio_console, receive_malformed)
{
    start();
    char *buf = (char *)heap_alloc(8, 16);
    ASSERT_TRUE(buf != nullptr);
    sRx.desc[0] = VirtqDesc {(uintptr_t)buf, 8,
                             VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT, 1};
    sRx.desc[1] = VirtqDesc {(uintptr_t)buf, 8,
                             VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT, 0};
    virtq_publish(&sRx, 0);
    virtq_kick(&sRx);
    t_host_input("m", 1);
    EXPECT_TRUE(t_wait_until([] {
        return (virtio_status(&sDev) & VIRTIO_STATUS_NEEDS_RESET) != 0;
    }, 5000000));
    EXPECT_EQ(virtq_used_idx(&sRx), 0);

    start();
    char *good = rx_buffer(0, 8);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sRx, &e, 5000000));
    EXPECT_EQ(e.len, 1u);
    EXPECT_EQ(good[0], 'm');
}

/* A buffer smaller than what is typed takes what fits; the rest waits for
   the next one. */
TEST(virtio_console, receive_split)
{
    start();
    char *a = rx_buffer(0, 2);
    t_host_input("wxyz", 4);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sRx, &e, 5000000));
    EXPECT_EQ(e.id, 0u);
    ASSERT_GE(e.len, 1u);
    EXPECT_LE(e.len, 2u);
    char all[8] = {};
    uint32_t got = e.len;
    memcpy(all, a, e.len);
    for (uint16_t head = 1; got < 4 && head < QUEUE_SIZE; head++) {
        char *b = rx_buffer(head, 2);
        ASSERT_TRUE(virtq_get_used(&sRx, &e, 5000000));
        EXPECT_LE(e.len, 2u);
        memcpy(all + got, b, min(e.len, 4u - got));
        got += e.len;
    }
    EXPECT_EQ(got, 4u);
    EXPECT_EQ(memcmp(all, "wxyz", 4), 0);
}

/* Typed with no buffer queued, the input waits for one. */
TEST(virtio_console, receive_waits)
{
    start();
    t_host_input("q", 1);
    t_delay_us(200000);
    EXPECT_EQ(virtq_used_idx(&sRx), 0);
    char *buf = rx_buffer(1, 8);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sRx, &e, 5000000));
    EXPECT_EQ(e.len, 1u);
    EXPECT_EQ(buf[0], 'q');
}

/* Output is taken and handed back unwritten, whatever its length. */
TEST(virtio_console, transmit)
{
    start();
    static const char msg[] = "virtio-console transmit\n";
    char *buf = (char *)heap_alloc(sizeof(msg), 16);
    ASSERT_TRUE(buf != nullptr);
    memcpy(buf, msg, sizeof(msg) - 1);
    sTx.desc[0] = VirtqDesc {(uintptr_t)buf, sizeof(msg) - 1, 0, 0};
    virtq_publish(&sTx, 0);
    sTx.desc[1] = VirtqDesc {(uintptr_t)buf, 0, 0, 0};
    virtq_publish(&sTx, 1);
    virtq_kick(&sTx);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sTx, &e));
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(e.len, 0u);
    ASSERT_TRUE(virtq_get_used(&sTx, &e));
    EXPECT_EQ(e.id, 1u);
    EXPECT_EQ(e.len, 0u);

    /* longer than the device copies at once, over two descriptors */
    char *big = (char *)heap_alloc(10000, 16);
    ASSERT_TRUE(big != nullptr);
    memset(big, '.', 10000);
    big[9999] = '\n';
    sTx.desc[2] = VirtqDesc {(uintptr_t)big, 5000, VIRTQ_DESC_F_NEXT, 3};
    sTx.desc[3] = VirtqDesc {(uintptr_t)big + 5000, 5000, 0, 0};
    virtq_publish(&sTx, 2);
    virtq_kick(&sTx);
    ASSERT_TRUE(virtq_get_used(&sTx, &e));
    EXPECT_EQ(e.id, 2u);
    EXPECT_EQ(e.len, 0u);
}
