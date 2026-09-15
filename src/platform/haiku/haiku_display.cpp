/*
 * Haiku display
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
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <Application.h>
#include <Bitmap.h>
#include <Cursor.h>
#include <Messenger.h>
#include <View.h>
#include <Window.h>

#include "device_lock.h"
#include "haiku_keymap.h"
#include "platform_backends.h"
#include "run_control.h"
#include "screen_rect.h"

#define APP_SIGNATURE "application/x-vnd.TinyEMU"

/* posted to the window when there is something to draw */
static const uint32 kMsgUpdate = 'upd ';


static BRect to_brect(const ScreenRect &r)
{
    return BRect(r.x0, r.y0, r.x1 - 1, r.y1 - 1);
}


static ScreenRect from_brect(BRect r)
{
    ScreenRect res;
    res.x0 = (int)r.left;
    res.y0 = (int)r.top;
    res.x1 = (int)r.right + 1;
    res.y1 = (int)r.bottom + 1;
    return res;
}


static ScreenRect view_rect(BView *view)
{
    BRect bounds = view->Bounds();
    return ScreenRect(0, 0, bounds.IntegerWidth() + 1,
                      bounds.IntegerHeight() + 1);
}


class HaikuDisplay;


class ScreenView final: public BView {
private:
    HaikuDisplay &fDisplay;

public:
    ScreenView(BRect frame, HaikuDisplay &display);

    void AttachedToWindow() override;
    void Draw(BRect update) override;
    void FrameResized(float width, float height) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit,
                    const BMessage *drag) override;
};


/* Runs on a thread of its own, like every Haiku window. */
class ScreenWindow final: public BWindow {
private:
    HaikuDisplay &fDisplay;
    ScreenView *fView;

public:
    ScreenWindow(HaikuDisplay &display, int width, int height,
                 bool resizable);

    void DispatchMessage(BMessage *message, BHandler *handler) override;
    void MessageReceived(BMessage *message) override;
    void WindowActivated(bool active) override;
    bool QuitRequested() override;
};


class HaikuDisplay final: public HostDisplay {
private:
    friend class ScreenView;
    friend class ScreenWindow;

    DeviceLock &fDeviceLock;
    RunControl &fRunControl;
    /* declared first, so that the bitmaps go before it */
    std::unique_ptr<BApplication> fApp;
    std::atomic<bool> fQuitting {false};

    /* Everything below the mutex is shared with the other threads. */
    std::mutex fMutex;
    ScreenSource *fSource = nullptr;
    int fSourceWidth = 0;
    int fSourceHeight = 0;
    bool fResizable = false;
    /* the source's pixels, read in SetFramebuffer() and Update() */
    const uint8_t *fData = nullptr;
    int fDataStride = 0;
    /* the copy the window is drawn from */
    std::unique_ptr<BBitmap> fFb;
    int fFbWidth = 0;
    int fFbHeight = 0;
    /* changes whenever fFb is reallocated */
    uint32 fFbGeneration = 0;
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
    BMessenger fWindow;
    /* an update message is queued and not yet handled */
    bool fUpdatePosted = false;

    /* with the device lock held */
    KeyboardTarget *fKeyboard = nullptr;
    PointerTarget *fPointer = nullptr;

    /* the window's thread */
    uint32 fShownGeneration = 0;
    ScreenRect fCursorDrawn;
    std::unique_ptr<BBitmap> fComposite;
    std::unique_ptr<BCursor> fViewCursor;
    uint8 fKeyStates[16] {};
    BPoint fLastWhere;
    int32 fButtons = 0;

    void PostUpdate();
    void CopyFromSource(int x, int y, int w, int h);
    ScreenRect CursorRect() const;
    void Paint(BView *view, const ScreenRect &area,
               const ScreenRect &cursor);
    void UpdateHostCursor(BView *view);

