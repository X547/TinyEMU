/*
 * Test disk
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
#include "test_block.h"

#include <string.h>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "config_props.h"
#include "devices.h"
#include "host_time.h"
#include "host_timer.h"
#include "machine.h"
#include "test_control.h"

#define SECTOR_SIZE 512

/* The largest disk: 8 GB, which is past where SD and eMMC switch to sector
   addressing. Only the sectors written are kept in memory. */
#define TEST_BLOCK_MAX_SECTORS (16 * 1024 * 1024)

namespace {

class TestBlockDevice;

std::vector<TestBlockDevice *> sDisks;


/* A disk with the pattern of TEST_BLOCK_PATTERN, which finishes
   its requests the way the test-control device says. A held or delayed
   request moves data when it finishes, as a real disk's DMA would. */
class TestBlockDevice final: public HostBlockDevice, public TestBlockControl,
                             public TimerTarget {
private:
    struct Request {
        bool is_write;
        bool fail;
        uint64_t sector_num;
        int n;
        uint8_t *buf;
        BlockCompletion *completion;
    };

    /* what was written; every other sector holds the pattern */
    std::unordered_map<uint32_t, std::array<uint8_t, SECTOR_SIZE>> fWritten;
    uint32_t fSectors;
    bool fReadOnly;
    std::unique_ptr<HostTimer> fTimer;
    uint32_t fMode = TEST_BLOCK_MODE_SYNC;
    uint32_t fFail = 0;
    uint32_t fReads = 0;
    uint32_t fWrites = 0;
    uint32_t fCancels = 0;
    std::deque<Request> fHeld;
    /* to finish from the event loop */
    std::deque<Request> fReady;

    void Move(const Request &req)
    {
        for (int k = 0; k < req.n; k++) {
            uint32_t s = (uint32_t)req.sector_num + k;
            uint8_t *buf = req.buf + (size_t)k * SECTOR_SIZE;
            if (req.is_write) {
                memcpy(fWritten[s].data(), buf, SECTOR_SIZE);
                continue;
            }
            auto it = fWritten.find(s);
            if (it != fWritten.end()) {
                memcpy(buf, it->second.data(), SECTOR_SIZE);
                continue;
            }
            for (uint32_t i = 0; i < SECTOR_SIZE; i++)
                buf[i] = TEST_BLOCK_PATTERN(s, i);
        }
    }

    int Start(bool is_write, uint64_t sector_num, uint8_t *buf, int n,
              BlockCompletion *completion)
    {
        if (is_write)
            fWrites++;
        else
            fReads++;
        if (n < 0 || sector_num > fSectors ||
            (uint64_t)n > fSectors - sector_num || (is_write && fReadOnly))
            return -1;
        Request req = {is_write, false, sector_num, n, buf, completion};
        if (fFail > 0) {
            fFail--;
            req.fail = true;
        }
        switch (fMode) {
        case TEST_BLOCK_MODE_ASYNC:
            fReady.push_back(req);
            fTimer->Arm(host_monotonic_us());
            return 1;
        case TEST_BLOCK_MODE_HOLD:
            fHeld.push_back(req);
            return 1;
        default:
            if (req.fail)
                return -1;
            Move(req);
            return 0;
        }
    }

public:
    TestBlockDevice(uint32_t sectors, bool read_only,
                    std::unique_ptr<HostTimer> timer):
        fSectors(sectors),
        fReadOnly(read_only),
        fTimer(std::move(timer))
    {
        fTimer->target = this;
        sDisks.push_back(this);
    }

    ~TestBlockDevice() override
    {
        sDisks.erase(std::find(sDisks.begin(), sDisks.end(), this));
    }

    /* HostBlockDevice */
    int64_t SectorCount() override {return fSectors;}

    int ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                  BlockCompletion *completion) override
    {
        return Start(false, sector_num, buf, n, completion);
    }

    int WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                   BlockCompletion *completion) override
    {
        return Start(true, sector_num, const_cast<uint8_t *>(buf), n,
                     completion);
    }

    void Cancel(BlockCompletion *completion) override
    {
        auto drop = [this, completion](std::deque<Request> &q) {
            for (auto it = q.begin(); it != q.end();) {
                if (it->completion == completion) {
                    it = q.erase(it);
                    fCancels++;
                } else {
                    ++it;
                }
            }
        };
        drop(fHeld);
        drop(fReady);
    }

    /* TimerTarget */
    void TimerExpired() override
    {
        /* a completion may start the next request, which joins the queue */
        while (!fReady.empty()) {
            Request req = fReady.front();
            fReady.pop_front();
            if (!req.fail)
                Move(req);
            req.completion->Complete(req.fail ? -1 : 0);
        }
    }

    /* TestBlockControl */
    uint32_t Sectors() const override {return fSectors;}
    uint32_t Mode() const override {return fMode;}
    void SetMode(uint32_t mode) override
    {
        if (mode <= TEST_BLOCK_MODE_HOLD)
            fMode = mode;
    }
    uint32_t FailCount() const override {return fFail;}
    void SetFailCount(uint32_t count) override {fFail = count;}
    uint32_t Held() const override {return fHeld.size();}

    void Release(uint32_t count, bool newest_first) override
    {
        for (; count > 0 && !fHeld.empty(); count--) {
            if (newest_first) {
                fReady.push_back(fHeld.back());
                fHeld.pop_back();
            } else {
                fReady.push_back(fHeld.front());
                fHeld.pop_front();
            }
        }
        if (!fReady.empty())
            fTimer->Arm(host_monotonic_us());
    }

    uint32_t Reads() const override {return fReads;}
    uint32_t Writes() const override {return fWrites;}
    uint32_t Cancels() const override {return fCancels;}
};


/* "sectors": the size, 2048 (1 MB) unless given. */
class TestBlockDriver final: public BlockDriverClass {
public:
    TestBlockDriver(): BlockDriverClass("test") {}

    std::unique_ptr<HostBlockDevice> Open(const DeviceConfig &cfg,
                                          DeviceContext *ctx,
                                          bool read_only) const override
    {
        int sectors;
        if (!cfg.GetInt("sectors", &sectors, 2048))
            return nullptr;
        if (sectors <= 0 || sectors > TEST_BLOCK_MAX_SECTORS) {
            vm_error("%s: 'sectors' must be between 1 and %d\n", cfg.Type(),
                     TEST_BLOCK_MAX_SECTORS);
            return nullptr;
        }
        return std::make_unique<TestBlockDevice>(sectors, read_only,
                                                 ctx->platform->OpenTimer());
    }
};

const TestBlockDriver sTestBlockDriver;

}


int test_block_count()
{
    return sDisks.size();
}


TestBlockControl *test_block_get(int index)
{
    if (index < 0 || index >= (int)sDisks.size())
        return nullptr;
    return sDisks[index];
}
