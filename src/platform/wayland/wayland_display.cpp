/*
 * Host display: a Wayland window
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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#include "be_decorator.h"
#include "device_lock.h"
#ifdef CONFIG_WAYLAND_DMABUF
#include "dmabuf_allocator.h"
#endif
#include "platform_backends.h"
#include "run_control.h"
#include "screen_rect.h"
#include "title_font.h"
#include "window_buffer.h"
#include "WaylandKeycodes.h"

#define KEYCODE_MAX 127

/* The window is ARGB, so that the frame can have see-through parts. */
static const uint32_t kOpaque = 0xff000000;
/* wheel motion per notch, in axis_value120 units and in axis units */
static const int kWheelStep120 = 120;
static const double kWheelStepAxis = 10.0;
/* a second press this soon and this near is a double click */
static const uint32_t kDoubleClickMs = 400;
static const int kDoubleClickDistance = 4;
/* how far the pointer goes with the button held before the window moves */
static const int kDragDistance = 3;
/* the smallest content a resizable window is given */
static const int kMinContentSize = 64;


/* An xRGB pixel, which need not be aligned. */
static inline uint32_t load_pixel(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}


/* A straight alpha ARGB pixel premultiplied, as wl_shm takes it. */
static uint32_t premultiply(uint32_t p)
{
    uint32_t a = p >> 24;
    uint32_t out = a << 24;

    for (int shift = 0; shift < 24; shift += 8)
        out |= ((((p >> shift) & 0xff) * a + 127) / 255) << shift;
    return out;
}


static void damage_buffer(wl_surface *surface, uint32_t version,
                          const ScreenRect &r)
{
    if (version >= 4)
        wl_surface_damage_buffer(surface, r.x0, r.y0, r.Width(), r.Height());
    else
        wl_surface_damage(surface, r.x0, r.y0, r.Width(), r.Height());
}


class WaylandDisplay final: public HostDisplay {
private:
    /* the host cursor shown over the window: the guest's over the content,
       the window's own over the frame */
    enum CursorKind {
        kCursorGuest,
        kCursorFrame,
    };

    /* the window's own cursor image */
    struct FrameCursorSurface {
        wl_surface *surface = nullptr;
        std::unique_ptr<BufferRing> buffers;
        WindowBuffer *buffer = nullptr;
        int hot_x = 0;
        int hot_y = 0;
    };

    DeviceLock &fDeviceLock;
    RunControl &fRunControl;
    const DisplayOptions fOptions;

    /* Everything below the mutex is shared with the other threads. */
    std::mutex fMutex;
    std::condition_variable fCond;
    ScreenSource *fSource = nullptr;
    int fSourceWidth = 0;
    int fSourceHeight = 0;
    bool fResizable = false;
    /* the source's pixels, read in SetFramebuffer() and Update() */
    const uint8_t *fData = nullptr;
    int fDataStride = 0;
    /* the copy the window is drawn from, opaque, fFbWidth pixels per row */
    std::vector<uint32_t> fFb;
    int fFbWidth = 0;
    int fFbHeight = 0;
    /* changes whenever fFb changes size */
    uint32_t fFbGeneration = 0;
    ScreenRect fDirty;
    std::vector<uint32_t> fCursor;
    int fCursorWidth = 0;
    int fCursorHeight = 0;
    int fCursorX = 0;
    int fCursorY = 0;
    int fCursorHotX = 0;
    int fCursorHotY = 0;
    /* shown as the host cursor, which is where an absolute pointer is */
    bool fCursorNative = false;
    bool fCursorChanged = false;
    bool fCursorImageChanged = false;
    /* a byte is in the wakeup pipe and not yet handled */
    bool fWakePending = false;
    bool fQuitRequested = false;

    /* the window's thread reads, any thread writes */
    int fWakeRead = -1;
    int fWakeWrite = -1;

    /* with the device lock held */
    KeyboardTarget *fKeyboard = nullptr;
    PointerTarget *fPointer = nullptr;

    /* the window's thread */
    wl_display *fDisplay = nullptr;
    wl_registry *fRegistry = nullptr;
    wl_compositor *fCompositor = nullptr;
    uint32_t fCompositorVersion = 0;
    wl_shm *fShm = nullptr;
    /* the zwp_linux_dmabuf_v1 global, bound by the dma-buf allocator */
    uint32_t fDmabufName = 0;
    uint32_t fDmabufVersion = 0;
    xdg_wm_base *fWmBase = nullptr;
    zxdg_decoration_manager_v1 *fDecorationManager = nullptr;
    wl_seat *fSeat = nullptr;
    uint32_t fSeatVersion = 0;
    wl_keyboard *fKeyboardDevice = nullptr;
    wl_pointer *fPointerDevice = nullptr;

    wl_surface *fSurface = nullptr;
    xdg_surface *fXdgSurface = nullptr;
    xdg_toplevel *fToplevel = nullptr;
    zxdg_toplevel_decoration_v1 *fDecoration = nullptr;
    std::unique_ptr<ShmAllocator> fShmAllocator;
    /* the window's buffers when the compositor's GPU reads them in place */
    std::unique_ptr<BufferAllocator> fDmabufAllocator;
    std::unique_ptr<BufferRing> fBuffers;
    wl_callback *fFrameCallback = nullptr;
    bool fConfigured = false;
    /* what the last configure asked for; a size of 0 is the window's to
       choose */
    int fConfigureWidth = 0;
    int fConfigureHeight = 0;
    bool fConfigureActivated = false;
    bool fConfigureFullscreen = false;
    bool fConfigureMaximized = false;
    /* the compositor draws the frame */
    bool fServerSide = false;
    bool fCanMinimize = true;
    bool fWindowResizable = false;
    bool fFullscreen = false;
    bool fMaximized = false;

    /* the content, fWidth x fHeight, sits at (fContentX, fContentY) in a
       window of fWindowWidth x fWindowHeight */
    int fWidth = 0;
    int fHeight = 0;
    int fContentX = 0;
    int fContentY = 0;
    int fWindowWidth = 0;
    int fWindowHeight = 0;
    /* the frame is drawn here rather than by the compositor */
    bool fFrame = false;
    BeDecorator fDecorator;
    /* loaded when the frame is first drawn here */
    TitleFont fTitleFont;
    bool fTitleFontLoaded = false;
    /* the size and frame the window's regions were last set for */
    int fRegionWidth = 0;
    int fRegionHeight = 0;
    bool fRegionFrame = false;
    /* what to draw at the next frame, in the window */
    ScreenRect fWindowDirty;
    uint32_t fShownGeneration = 0;
    /* where the guest's cursor was drawn, in the frame buffer */
    ScreenRect fCursorDrawn;