    /* the window's thread */
    void DrawUpdate(BView *view);
    void DrawView(BView *view, BRect update);
    void ViewResized(int width, int height);
    void SetKeyStates(const uint8 *states);
    void HandleKeyMessage(BMessage *message);
    void HandleMouse(BMessage *message, BPoint where, int dz);
    void HandleWheel(BMessage *message);
    void ReleaseKeys();
    bool QuitWindow();

public:
    HaikuDisplay(DeviceLock &lock, RunControl &run_control,
                 std::unique_ptr<BApplication> app):
        fDeviceLock(lock), fRunControl(run_control), fApp(std::move(app)) {}
    ~HaikuDisplay() override;

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


//#pragma mark - ScreenView

ScreenView::ScreenView(BRect frame, HaikuDisplay &display):
    BView(frame, "screen", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS),
    fDisplay(display)
{
    /* everything is drawn here, so nothing is erased first */
    SetViewColor(B_TRANSPARENT_COLOR);
}


void ScreenView::AttachedToWindow()
{
    BCursor cursor(B_CURSOR_ID_NO_CURSOR);
    SetViewCursor(&cursor);
    SetDrawingMode(B_OP_COPY);
    MakeFocus(true);
}


void ScreenView::Draw(BRect update)
{
    fDisplay.DrawView(this, update);
}


void ScreenView::FrameResized(float width, float height)
{
    fDisplay.ViewResized((int)width + 1, (int)height + 1);
}


void ScreenView::MouseDown(BPoint where)
{
    /* keep the motion while a button is held outside the view */
    SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
    fDisplay.HandleMouse(Window()->CurrentMessage(), where, 0);
}


void ScreenView::MouseUp(BPoint where)
{
    fDisplay.HandleMouse(Window()->CurrentMessage(), where, 0);
}


void ScreenView::MouseMoved(BPoint where, uint32 transit, const BMessage *drag)
{
    (void)drag;
    if (transit == B_ENTERED_VIEW)
        fDisplay.fLastWhere = where;
    fDisplay.HandleMouse(Window()->CurrentMessage(), where, 0);
}


//#pragma mark - ScreenWindow

ScreenWindow::ScreenWindow(HaikuDisplay &display, int width, int height,
                           bool resizable):
    BWindow(BRect(0, 0, width - 1, height - 1), "TinyEMU", B_TITLED_WINDOW,
            resizable ? 0 : B_NOT_RESIZABLE | B_NOT_ZOOMABLE),
    fDisplay(display)
{
    fView = new ScreenView(Bounds(), display);
    AddChild(fView);
    CenterOnScreen();
}


void ScreenWindow::DispatchMessage(BMessage *message, BHandler *handler)
{
    /* keys go to the guest before any shortcut or navigation sees them */
    switch (message->what) {
    case B_KEY_DOWN:
    case B_KEY_UP:
    case B_UNMAPPED_KEY_DOWN:
    case B_UNMAPPED_KEY_UP:
    case B_MODIFIERS_CHANGED:
        fDisplay.HandleKeyMessage(message);
        return;
    case B_MOUSE_WHEEL_CHANGED:
        fDisplay.HandleWheel(message);
        return;
    }
    BWindow::DispatchMessage(message, handler);
}


void ScreenWindow::MessageReceived(BMessage *message)
{
    if (message->what == kMsgUpdate) {
        fDisplay.DrawUpdate(fView);
        return;
    }
    BWindow::MessageReceived(message);
}


void ScreenWindow::WindowActivated(bool active)
{
    /* the keys held now are released where the guest cannot see it */
    if (!active)
        fDisplay.ReleaseKeys();
    BWindow::WindowActivated(active);
}


bool ScreenWindow::QuitRequested()
{
    return fDisplay.QuitWindow();
}


//#pragma mark - HaikuDisplay

HaikuDisplay::~HaikuDisplay()
{
    fFb.reset();
    fComposite.reset();
    fViewCursor.reset();
}


/* With fMutex held. Fails when the window's queue is full. */
void HaikuDisplay::PostUpdate()
{
    if (fUpdatePosted || !fWindow.IsValid())
        return;
    BMessage message(kMsgUpdate);
    fUpdatePosted = fWindow.SendMessage(&message, (BHandler *)nullptr, 0) ==
        B_OK;
}


void HaikuDisplay::SetTarget(KeyboardTarget *target)
{
    fKeyboard = target;
}


void HaikuDisplay::SetTarget(PointerTarget *target)
{
    fPointer = target;
}


void HaikuDisplay::SetSource(ScreenSource *source, int width, int height)
{
    std::lock_guard<std::mutex> locker(fMutex);
    fSource = source;
    fSourceWidth = width;
    fSourceHeight = height;
    fResizable = source != nullptr && source->Resizable();
}


/* With fMutex held: copies a rectangle, already clipped, into fFb. */
void HaikuDisplay::CopyFromSource(int x, int y, int w, int h)
{
    uint8_t *bits = static_cast<uint8_t *>(fFb->Bits());
    int32 stride = fFb->BytesPerRow();

    for (int i = 0; i < h; i++) {
        memcpy(bits + (y + i) * stride + x * 4,
               fData + (y + i) * fDataStride + x * 4, w * 4);
    }
}


void HaikuDisplay::SetFramebuffer(uint8_t *data, int width, int height,
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
        fFb.reset();
        if (width > 0) {
            fFb = std::make_unique<BBitmap>(BRect(0, 0, width - 1, height - 1),
                                            B_RGB32);
            if (fFb->InitCheck() != B_OK) {
                fprintf(stderr, "Could not allocate a %dx%d frame buffer\n",
                        width, height);
                exit(1);
            }
            memset(fFb->Bits(), 0, fFb->BitsLength());
        }
        fFbWidth = width;
        fFbHeight = height;
        fFbGeneration++;
    }
    fData = data;
    fDataStride = stride;
    if (data != nullptr) {
        CopyFromSource(0, 0, width, height);
    } else if (!resized && fFb != nullptr) {
        memset(fFb->Bits(), 0, fFb->BitsLength());
    }
    fDirty |= ScreenRect(0, 0, width, height);
    PostUpdate();
}