    /* the host cursor over the window: the guest's image, or none */
    wl_surface *fCursorSurface = nullptr;
    std::unique_ptr<BufferRing> fCursorBuffers;
    /* drawn, attached once the cursor is shown */
    WindowBuffer *fCursorBuffer = nullptr;
    std::vector<uint32_t> fHostCursor;
    int fHostCursorWidth = 0;
    int fHostCursorHeight = 0;
    int fHostCursorHotX = 0;
    int fHostCursorHotY = 0;
    /* waits for a free cursor buffer */
    bool fHostCursorPending = false;
    FrameCursorSurface fFrameCursor;
    CursorKind fCursorKind = kCursorGuest;
    /* the cursor surface last shown with an image */
    wl_surface *fShownCursorSurface = nullptr;

    bool fPointerInside = false;
    uint32_t fEnterSerial = 0;
    /* in the window */
    int fPointerX = 0;
    int fPointerY = 0;
    BeDecorator::Part fPointerPart = BeDecorator::kContent;
    /* held down on the content; the guest has the pointer until they are
       all released */
    unsigned int fButtons = 0;
    /* collected until the pointer's frame event */
    bool fPointerChanged = false;
    int fWheel120 = 0;
    int fWheelDiscrete = 0;
    bool fWheelHasSteps = false;
    double fWheelAxis = 0;
    bool fHaveLastPos = false;
    int fLastX = 0;
    int fLastY = 0;
    /* the left button went down on the close button */
    bool fCloseTracking = false;
    /* the left button went down on the frame; the window moves once the
       pointer goes far enough */
    bool fDragPending = false;
    uint32_t fDragSerial = 0;
    int fDragX = 0;
    int fDragY = 0;
    /* the last press on the frame, for double clicks */
    bool fHaveLastClick = false;
    uint32_t fLastClickTime = 0;
    int fLastClickX = 0;
    int fLastClickY = 0;
    uint8_t fKeyPressed[KEYCODE_MAX + 1] {};

    static const wl_registry_listener kRegistryListener;
    static const xdg_wm_base_listener kWmBaseListener;
    static const xdg_surface_listener kXdgSurfaceListener;
    static const xdg_toplevel_listener kToplevelListener;
    static const zxdg_toplevel_decoration_v1_listener kDecorationListener;
    static const wl_callback_listener kFrameListener;
    static const wl_seat_listener kSeatListener;
    static const wl_keyboard_listener kKeyboardListener;
    static const wl_pointer_listener kPointerListener;

    void PostUpdate();
    void CopyFromSource(int x, int y, int w, int h);
    ScreenRect CursorRect() const;
    ScreenRect ContentRect() const;
    void Paint(WindowBuffer *buffer, const ScreenRect &area);

    /* the window's thread */
    void HandleGlobal(wl_registry *registry, uint32_t name,
                      const char *interface, uint32_t version);
    void HandleSeatCapabilities(uint32_t caps);
    void CreateWindow(int width, int height, bool resizable);
    void CreateBuffers();
    void CreateFrameCursor(FrameCursorSurface &cursor,
                           const FrameCursor &image);
    void Layout();
    void SetSizeLimits();
    void SetRegions();
    void HandleToplevelConfigure(int width, int height, wl_array *states);
    void HandleConfigure(uint32_t serial);
    void TellSourceSize();
    bool HandleUpdate();
    void Redraw();
    void HandleFrameDone();
    void SetHostCursor();
    void ShowHostCursor();
    void SetCursorKind(CursorKind kind);
    void ReplaceCursorSurface(wl_surface *old);
    void SendKey(int code, bool down);
    void ReleaseKeys();
    void HandleKey(uint32_t key, uint32_t state);
    bool PointerOnGuest() const;
    void UpdatePointerPart();
    void HandlePointerEnter(uint32_t serial, wl_fixed_t x, wl_fixed_t y);
    void HandlePointerLeave();
    void HandlePointerMotion(wl_fixed_t x, wl_fixed_t y);
    void HandlePointerButton(uint32_t serial, uint32_t time, uint32_t button,
                             uint32_t state);
    void PressFrame(uint32_t serial, uint32_t time);
    void ReleaseFrame();
    void SetClosePressed(bool pressed);
    void HandlePointerFrame();
    void SendMouse(int dz);
    bool Fail();
    bool EventLoop();

public:
    WaylandDisplay(DeviceLock &lock, RunControl &run_control,
                   const DisplayOptions &options):
        fDeviceLock(lock), fRunControl(run_control), fOptions(options) {}
    ~WaylandDisplay() override;

    /* Connects to the compositor; false when there is none to use. */
    bool Connect();

    /* HostScreen */
    void SetSource(ScreenSource *source, int width, int height) override;
    void SetFramebuffer(uint8_t *data, int width, int height,
                        int stride) override;
    void Update(int x, int y, int w, int h) override;
    void SetCursor(const uint32_t *pixels, int width, int height,
                   int hot_x, int hot_y) override;
    void MoveCursor(int x, int y) override;

    /* HostKeyboard, HostPointer */
    void SetTarget(KeyboardTarget *target) override;
    void SetTarget(PointerTarget *target) override;

    /* HostDisplay */
    void Run() override;
    void Quit() override;
};


WaylandDisplay::~WaylandDisplay()
{
    fBuffers.reset();
    fCursorBuffers.reset();
    fFrameCursor.buffers.reset();
    fDmabufAllocator.reset();
    fShmAllocator.reset();
    if (fFrameCursor.surface != nullptr)
        wl_surface_destroy(fFrameCursor.surface);
    if (fFrameCallback != nullptr)
        wl_callback_destroy(fFrameCallback);
    if (fDecoration != nullptr)
        zxdg_toplevel_decoration_v1_destroy(fDecoration);
    if (fToplevel != nullptr)
        xdg_toplevel_destroy(fToplevel);
    if (fXdgSurface != nullptr)
        xdg_surface_destroy(fXdgSurface);
    if (fSurface != nullptr)
        wl_surface_destroy(fSurface);
    if (fCursorSurface != nullptr)
        wl_surface_destroy(fCursorSurface);
    if (fKeyboardDevice != nullptr)
        wl_keyboard_destroy(fKeyboardDevice);
    if (fPointerDevice != nullptr)
        wl_pointer_destroy(fPointerDevice);
    if (fSeat != nullptr)
        wl_seat_destroy(fSeat);
    if (fDecorationManager != nullptr)
        zxdg_decoration_manager_v1_destroy(fDecorationManager);
    if (fWmBase != nullptr)
        xdg_wm_base_destroy(fWmBase);
    if (fShm != nullptr)
        wl_shm_destroy(fShm);
    if (fCompositor != nullptr)
        wl_compositor_destroy(fCompositor);
    if (fRegistry != nullptr)
        wl_registry_destroy(fRegistry);
    if (fDisplay != nullptr)
        wl_display_disconnect(fDisplay);
    if (fWakeRead >= 0)
        close(fWakeRead);
    if (fWakeWrite >= 0)
        close(fWakeWrite);
}


//#pragma mark - calls from the devices

/* With fMutex held. */
void WaylandDisplay::PostUpdate()
{
    if (fWakePending)
        return;
    fWakePending = true;
    char c = 0;
    while (write(fWakeWrite, &c, 1) < 0 && errno == EINTR) {}
}


void WaylandDisplay::SetTarget(KeyboardTarget *target)
{
    fKeyboard = target;
}


void WaylandDisplay::SetTarget(PointerTarget *target)
{
    fPointer = target;
}


void WaylandDisplay::SetSource(ScreenSource *source, int width, int height)
{
    std::lock_guard<std::mutex> locker(fMutex);
    fSource = source;
    fSourceWidth = width;
    fSourceHeight = height;
    fResizable = source != nullptr && source->Resizable();
}


/* With fMutex held: copies a rectangle, already clipped, into fFb. */
void WaylandDisplay::CopyFromSource(int x, int y, int w, int h)
{
    for (int i = 0; i < h; i++) {
        const uint8_t *src = fData + (y + i) * fDataStride + x * 4;
        uint32_t *dst = &fFb[(y + i) * fFbWidth + x];
        for (int j = 0; j < w; j++)
            dst[j] = load_pixel(src + j * 4) | kOpaque;
    }
}


void WaylandDisplay::SetFramebuffer(uint8_t *data, int width, int height,
                                    int stride)
{
    std::lock_guard<std::mutex> locker(fMutex);

    if (width <= 0 || height <= 0) {
        data = nullptr;
        width = height = 0;
    }
    bool resized = width != fFbWidth || height != fFbHeight;
    if (!resized && data == fData && stride == fDataStride)
        return;

    if (resized) {
        fFb.assign((size_t)width * height, kOpaque);
        fFbWidth = width;
        fFbHeight = height;
        fFbGeneration++;
    }
    fData = data;
    fDataStride = stride;
    if (data != nullptr) {
        CopyFromSource(0, 0, width, height);
    } else if (!resized) {
        std::fill(fFb.begin(), fFb.end(), kOpaque);
    }
    fDirty |= ScreenRect(0, 0, width, height);
    PostUpdate();
}


void WaylandDisplay::Update(int x, int y, int w, int h)
{
    std::lock_guard<std::mutex> locker(fMutex);

    ScreenRect r = ScreenRect(x, y, w, h) & ScreenRect(0, 0, fFbWidth,
                                                       fFbHeight);
    if (r.Empty() || fData == nullptr)
        return;
    CopyFromSource(r.x0, r.y0, r.Width(), r.Height());
    fDirty |= r;
    PostUpdate();
}


void WaylandDisplay::SetCursor(const uint32_t *pixels, int width, int height,
                               int hot_x, int hot_y)
{
    /* the device lock is held, as for any call from a device */
    bool native = fPointer != nullptr && fPointer->MouseIsAbsolute();
    std::lock_guard<std::mutex> locker(fMutex);

    if (pixels == nullptr || width <= 0 || height <= 0) {
        fCursor.clear();
        fCursorWidth = fCursorHeight = 0;
    } else {
        fCursor.assign(pixels, pixels + width * height);
        fCursorWidth = width;
        fCursorHeight = height;
    }
    fCursorHotX = hot_x;
    fCursorHotY = hot_y;
    fCursorNative = native;
    fCursorChanged = true;
    fCursorImageChanged = true;
    PostUpdate();
}


void WaylandDisplay::MoveCursor(int x, int y)
{
    std::lock_guard<std::mutex> locker(fMutex);

    if (x == fCursorX && y == fCursorY)
        return;
    fCursorX = x;
    fCursorY = y;
    /* the host cursor follows the host pointer by itself */
    if (!fCursorNative) {
        fCursorChanged = true;
        PostUpdate();
    }
}


void WaylandDisplay::Quit()
{
    std::lock_guard<std::mutex> locker(fMutex);
    fQuitRequested = true;
    fCond.notify_all();
    PostUpdate();
}


//#pragma mark - drawing

/* With fMutex held: where the cursor is drawn over the frame buffer. */
ScreenRect WaylandDisplay::CursorRect() const
{
    if (fCursor.empty() || fCursorNative)
        return ScreenRect();
    return ScreenRect(fCursorX, fCursorY, fCursorWidth, fCursorHeight);
}


/* Where the frame buffer is shown in the window. */
ScreenRect WaylandDisplay::ContentRect() const
{
    return ScreenRect(fContentX, fContentY, fWidth, fHeight);
}


/* With fMutex held: draws the frame, and in the content the frame buffer,
   black past its edges, and the cursor over both, into one rectangle of
   'buffer'. */
void WaylandDisplay::Paint(WindowBuffer *buffer, const ScreenRect &area)
{
    int pitch = buffer->pitch;
    ScreenRect content = ContentRect();

    bool in_content = area.x0 >= content.x0 && area.y0 >= content.y0 &&
                      area.x1 <= content.x1 && area.y1 <= content.y1;
    if (fFrame && !in_content)
        fDecorator.Paint(buffer->pixels, pitch, area);

    /* in the frame buffer's coordinates from here on */
    ScreenRect r = area & content;
    if (r.Empty())
        return;
    r = ScreenRect(r.x0 - fContentX, r.y0 - fContentY, r.Width(), r.Height());
    uint32_t *base = buffer->pixels + fContentY * pitch + fContentX;

    ScreenRect fb = r & ScreenRect(0, 0, fFbWidth, fFbHeight);
    if (!fb.Empty()) {
        for (int y = fb.y0; y < fb.y1; y++) {
            memcpy(&base[y * pitch + fb.x0], &fFb[y * fFbWidth + fb.x0],
                   fb.Width() * 4);
        }
    }

    ScreenRect outside[2];
    int count = screen_rect_outside(r, fFbWidth, fFbHeight, outside);
    for (int i = 0; i < count; i++) {
        const ScreenRect &o = outside[i];
        for (int y = o.y0; y < o.y1; y++)
            std::fill_n(&base[y * pitch + o.x0], o.Width(), kOpaque);
    }

    ScreenRect c = r & CursorRect();
    for (int sy = c.y0; sy < c.y1; sy++) {
        for (int sx = c.x0; sx < c.x1; sx++) {
            uint32_t under = kOpaque;
            if (sx < fFbWidth && sy < fFbHeight)
                under = fFb[sy * fFbWidth + sx];
            uint32_t src = fCursor[(sy - fCursorY) * fCursorWidth +
                                   (sx - fCursorX)];
            base[sy * pitch + sx] = cursor_blend(under, src) | kOpaque;
        }
    }
}


/* Commits what changed, once the compositor has shown the last frame and a
   buffer is free. */