void HaikuDisplay::Update(int x, int y, int w, int h)
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


void HaikuDisplay::SetCursor(const uint32_t *pixels, int width, int height,
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


void HaikuDisplay::MoveCursor(int x, int y)
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


/* With fMutex held. */
ScreenRect HaikuDisplay::CursorRect() const
{
    if (fCursor.empty() || fCursorNative)
        return ScreenRect();
    return ScreenRect(fCursorX, fCursorY, fCursorWidth, fCursorHeight);
}


/* With fMutex held, on the window's thread: draws the frame buffer, black
   past its edges, and the cursor over both, into one rectangle of the
   view. */
void HaikuDisplay::Paint(BView *view, const ScreenRect &area,
                         const ScreenRect &cursor)
{
    ScreenRect r = area & view_rect(view);
    if (r.Empty())
        return;

    ScreenRect fb = r & ScreenRect(0, 0, fFbWidth, fFbHeight);
    if (!fb.Empty())
        view->DrawBitmap(fFb.get(), to_brect(fb), to_brect(fb));

    ScreenRect outside[2];
    int count = screen_rect_outside(r, fFbWidth, fFbHeight, outside);
    view->SetHighColor(0, 0, 0);
    for (int i = 0; i < count; i++)
        view->FillRect(to_brect(outside[i]));

    ScreenRect c = r & cursor;
    if (c.Empty())
        return;
    int w = c.Width();
    int h = c.Height();
    if (fComposite == nullptr || fComposite->Bounds().IntegerWidth() + 1 < w ||
        fComposite->Bounds().IntegerHeight() + 1 < h) {
        int size_w = w, size_h = h;
        if (fComposite != nullptr) {
            size_w = std::max(size_w, fComposite->Bounds().IntegerWidth() + 1);
            size_h = std::max(size_h,
                              fComposite->Bounds().IntegerHeight() + 1);
        }
        fComposite = std::make_unique<BBitmap>(
            BRect(0, 0, size_w - 1, size_h - 1), B_RGB32);
        if (fComposite->InitCheck() != B_OK) {
            fComposite.reset();
            return;
        }
    }
    uint32_t *dst = static_cast<uint32_t *>(fComposite->Bits());
    int32 dst_stride = fComposite->BytesPerRow() / 4;
    const uint32_t *fb_bits = fFb != nullptr ?
        static_cast<uint32_t *>(fFb->Bits()) : nullptr;
    int32 fb_stride = fFb != nullptr ? fFb->BytesPerRow() / 4 : 0;
    for (int y = 0; y < h; y++) {
        int sy = c.y0 + y;
        for (int x = 0; x < w; x++) {
            int sx = c.x0 + x;
            uint32_t base = 0;
            if (sx < fFbWidth && sy < fFbHeight)
                base = fb_bits[sy * fb_stride + sx];
            uint32_t src = fCursor[(sy - fCursorY) * fCursorWidth +
                                   (sx - fCursorX)];
            dst[y * dst_stride + x] = cursor_blend(base, src);
        }
    }
    view->DrawBitmap(fComposite.get(), BRect(0, 0, w - 1, h - 1),
                     to_brect(c));
}


/* With fMutex held, on the window's thread: the host cursor over the view,
   which is the guest's image when it is shown natively and none otherwise. */
void HaikuDisplay::UpdateHostCursor(BView *view)
{
    std::unique_ptr<BCursor> cursor;

    if (fCursorNative && !fCursor.empty()) {
        BBitmap bitmap(BRect(0, 0, fCursorWidth - 1, fCursorHeight - 1),
                       B_RGBA32);
        if (bitmap.InitCheck() == B_OK) {
            uint8_t *bits = static_cast<uint8_t *>(bitmap.Bits());
            int32 stride = bitmap.BytesPerRow();
            /* ARGB in host order is B, G, R, A bytes, as B_RGBA32 is */
            for (int y = 0; y < fCursorHeight; y++) {
                memcpy(bits + y * stride, &fCursor[y * fCursorWidth],
                       fCursorWidth * 4);
            }
            cursor = std::make_unique<BCursor>(&bitmap,
                                               BPoint(fCursorHotX,
                                                      fCursorHotY));
        }
    }
    if (cursor == nullptr)
        cursor = std::make_unique<BCursor>(B_CURSOR_ID_NO_CURSOR);
    view->SetViewCursor(cursor.get());
    fViewCursor = std::move(cursor);
}


/* Redraws what changed since the last time. */
void HaikuDisplay::DrawUpdate(BView *view)
{
    std::lock_guard<std::mutex> locker(fMutex);

    fUpdatePosted = false;
    ScreenRect dirty = fDirty;
    fDirty = ScreenRect();
    if (fFbGeneration != fShownGeneration) {
        fShownGeneration = fFbGeneration;
        /* a window the user sizes keeps its size; the guest follows it */
        ScreenRect bounds = view_rect(view);
        if (!fResizable && fFbWidth > 0 &&
            (fFbWidth != bounds.Width() || fFbHeight != bounds.Height()))
            view->Window()->ResizeTo(fFbWidth - 1, fFbHeight - 1);
        dirty = view_rect(view);
        fCursorChanged = true;
    }

    if (fCursorImageChanged) {
        fCursorImageChanged = false;
        UpdateHostCursor(view);
    }
    ScreenRect cursor = CursorRect();
    Paint(view, dirty, cursor);
    if (fCursorChanged) {
        fCursorChanged = false;
        Paint(view, fCursorDrawn, cursor);
        Paint(view, cursor, cursor);
        fCursorDrawn = cursor;
    }
    /* the server has read the pixels before the source may change them */
    view->Sync();
}


void HaikuDisplay::DrawView(BView *view, BRect update)
{
    std::lock_guard<std::mutex> locker(fMutex);
    Paint(view, from_brect(update), CursorRect());
}


void HaikuDisplay::ViewResized(int width, int height)
{
    ScreenSource *source;

    DeviceLocker device_locker(fDeviceLock);
    {
        std::lock_guard<std::mutex> locker(fMutex);
        /* a fixed size window only moves when the frame buffer does */
        if (!fResizable)
            return;
        source = fSource;
    }
    if (source != nullptr)
        source->ScreenResized(width, height);
}


/* The key states of an input message, bit 7 of the first byte being key
   0. Reports each key whose state differs from the last one sent. */
void HaikuDisplay::SetKeyStates(const uint8 *states)
{
    DeviceLocker locker(fDeviceLock);

    for (int32 key = 0; key < 128; key++) {
        uint8 mask = 0x80 >> (key & 7);
        bool down = (states[key >> 3] & mask) != 0;
        if (down == ((fKeyStates[key >> 3] & mask) != 0))
            continue;
        fKeyStates[key >> 3] ^= mask;
        int code = haiku_key_to_evdev(key);
        if (code != 0 && fKeyboard != nullptr)
            fKeyboard->SendKeyEvent(down, code);
    }
}


void HaikuDisplay::HandleKeyMessage(BMessage *message)
{
    const void *data;
    ssize_t size;

    /* modifier keys come only as a change of the states */
    if (message->FindData("states", B_UINT8_TYPE, &data, &size) == B_OK &&
        size >= (ssize_t)sizeof(fKeyStates)) {
        SetKeyStates(static_cast<const uint8 *>(data));
        return;
    }
    int32 key;
    if (message->FindInt32("key", &key) != B_OK || key < 0 || key >= 128)
        return;
    uint8 states[sizeof(fKeyStates)];
    memcpy(states, fKeyStates, sizeof(states));
    uint8 mask = 0x80 >> (key & 7);
    if (message->what == B_KEY_DOWN || message->what == B_UNMAPPED_KEY_DOWN)
        states[key >> 3] |= mask;
    else
        states[key >> 3] &= ~mask;
    SetKeyStates(states);
}


void HaikuDisplay::ReleaseKeys()
{
    uint8 states[sizeof(fKeyStates)] {};
    SetKeyStates(states);
}


/* 'message' is the mouse message being handled, for its buttons; nullptr
   keeps the last ones. */
void HaikuDisplay::HandleMouse(BMessage *message, BPoint where, int dz)
{
    int32 buttons;
    if (message != nullptr && message->FindInt32("buttons", &buttons) == B_OK)
        fButtons = buttons;

    unsigned int state = 0;
    if (fButtons & B_PRIMARY_MOUSE_BUTTON)
        state |= 1 << 0;
    if (fButtons & B_SECONDARY_MOUSE_BUTTON)
        state |= 1 << 1;
    if (fButtons & B_TERTIARY_MOUSE_BUTTON)
        state |= 1 << 2;

    BPoint last = fLastWhere;
    fLastWhere = where;

    DeviceLocker device_locker(fDeviceLock);
    if (fPointer == nullptr)
        return;
    int x, y;
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
        x = (std::clamp((int)where.x, 0, width - 1) * 32768) / width;
        y = (std::clamp((int)where.y, 0, height - 1) * 32768) / height;
    } else {
        x = (int)(where.x - last.x);
        y = (int)(where.y - last.y);
    }
    fPointer->SendMouseEvent(x, y, dz, state);
}