void WaylandDisplay::Redraw()
{
    if (!fConfigured || fFrameCallback != nullptr || fWindowDirty.Empty())
        return;
    WindowBuffer *buffer = fBuffers->Acquire(fWindowWidth, fWindowHeight);
    if (buffer == nullptr && fBuffers->CreateFailed() &&
        fDmabufAllocator != nullptr) {
        /* the old buffers go, and the next ones are drawn in full */
        fBuffers.reset();
        fDmabufAllocator.reset();
        CreateBuffers();
        buffer = fBuffers->Acquire(fWindowWidth, fWindowHeight);
    }
    if (buffer == nullptr)
        return;

    fBuffers->Invalidate(fWindowDirty);
    fWindowDirty = ScreenRect();
    ScreenRect area = buffer->stale;
    {
        std::lock_guard<std::mutex> locker(fMutex);
        Paint(buffer, area);
    }

    SetRegions();
    wl_surface_attach(fSurface, buffer->buffer, 0, 0);
    damage_buffer(fSurface, fCompositorVersion, area);
    fFrameCallback = wl_surface_frame(fSurface);
    wl_callback_add_listener(fFrameCallback, &kFrameListener, this);
    wl_surface_commit(fSurface);
    fBuffers->Attached(buffer);
}


void WaylandDisplay::HandleFrameDone()
{
    wl_callback_destroy(fFrameCallback);
    fFrameCallback = nullptr;
    Redraw();
}


/* Takes in what the devices changed since the last time; false once the
   window is to close. */
bool WaylandDisplay::HandleUpdate()
{
    char buf[64];
    while (read(fWakeRead, buf, sizeof(buf)) > 0) {}

    bool resized = false;
    bool cursor_image = false;
    ScreenRect dirty;
    {
        std::lock_guard<std::mutex> locker(fMutex);

        fWakePending = false;
        if (fQuitRequested)
            return false;
        if (fFbGeneration != fShownGeneration) {
            fShownGeneration = fFbGeneration;
            /* a window the user sizes keeps its size; the guest follows it */
            if (!fWindowResizable && fFbWidth > 0 &&
                (fFbWidth != fWidth || fFbHeight != fHeight)) {
                fWidth = fFbWidth;
                fHeight = fFbHeight;
                resized = true;
            }
            dirty |= ScreenRect(0, 0, fWidth, fHeight);
            fCursorChanged = true;
        }
        dirty |= fDirty;
        fDirty = ScreenRect();

        if (fCursorImageChanged) {
            fCursorImageChanged = false;
            cursor_image = true;
            if (fCursorNative) {
                fHostCursor = fCursor;
                fHostCursorWidth = fCursorWidth;
                fHostCursorHeight = fCursorHeight;
                fHostCursorHotX = fCursorHotX;
                fHostCursorHotY = fCursorHotY;
            } else {
                fHostCursor.clear();
            }
        }
        if (fCursorChanged) {
            fCursorChanged = false;
            ScreenRect c = CursorRect();
            dirty |= fCursorDrawn;
            dirty |= c;
            fCursorDrawn = c;
        }
    }

    if (resized) {
        Layout();
        SetSizeLimits();
        fWindowDirty = ScreenRect(0, 0, fWindowWidth, fWindowHeight);
    }
    dirty = dirty & ScreenRect(0, 0, fWidth, fHeight);
    fWindowDirty |= ScreenRect(dirty.x0 + fContentX, dirty.y0 + fContentY,
                               dirty.Width(), dirty.Height());
    if (cursor_image)
        SetHostCursor();
    Redraw();
    return true;
}


//#pragma mark - the window

void WaylandDisplay::CreateWindow(int width, int height, bool resizable)
{
    fWidth = width;
    fHeight = height;
    fWindowResizable = resizable;
    fDecorator.SetTitle("TinyEMU");
    /* what the compositor will most likely answer */
    fServerSide = fDecorationManager != nullptr && !fOptions.client_decorations;
    Layout();

    CreateBuffers();
    fCursorBuffers = std::make_unique<BufferRing>(
        *fShmAllocator, 2, [this]() {
            if (fHostCursorPending)
                SetHostCursor();
        });

    fSurface = wl_compositor_create_surface(fCompositor);
    fCursorSurface = wl_compositor_create_surface(fCompositor);
    CreateFrameCursor(fFrameCursor, frame_cursor());
    fXdgSurface = xdg_wm_base_get_xdg_surface(fWmBase, fSurface);
    xdg_surface_add_listener(fXdgSurface, &kXdgSurfaceListener, this);
    fToplevel = xdg_surface_get_toplevel(fXdgSurface);
    xdg_toplevel_add_listener(fToplevel, &kToplevelListener, this);
    xdg_toplevel_set_title(fToplevel, "TinyEMU");
    xdg_toplevel_set_app_id(fToplevel, "tinyemu");
    SetSizeLimits();
    SetHostCursor();
    if (fDecorationManager != nullptr) {
        fDecoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            fDecorationManager, fToplevel);
        zxdg_toplevel_decoration_v1_add_listener(fDecoration,
                                                 &kDecorationListener, this);
        zxdg_toplevel_decoration_v1_set_mode(fDecoration,
            fOptions.client_decorations ?
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE :
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    /* no buffer until the first configure */
    wl_surface_commit(fSurface);
}


/* The window's buffers: dma-bufs when the compositor takes them, shared
   memory otherwise. */
void WaylandDisplay::CreateBuffers()
{
    BufferAllocator *allocator = fDmabufAllocator != nullptr ?
                                 fDmabufAllocator.get() : fShmAllocator.get();
    fBuffers = std::make_unique<BufferRing>(*allocator, 3,
                                            [this]() {Redraw();});
}


void WaylandDisplay::CreateFrameCursor(FrameCursorSurface &cursor,
                                       const FrameCursor &image)
{
    cursor.surface = wl_compositor_create_surface(fCompositor);
    cursor.buffers = std::make_unique<BufferRing>(*fShmAllocator, 1, nullptr);
    cursor.hot_x = image.hot_x;
    cursor.hot_y = image.hot_y;
    cursor.buffer = cursor.buffers->Acquire(image.width, image.height);
    if (cursor.buffer != nullptr) {
        std::copy(image.pixels.begin(), image.pixels.end(),
                  cursor.buffer->pixels);
    }
}


/* Places the content in the window, with the frame around it when it is
   drawn here. */
void WaylandDisplay::Layout()
{
    fFrame = !fServerSide && !fFullscreen;
    if (fFrame && !fTitleFontLoaded) {
        /* the system's bold font, as Haiku titles windows */
        fTitleFontLoaded = true;
        if (fTitleFont.Open("sans-serif:bold", 12))
            fDecorator.SetFont(&fTitleFont);
        else
            fprintf(stderr, "No font for the window's title\n");
    }
    fContentX = fFrame ? BeDecorator::kLeft : 0;
    fContentY = fFrame ? fDecorator.Top() : 0;
    fWindowWidth = fWidth + (fFrame ? BeDecorator::kLeft +
                                      BeDecorator::kRight : 0);
    fWindowHeight = fHeight + (fFrame ? fDecorator.Top() +
                                        BeDecorator::kBottom : 0);
    fDecorator.SetSize(fWindowWidth, fWindowHeight);
    fDecorator.SetResizable(fWindowResizable && !fMaximized);
}


/* A fixed size window keeps the size of the frame buffer. */
void WaylandDisplay::SetSizeLimits()
{
    int frame_width = fWindowWidth - fWidth;
    int frame_height = fWindowHeight - fHeight;

    if (fWindowResizable) {
        xdg_toplevel_set_min_size(fToplevel, kMinContentSize + frame_width,
                                  kMinContentSize + frame_height);
        xdg_toplevel_set_max_size(fToplevel, 0, 0);
    } else {
        xdg_toplevel_set_min_size(fToplevel, fWindowWidth, fWindowHeight);
        xdg_toplevel_set_max_size(fToplevel, fWindowWidth, fWindowHeight);
    }
}


/* The window's shape, which the frame's see-through part is not part of,
   set with the next commit. */
void WaylandDisplay::SetRegions()
{
    if (fRegionWidth == fWindowWidth && fRegionHeight == fWindowHeight &&
        fRegionFrame == fFrame)
        return;
    fRegionWidth = fWindowWidth;
    fRegionHeight = fWindowHeight;
    fRegionFrame = fFrame;

    wl_region *region = wl_compositor_create_region(fCompositor);
    if (fFrame) {
        ScreenRect rects[2];
        int count = fDecorator.SolidRects(rects);
        for (int i = 0; i < count; i++) {
            wl_region_add(region, rects[i].x0, rects[i].y0, rects[i].Width(),
                          rects[i].Height());
        }
    } else {
        wl_region_add(region, 0, 0, fWindowWidth, fWindowHeight);
    }
    wl_surface_set_opaque_region(fSurface, region);
    wl_surface_set_input_region(fSurface, region);
    wl_region_destroy(region);
    xdg_surface_set_window_geometry(fXdgSurface, 0, 0, fWindowWidth,
                                    fWindowHeight);
}


void WaylandDisplay::HandleToplevelConfigure(int width, int height,
                                             wl_array *states)
{
    fConfigureWidth = width;
    fConfigureHeight = height;
    fConfigureActivated = false;
    fConfigureFullscreen = false;
    fConfigureMaximized = false;

    const uint32_t *state = static_cast<const uint32_t *>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); i++) {
        switch (state[i]) {
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            fConfigureActivated = true;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            fConfigureFullscreen = true;
            break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            fConfigureMaximized = true;
            break;
        }
    }
}


void WaylandDisplay::HandleConfigure(uint32_t serial)
{
    xdg_surface_ack_configure(fXdgSurface, serial);
    fConfigured = true;

    fFullscreen = fConfigureFullscreen;
    fMaximized = fConfigureMaximized;
    fDecorator.SetActive(fConfigureActivated);
    Layout();

    bool resized = false;
    if (fWindowResizable && fConfigureWidth > 0 && fConfigureHeight > 0) {
        /* not every compositor keeps to the minimum size */
        int width = std::max(fConfigureWidth - (fWindowWidth - fWidth),
                             kMinContentSize);
        int height = std::max(fConfigureHeight - (fWindowHeight - fHeight),
                              kMinContentSize);
        if (width != fWidth || height != fHeight) {
            fWidth = width;
            fHeight = height;
            resized = true;
            Layout();
        }
    }
    SetSizeLimits();
    UpdatePointerPart();
    /* the acknowledgement takes effect with the next commit */
    fWindowDirty = ScreenRect(0, 0, fWindowWidth, fWindowHeight);
    if (resized)
        TellSourceSize();
    Redraw();
}


void WaylandDisplay::TellSourceSize()
{
    DeviceLocker device_locker(fDeviceLock);
    ScreenSource *source;
    {
        std::lock_guard<std::mutex> locker(fMutex);
        source = fSource;
    }
    if (source != nullptr)
        source->ScreenResized(fWidth, fHeight);
}


/* The guest's cursor image: shown natively, or none. None is the cursor
   surface without a buffer rather than no cursor surface, which WSLg
   answers by keeping the last image. */
void WaylandDisplay::SetHostCursor()
{
    fHostCursorPending = false;
    /* the old buffer may go when the size changes */
    fCursorBuffer = nullptr;
    if (!fHostCursor.empty()) {
        WindowBuffer *buffer = fCursorBuffers->Acquire(fHostCursorWidth,
                                                       fHostCursorHeight);
        if (buffer == nullptr) {
            fHostCursorPending = true;
            return;
        }
        for (size_t i = 0; i < fHostCursor.size(); i++)
            buffer->pixels[i] = premultiply(fHostCursor[i]);
        fCursorBuffer = buffer;
    }
    if (fCursorKind == kCursorGuest)
        ShowHostCursor();
}


/* The cursor is set again each time the pointer enters the window. */
void WaylandDisplay::ShowHostCursor()
{
    if (fPointerDevice == nullptr || !fPointerInside)
        return;

    wl_surface *surface;
    BufferRing *ring;
    WindowBuffer *buffer;
    int hot_x, hot_y;
    if (fCursorKind == kCursorGuest) {
        surface = fCursorSurface;
        ring = fCursorBuffers.get();
        buffer = fCursorBuffer;
        hot_x = fHostCursorHotX;
        hot_y = fHostCursorHotY;
    } else {
        surface = fFrameCursor.surface;
        ring = fFrameCursor.buffers.get();
        buffer = fFrameCursor.buffer;
        hot_x = fFrameCursor.hot_x;
        hot_y = fFrameCursor.hot_y;
    }
    if (buffer == nullptr) {
        /* A cursor surface without a buffer hides the cursor; WSLg hides it
           only once the surface that showed it is destroyed. */
        if (fShownCursorSurface != nullptr) {
            ReplaceCursorSurface(fShownCursorSurface);
            fShownCursorSurface = nullptr;
        }
        wl_pointer_set_cursor(fPointerDevice, fEnterSerial, fCursorSurface,
                              0, 0);
        return;
    }
    wl_pointer_set_cursor(fPointerDevice, fEnterSerial, surface, hot_x,
                          hot_y);
    /* WSLg takes the image from a commit made after the surface became the
       cursor, not from one made before */
    wl_surface_attach(surface, buffer->buffer, 0, 0);
    damage_buffer(surface, fCompositorVersion,
                  ScreenRect(0, 0, buffer->width, buffer->height));
    wl_surface_commit(surface);
    ring->Attached(buffer);
    fShownCursorSurface = surface;
}


/* A new surface in place of 'old', one of the cursor surfaces. */
void WaylandDisplay::ReplaceCursorSurface(wl_surface *old)
{
    for (wl_surface **surface : {&fCursorSurface, &fFrameCursor.surface}) {
        if (*surface == old) {
            wl_surface_destroy(old);
            *surface = wl_compositor_create_surface(fCompositor);
            return;
        }
    }
}


void WaylandDisplay::SetCursorKind(CursorKind kind)
{
    if (kind == fCursorKind)
        return;
    fCursorKind = kind;
    ShowHostCursor();
}


//#pragma mark - keyboard