void HaikuDisplay::HandleWheel(BMessage *message)
{
    float delta;

    if (message->FindFloat("be:wheel_delta_y", &delta) != B_OK || delta == 0)
        return;
    /* up is negative here and positive for the guest */
    HandleMouse(nullptr, fLastWhere, delta < 0 ? 1 : -1);
}


/* The window's QuitRequested(): closing it asks the machine to shut down,
   which then quits the application and the window with it. */
bool HaikuDisplay::QuitWindow()
{
    if (fQuitting.load())
        return true;
    fRunControl.RequestShutdown(0);
    return false;
}


void HaikuDisplay::Run()
{
    ScreenWindow *window = nullptr;
    int width, height;
    bool resizable;

    {
        std::lock_guard<std::mutex> locker(fMutex);
        width = fSourceWidth;
        height = fSourceHeight;
        resizable = fResizable;
        if (fSource == nullptr || width <= 0 || height <= 0)
            width = 0;
    }
    /* nothing to show, so no window */
    if (width > 0 && !fQuitting.load()) {
        window = new ScreenWindow(*this, width, height, resizable);
        std::lock_guard<std::mutex> locker(fMutex);
        fWindow = BMessenger(window);
        /* whatever arrived before the window existed */
        PostUpdate();
    }
    if (window != nullptr)
        window->Show();
    fApp->Run();
}


void HaikuDisplay::Quit()
{
    fQuitting.store(true);
    fApp->PostMessage(B_QUIT_REQUESTED);
}


std::unique_ptr<HostDisplay> host_display_create(DeviceLock &lock,
                                                 RunControl &run_control)
{
    status_t status;
    auto app = std::make_unique<BApplication>(APP_SIGNATURE, &status);

    if (status != B_OK) {
        fprintf(stderr, "Could not connect to the window system: %s\n",
                strerror(status));
        return nullptr;
    }
    return std::make_unique<HaikuDisplay>(lock, run_control, std::move(app));
}