/* With the device lock held. */
void WaylandDisplay::SendKey(int code, bool down)
{
    if (code <= 0 || code > KEYCODE_MAX)
        return;
    fKeyPressed[code] = down;
    if (fKeyboard != nullptr)
        fKeyboard->SendKeyEvent(down, code);
}


/* The keys held now are released where the guest cannot see it. */
void WaylandDisplay::ReleaseKeys()
{
    DeviceLocker locker(fDeviceLock);
    for (int code = 1; code <= KEYCODE_MAX; code++) {
        if (fKeyPressed[code])
            SendKey(code, false);
    }
}


/* 'key' is already an evdev key code. */
void WaylandDisplay::HandleKey(uint32_t key, uint32_t state)
{
    bool down = state == WL_KEYBOARD_KEY_STATE_PRESSED;

    if (key == 0 || key > KEYCODE_MAX)
        return;
    DeviceLocker locker(fDeviceLock);
    /* auto repeat; the guest repeats by itself */
    if (!(down && fKeyPressed[key]))
        SendKey(key, down);
}


//#pragma mark - pointer

/* The guest has the pointer while it is over the content, and while a
   button pressed there is held. */
bool WaylandDisplay::PointerOnGuest() const
{
    return fButtons != 0 || fPointerPart == BeDecorator::kContent;
}


/* Which part of the window the pointer is over, and the cursor for it. */
void WaylandDisplay::UpdatePointerPart()
{
    if (!fPointerInside)
        return;
    fPointerPart = fFrame ? fDecorator.HitTest(fPointerX, fPointerY) :
                            BeDecorator::kContent;
    SetCursorKind(PointerOnGuest() ? kCursorGuest : kCursorFrame);
}


void WaylandDisplay::HandlePointerEnter(uint32_t serial, wl_fixed_t x,
                                        wl_fixed_t y)
{
    fPointerInside = true;
    fEnterSerial = serial;
    fPointerX = wl_fixed_to_int(x);
    fPointerY = wl_fixed_to_int(y);
    fHaveLastPos = false;
    UpdatePointerPart();
    ShowHostCursor();
    if (PointerOnGuest())
        fPointerChanged = true;
}


/* No more pointer events until it comes back: what was held is let go. */
void WaylandDisplay::HandlePointerLeave()
{
    fPointerInside = false;
    fDragPending = false;
    if (fCloseTracking) {
        fCloseTracking = false;
        SetClosePressed(false);
    }
    if (fButtons != 0) {
        fButtons = 0;
        fPointerChanged = true;
    }
}


void WaylandDisplay::HandlePointerMotion(wl_fixed_t x, wl_fixed_t y)
{
    fPointerX = wl_fixed_to_int(x);
    fPointerY = wl_fixed_to_int(y);

    if (fCloseTracking) {
        SetClosePressed(fDecorator.HitTest(fPointerX, fPointerY) ==
                        BeDecorator::kClose);
        return;
    }
    if (fDragPending) {
        if (abs(fPointerX - fDragX) >= kDragDistance ||
            abs(fPointerY - fDragY) >= kDragDistance) {
            fDragPending = false;
            xdg_toplevel_move(fToplevel, fSeat, fDragSerial);
        }
        return;
    }
    if (fButtons == 0)
        UpdatePointerPart();
    if (PointerOnGuest())
        fPointerChanged = true;
}


void WaylandDisplay::HandlePointerButton(uint32_t serial, uint32_t time,
                                         uint32_t button, uint32_t state)
{
    unsigned int bit;
    switch (button) {
    case BTN_LEFT:
        bit = 1 << 0;
        break;
    case BTN_RIGHT:
        bit = 1 << 1;
        break;
    case BTN_MIDDLE:
        bit = 1 << 2;
        break;
    default:
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (!PointerOnGuest()) {
            if (button == BTN_LEFT)
                PressFrame(serial, time);
            return;
        }
        fButtons |= bit;
        fPointerChanged = true;
    } else if (fButtons & bit) {
        fButtons &= ~bit;
        fPointerChanged = true;
        if (fButtons == 0)
            UpdatePointerPart();
    } else if (button == BTN_LEFT) {
        ReleaseFrame();
    }
}


/* The left button went down on the frame. */
void WaylandDisplay::PressFrame(uint32_t serial, uint32_t time)
{
    switch (fPointerPart) {
    case BeDecorator::kClose:
        fCloseTracking = true;
        SetClosePressed(true);
        break;

    case BeDecorator::kResize:
        xdg_toplevel_resize(fToplevel, fSeat, serial,
                            XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
        break;

    case BeDecorator::kMove:
        if (fHaveLastClick && time - fLastClickTime < kDoubleClickMs &&
            abs(fPointerX - fLastClickX) <= kDoubleClickDistance &&
            abs(fPointerY - fLastClickY) <= kDoubleClickDistance) {
            fHaveLastClick = false;
            if (fCanMinimize)
                xdg_toplevel_set_minimized(fToplevel);
            break;
        }
        fHaveLastClick = true;
        fLastClickTime = time;
        fLastClickX = fPointerX;
        fLastClickY = fPointerY;
        /* moving only once dragged keeps a double click from being taken
           by the compositor's move */
        fDragPending = true;
        fDragSerial = serial;
        fDragX = fPointerX;
        fDragY = fPointerY;
        break;

    default:
        break;
    }
}


void WaylandDisplay::ReleaseFrame()
{
    fDragPending = false;
    if (!fCloseTracking)
        return;
    fCloseTracking = false;
    if (fDecorator.ClosePressed()) {
        SetClosePressed(false);
        /* the machine's shutdown closes the window */
        fRunControl.RequestShutdown(0);
    }
}


void WaylandDisplay::SetClosePressed(bool pressed)
{
    if (pressed == fDecorator.ClosePressed())
        return;
    fDecorator.SetClosePressed(pressed);
    fWindowDirty |= fDecorator.TabRect();
    Redraw();
}


/* (fPointerX, fPointerY) relative to the content, as the guest sees it. */
void WaylandDisplay::SendMouse(int dz)
{
    int x = fPointerX - fContentX;
    int y = fPointerY - fContentY;
    int dx = fHaveLastPos ? x - fLastX : 0;
    int dy = fHaveLastPos ? y - fLastY : 0;
    fHaveLastPos = true;
    fLastX = x;
    fLastY = y;

    DeviceLocker device_locker(fDeviceLock);
    if (fPointer == nullptr)
        return;
    if (fPointer->MouseIsAbsolute()) {
        /* scaled to the frame buffer, which may be smaller than the window
           until the guest follows a resize */
        int width, height;
        {
            std::lock_guard<std::mutex> locker(fMutex);
            width = fFbWidth;
            height = fFbHeight;
        }
        if (width <= 0)
            return;
        x = (std::clamp(x, 0, width - 1) * 32768) / width;
        y = (std::clamp(y, 0, height - 1) * 32768) / height;
        fPointer->SendMouseEvent(x, y, dz, fButtons);
    } else {
        fPointer->SendMouseEvent(dx, dy, dz, fButtons);
    }
}


/* Sends what the pointer did since its last frame: one event per wheel
   notch, a wheel without notches counted in steps of kWheelStepAxis. */
void WaylandDisplay::HandlePointerFrame()
{
    int steps = 0;

    if (fWheelHasSteps) {
        fWheelAxis = 0;
        steps = fWheelDiscrete + fWheel120 / kWheelStep120;
        fWheel120 %= kWheelStep120;
    } else {
        steps = (int)(fWheelAxis / kWheelStepAxis);
        fWheelAxis -= steps * kWheelStepAxis;
    }
    fWheelDiscrete = 0;
    fWheelHasSteps = false;

    if (!fPointerChanged && steps == 0)
        return;
    fPointerChanged = false;
    if (steps == 0)
        SendMouse(0);
    /* the axis grows downwards, the wheel steps upwards */
    for (; steps > 0; steps--)
        SendMouse(-1);
    for (; steps < 0; steps++)
        SendMouse(1);
}


//#pragma mark - listeners

const wl_registry_listener WaylandDisplay::kRegistryListener = {
    .global = [](void *data, wl_registry *registry, uint32_t name,
                 const char *interface, uint32_t version) {
        static_cast<WaylandDisplay *>(data)->HandleGlobal(registry, name,
                                                          interface, version);
    },
    .global_remove = [](void *, wl_registry *, uint32_t) {},
};


const xdg_wm_base_listener WaylandDisplay::kWmBaseListener = {
    .ping = [](void *, xdg_wm_base *wm_base, uint32_t serial) {
        xdg_wm_base_pong(wm_base, serial);
    },
};


const xdg_surface_listener WaylandDisplay::kXdgSurfaceListener = {
    .configure = [](void *data, xdg_surface *, uint32_t serial) {
        static_cast<WaylandDisplay *>(data)->HandleConfigure(serial);
    },
};


const xdg_toplevel_listener WaylandDisplay::kToplevelListener = {
    .configure = [](void *data, xdg_toplevel *, int32_t width, int32_t height,
                    wl_array *states) {
        static_cast<WaylandDisplay *>(data)->HandleToplevelConfigure(
            width, height, states);
    },
    .close = [](void *data, xdg_toplevel *) {
        /* the machine's shutdown closes the window */
        static_cast<WaylandDisplay *>(data)->fRunControl.RequestShutdown(0);
    },
    .configure_bounds = [](void *, xdg_toplevel *, int32_t, int32_t) {},
    .wm_capabilities = [](void *data, xdg_toplevel *,
                          wl_array *capabilities) {
        auto *display = static_cast<WaylandDisplay *>(data);
        const uint32_t *cap = static_cast<const uint32_t *>(
            capabilities->data);
        display->fCanMinimize = false;
        for (size_t i = 0; i < capabilities->size / sizeof(uint32_t); i++) {
            if (cap[i] == XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE)
                display->fCanMinimize = true;
        }
    },
};


/* A configure of the window follows, which applies the mode. */
const zxdg_toplevel_decoration_v1_listener
WaylandDisplay::kDecorationListener = {
    .configure = [](void *data, zxdg_toplevel_decoration_v1 *, uint32_t mode) {
        static_cast<WaylandDisplay *>(data)->fServerSide =
            mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    },
};


const wl_callback_listener WaylandDisplay::kFrameListener = {
    .done = [](void *data, wl_callback *, uint32_t) {
        static_cast<WaylandDisplay *>(data)->HandleFrameDone();
    },
};


const wl_seat_listener WaylandDisplay::kSeatListener = {
    .capabilities = [](void *data, wl_seat *, uint32_t caps) {
        static_cast<WaylandDisplay *>(data)->HandleSeatCapabilities(caps);
    },
    .name = [](void *, wl_seat *, const char *) {},
};


const wl_keyboard_listener WaylandDisplay::kKeyboardListener = {
    .keymap = [](void *, wl_keyboard *, uint32_t, int32_t fd, uint32_t) {
        /* the keys are sent as key codes, so the layout does not matter */
        close(fd);
    },
    .enter = [](void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {},
    .leave = [](void *data, wl_keyboard *, uint32_t, wl_surface *) {
        static_cast<WaylandDisplay *>(data)->ReleaseKeys();
    },
    .key = [](void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key,
              uint32_t state) {
        static_cast<WaylandDisplay *>(data)->HandleKey(key, state);
    },
    .modifiers = [](void *, wl_keyboard *, uint32_t, uint32_t, uint32_t,
                    uint32_t, uint32_t) {},
    .repeat_info = [](void *, wl_keyboard *, int32_t, int32_t) {},
};


/* Before version 5 nothing groups the events, so each is a frame of its
   own. */
const wl_pointer_listener WaylandDisplay::kPointerListener = {
    .enter = [](void *data, wl_pointer *, uint32_t serial, wl_surface *,
                wl_fixed_t x, wl_fixed_t y) {
        auto *display = static_cast<WaylandDisplay *>(data);
        display->HandlePointerEnter(serial, x, y);
        if (display->fSeatVersion < 5)
            display->HandlePointerFrame();
    },
    .leave = [](void *data, wl_pointer *, uint32_t, wl_surface *) {
        auto *display = static_cast<WaylandDisplay *>(data);
        display->HandlePointerLeave();
        if (display->fSeatVersion < 5)
            display->HandlePointerFrame();
    },
    .motion = [](void *data, wl_pointer *, uint32_t, wl_fixed_t x,
                 wl_fixed_t y) {
        auto *display = static_cast<WaylandDisplay *>(data);
        display->HandlePointerMotion(x, y);
        if (display->fSeatVersion < 5)
            display->HandlePointerFrame();
    },
    .button = [](void *data, wl_pointer *, uint32_t serial, uint32_t time,
                 uint32_t button, uint32_t state) {
        auto *display = static_cast<WaylandDisplay *>(data);
        display->HandlePointerButton(serial, time, button, state);
        if (display->fSeatVersion < 5)
            display->HandlePointerFrame();
    },
    .axis = [](void *data, wl_pointer *, uint32_t, uint32_t axis,
               wl_fixed_t value) {
        auto *display = static_cast<WaylandDisplay *>(data);
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL ||
            !display->PointerOnGuest())
            return;
        display->fWheelAxis += wl_fixed_to_double(value);
        if (display->fSeatVersion < 5)
            display->HandlePointerFrame();
    },
    .frame = [](void *data, wl_pointer *) {
        static_cast<WaylandDisplay *>(data)->HandlePointerFrame();
    },
    .axis_source = [](void *, wl_pointer *, uint32_t) {},
    .axis_stop = [](void *, wl_pointer *, uint32_t, uint32_t) {},
    .axis_discrete = [](void *data, wl_pointer *, uint32_t axis,
                        int32_t discrete) {
        auto *display = static_cast<WaylandDisplay *>(data);
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL ||
            !display->PointerOnGuest())
            return;
        display->fWheelDiscrete += discrete;
        display->fWheelHasSteps = true;
    },
    .axis_value120 = [](void *data, wl_pointer *, uint32_t axis,
                        int32_t value120) {
        auto *display = static_cast<WaylandDisplay *>(data);
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL ||
            !display->PointerOnGuest())
            return;
        display->fWheel120 += value120;
        display->fWheelHasSteps = true;
    },
};


void WaylandDisplay::HandleGlobal(wl_registry *registry, uint32_t name,
                                  const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        fCompositorVersion = std::min(version, 4u);
        fCompositor = static_cast<wl_compositor *>(wl_registry_bind(
            registry, name, &wl_compositor_interface, fCompositorVersion));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        fShm = static_cast<wl_shm *>(wl_registry_bind(
            registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        fDmabufName = name;
        fDmabufVersion = version;
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        fWmBase = static_cast<xdg_wm_base *>(wl_registry_bind(
            registry, name, &xdg_wm_base_interface, std::min(version, 5u)));
        xdg_wm_base_add_listener(fWmBase, &kWmBaseListener, this);
    } else if (strcmp(interface,
                      zxdg_decoration_manager_v1_interface.name) == 0) {
        fDecorationManager = static_cast<zxdg_decoration_manager_v1 *>(
            wl_registry_bind(registry, name,
                             &zxdg_decoration_manager_v1_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
               fSeat == nullptr) {
        fSeatVersion = std::min(version, 8u);
        fSeat = static_cast<wl_seat *>(wl_registry_bind(
            registry, name, &wl_seat_interface, fSeatVersion));
        wl_seat_add_listener(fSeat, &kSeatListener, this);
    }
}


void WaylandDisplay::HandleSeatCapabilities(uint32_t caps)
{
    bool keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    bool pointer = caps & WL_SEAT_CAPABILITY_POINTER;

    if (keyboard && fKeyboardDevice == nullptr) {
        fKeyboardDevice = wl_seat_get_keyboard(fSeat);
        wl_keyboard_add_listener(fKeyboardDevice, &kKeyboardListener, this);
    } else if (!keyboard && fKeyboardDevice != nullptr) {
        ReleaseKeys();
        wl_keyboard_destroy(fKeyboardDevice);
        fKeyboardDevice = nullptr;
    }
    if (pointer && fPointerDevice == nullptr) {
        fPointerDevice = wl_seat_get_pointer(fSeat);
        wl_pointer_add_listener(fPointerDevice, &kPointerListener, this);
    } else if (!pointer && fPointerDevice != nullptr) {
        HandlePointerLeave();
        HandlePointerFrame();
        wl_pointer_destroy(fPointerDevice);
        fPointerDevice = nullptr;
    }
}


//#pragma mark - running

bool WaylandDisplay::Connect()
{
    fDisplay = wl_display_connect(nullptr);
    if (fDisplay == nullptr) {
        fprintf(stderr, "No Wayland compositor to show the display on\n");
        return false;
    }
    int fds[2];
    if (pipe(fds) < 0) {
        perror("pipe");
        return false;
    }
    fWakeRead = fds[0];
    fWakeWrite = fds[1];
    for (int fd : fds) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    fRegistry = wl_display_get_registry(fDisplay);
    wl_registry_add_listener(fRegistry, &kRegistryListener, this);
    if (wl_display_roundtrip(fDisplay) < 0) {
        fprintf(stderr, "Could not talk to the Wayland compositor\n");
        return false;
    }
    if (fCompositor == nullptr || fShm == nullptr || fWmBase == nullptr) {
        fprintf(stderr, "The Wayland compositor has no %s\n",
                fCompositor == nullptr ? "wl_compositor" :
                fShm == nullptr ? "wl_shm" : "xdg_wm_base");
        return false;
    }
    fShmAllocator = std::make_unique<ShmAllocator>(fShm,
                                                   WL_SHM_FORMAT_ARGB8888);
#ifdef CONFIG_WAYLAND_DMABUF
    if (fDmabufName != 0) {
        fDmabufAllocator = dmabuf_allocator_create(fDisplay, fRegistry,
                                                   fDmabufName,
                                                   fDmabufVersion);
    }
#endif
    return true;
}


/* The connection is lost: the machine is shut down. */
bool WaylandDisplay::Fail()
{
    int err = wl_display_get_error(fDisplay);
    fprintf(stderr, "Lost the connection to the Wayland compositor: %s\n",
            strerror(err != 0 ? err : errno));
    fRunControl.RequestShutdown(1);
    return false;
}


/* Runs until the window is to close, true then, or the connection is
   lost. */
bool WaylandDisplay::EventLoop()
{
    pollfd fds[2] = {
        {wl_display_get_fd(fDisplay), POLLIN, 0},
        {fWakeRead, POLLIN, 0},
    };

    for (;;) {
        while (wl_display_prepare_read(fDisplay) != 0) {
            if (wl_display_dispatch_pending(fDisplay) < 0)
                return Fail();
        }
        fds[0].events = POLLIN;
        if (wl_display_flush(fDisplay) < 0) {
            if (errno != EAGAIN) {
                wl_display_cancel_read(fDisplay);
                return Fail();
            }
            /* the rest goes once the socket has room */
            fds[0].events |= POLLOUT;
        }

        if (poll(fds, 2, -1) < 0) {
            wl_display_cancel_read(fDisplay);
            if (errno == EINTR)
                continue;
            return Fail();
        }
        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(fDisplay) < 0)
                return Fail();
        } else {
            wl_display_cancel_read(fDisplay);
        }
        if (wl_display_dispatch_pending(fDisplay) < 0)
            return Fail();
        if ((fds[1].revents & POLLIN) && !HandleUpdate())
            return true;
    }
}


void WaylandDisplay::Run()
{
    int width, height;
    bool resizable;

    {
        std::unique_lock<std::mutex> locker(fMutex);
        if (fSource == nullptr || fSourceWidth <= 0 || fSourceHeight <= 0) {
            /* nothing to show, so no window */
            fCond.wait(locker, [this]() {return fQuitRequested;});
            return;
        }
        if (fQuitRequested)
            return;
        width = fSourceWidth;
        height = fSourceHeight;
        resizable = fResizable;
    }

    CreateWindow(width, height, resizable);
    if (!EventLoop()) {
        std::unique_lock<std::mutex> locker(fMutex);
        fCond.wait(locker, [this]() {return fQuitRequested;});
    }
}


std::unique_ptr<HostDisplay> host_display_create(DeviceLock &lock,
                                                 RunControl &run_control,
                                                 const DisplayOptions &options)
{
    auto display = std::make_unique<WaylandDisplay>(lock, run_control,
                                                    options);
    if (!display->Connect()) {
        fprintf(stderr, "Running without a display\n");
        return nullptr;
    }
    return display;
}
