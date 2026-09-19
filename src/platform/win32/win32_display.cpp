/*
 * Host display: a Win32 window
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
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>

#include <windows.h>
#include <windowsx.h>
#include <imm.h>

#include "device_lock.h"
#include "platform_backends.h"
#include "run_control.h"
#include "screen_rect.h"
#include "win32_keymap.h"
#include "WaylandKeycodes.h"

#define KEYCODE_MAX 127

/* posted to the window */
#define WM_APP_UPDATE (WM_APP + 0)
#define WM_APP_QUIT   (WM_APP + 1)

static const wchar_t kWindowClass[] = L"TinyEMU";


static RECT to_rect(const ScreenRect &r)
{
    RECT res = {r.x0, r.y0, r.x1, r.y1};
    return res;
}


/* 'h' rows of 32 bit xRGB pixels, 'row_pixels' apart, starting 'src_x'
   pixels into the first row, drawn at (x, y). */
static void draw_pixels(HDC hdc, int x, int y, int w, int h,
                        const uint32_t *rows, int row_pixels, int src_x)
{
    BITMAPINFO bmi = {};

    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = row_pixels;
    bmi.bmiHeader.biHeight = -h; /* top down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, x, y, w, h, src_x, 0, 0, h, rows, &bmi,
                      DIB_RGB_COLORS);
}


class Win32Display final: public HostDisplay {
private:
    DeviceLock &fDeviceLock;
    RunControl &fRunControl;

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
    /* the copy the window is drawn from, fFbWidth pixels per row */
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
    HWND fWindow = nullptr;
    /* an update message is queued and not yet handled */
    bool fUpdatePosted = false;
    bool fQuitRequested = false;

    /* with the device lock held */
    KeyboardTarget *fKeyboard = nullptr;
    PointerTarget *fPointer = nullptr;

    /* the window's thread */
    DWORD fStyle = 0;
    bool fWindowResizable = false;
    int fClientWidth = 0;
    int fClientHeight = 0;
    uint32_t fShownGeneration = 0;
    ScreenRect fCursorDrawn;
    HCURSOR fHostCursor = nullptr;
    /* the size is being set here rather than by the user */
    bool fSettingSize = false;
    uint8_t fKeyPressed[KEYCODE_MAX + 1] {};
    bool fHaveLastPos = false;
    int fLastX = 0;
    int fLastY = 0;
    int fWheel = 0;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam);
    LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

    void PostUpdate();
    void CopyFromSource(int x, int y, int w, int h);
    ScreenRect CursorRect() const;
    void Paint(HDC hdc, const ScreenRect &area);
    void Invalidate(const ScreenRect &r);

    /* the window's thread */
    bool CreateScreenWindow(int width, int height, bool resizable);
    void HandleUpdate();
    void SetClientSize(int width, int height);
    void SetHostCursor(const std::vector<uint32_t> &pixels, int width,
                       int height, int hot_x, int hot_y);
    void HandleSize(int width, int height);
    void HandleDpiChanged(WPARAM wparam, const RECT *suggested);
    void HandleKey(WPARAM vk, LPARAM lparam, bool down);
    void SendKey(int code, bool down);
    void ReleaseKeys();
    void HandleMouse(int x, int y, WPARAM keys, int dz);
    void HandleWheel(WPARAM wparam, LPARAM lparam);

public:
    Win32Display(DeviceLock &lock, RunControl &run_control):
        fDeviceLock(lock), fRunControl(run_control) {}
    ~Win32Display() override;

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


Win32Display::~Win32Display()
{
    if (fHostCursor != nullptr)
        DestroyCursor(fHostCursor);
}


//#pragma mark - calls from the devices

/* With fMutex held. Fails when the window's queue is full. */
void Win32Display::PostUpdate()
{
    if (fWindow != nullptr && !fUpdatePosted)
        fUpdatePosted = PostMessageW(fWindow, WM_APP_UPDATE, 0, 0);
}


void Win32Display::SetTarget(KeyboardTarget *target)
{
    fKeyboard = target;
}


void Win32Display::SetTarget(PointerTarget *target)
{
    fPointer = target;
}


void Win32Display::SetSource(ScreenSource *source, int width, int height)
{
    std::lock_guard<std::mutex> locker(fMutex);
    fSource = source;
    fSourceWidth = width;
    fSourceHeight = height;
    fResizable = source != nullptr && source->Resizable();
}


/* With fMutex held: copies a rectangle, already clipped, into fFb. */
void Win32Display::CopyFromSource(int x, int y, int w, int h)
{
    for (int i = 0; i < h; i++) {
        memcpy(&fFb[(y + i) * fFbWidth + x],
               fData + (y + i) * fDataStride + x * 4, w * 4);
    }
}


void Win32Display::SetFramebuffer(uint8_t *data, int width, int height,
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
        fFb.assign((size_t)width * height, 0);
        fFbWidth = width;
        fFbHeight = height;
        fFbGeneration++;
    }
    fData = data;
    fDataStride = stride;
    if (data != nullptr) {
        CopyFromSource(0, 0, width, height);
    } else if (!resized) {
        std::fill(fFb.begin(), fFb.end(), 0);
    }
    fDirty |= ScreenRect(0, 0, width, height);
    PostUpdate();
}


void Win32Display::Update(int x, int y, int w, int h)
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


void Win32Display::SetCursor(const uint32_t *pixels, int width, int height,
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


void Win32Display::MoveCursor(int x, int y)
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


void Win32Display::Quit()
{
    std::lock_guard<std::mutex> locker(fMutex);
    fQuitRequested = true;
    if (fWindow != nullptr)
        PostMessageW(fWindow, WM_APP_QUIT, 0, 0);
    fCond.notify_all();
}


//#pragma mark - drawing

/* With fMutex held: where the cursor is drawn over the frame buffer. */
ScreenRect Win32Display::CursorRect() const
{
    if (fCursor.empty() || fCursorNative)
        return ScreenRect();
    return ScreenRect(fCursorX, fCursorY, fCursorWidth, fCursorHeight);
}


/* With fMutex held, on the window's thread: draws the frame buffer, black
   past its edges, and the cursor over both, into one rectangle of the
   window. */
void Win32Display::Paint(HDC hdc, const ScreenRect &area)
{
    ScreenRect fb = area & ScreenRect(0, 0, fFbWidth, fFbHeight);
    if (!fb.Empty()) {
        draw_pixels(hdc, fb.x0, fb.y0, fb.Width(), fb.Height(),
                    &fFb[fb.y0 * fFbWidth], fFbWidth, fb.x0);
    }

    ScreenRect outside[2];
    int count = screen_rect_outside(area, fFbWidth, fFbHeight, outside);
    for (int i = 0; i < count; i++) {
        RECT rc = to_rect(outside[i]);
        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }

    ScreenRect c = area & CursorRect();
    if (c.Empty())
        return;
    int w = c.Width();
    int h = c.Height();
    std::vector<uint32_t> pixels((size_t)w * h);
    for (int y = 0; y < h; y++) {
        int sy = c.y0 + y;
        for (int x = 0; x < w; x++) {
            int sx = c.x0 + x;
            uint32_t base = 0;
            if (sx < fFbWidth && sy < fFbHeight)
                base = fFb[sy * fFbWidth + sx];
            uint32_t src = fCursor[(sy - fCursorY) * fCursorWidth +
                                   (sx - fCursorX)];
            pixels[y * w + x] = cursor_blend(base, src);
        }
    }
    draw_pixels(hdc, c.x0, c.y0, w, h, pixels.data(), w, 0);
}


void Win32Display::Invalidate(const ScreenRect &r)
{
    if (r.Empty())
        return;
    RECT rc = to_rect(r);
    InvalidateRect(fWindow, &rc, FALSE);
}


/* Takes in what the devices changed since the last time. */
void Win32Display::HandleUpdate()
{
    int follow_width = 0, follow_height = 0;
    bool cursor_image = false;
    std::vector<uint32_t> cursor;
    int cursor_width = 0, cursor_height = 0, hot_x = 0, hot_y = 0;

    {
        std::lock_guard<std::mutex> locker(fMutex);

        fUpdatePosted = false;
        if (fFbGeneration != fShownGeneration) {
            fShownGeneration = fFbGeneration;
            /* a window the user sizes keeps its size; the guest follows it */
            if (!fWindowResizable && fFbWidth > 0 &&
                (fFbWidth != fClientWidth || fFbHeight != fClientHeight)) {
                follow_width = fFbWidth;
                follow_height = fFbHeight;
            }
            InvalidateRect(fWindow, nullptr, FALSE);
            fCursorChanged = true;
        } else {
            Invalidate(fDirty);
        }
        fDirty = ScreenRect();

        if (fCursorImageChanged) {
            fCursorImageChanged = false;
            cursor_image = true;
            if (fCursorNative) {
                cursor = fCursor;
                cursor_width = fCursorWidth;
                cursor_height = fCursorHeight;
                hot_x = fCursorHotX;
                hot_y = fCursorHotY;
            }
        }
        if (fCursorChanged) {
            fCursorChanged = false;
            ScreenRect c = CursorRect();
            Invalidate(fCursorDrawn);
            Invalidate(c);
            fCursorDrawn = c;
        }
    }

    /* Outside the lock: resizing sends the window messages at once. */
    if (follow_width > 0)
        SetClientSize(follow_width, follow_height);
    if (cursor_image)
        SetHostCursor(cursor, cursor_width, cursor_height, hot_x, hot_y);
}


//#pragma mark - the window

void Win32Display::SetClientSize(int width, int height)
{
    RECT window, client;

    GetWindowRect(fWindow, &window);
    GetClientRect(fWindow, &client);
    int frame_width = (window.right - window.left) - client.right;
    int frame_height = (window.bottom - window.top) - client.bottom;
    fSettingSize = true;
    SetWindowPos(fWindow, nullptr, 0, 0, width + frame_width,
                 height + frame_height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    fSettingSize = false;
}


/* The host cursor over the window: the guest's image when it is shown
   natively, and none otherwise. */
void Win32Display::SetHostCursor(const std::vector<uint32_t> &pixels,
                                 int width, int height, int hot_x, int hot_y)
{
    HCURSOR cursor = nullptr;

    if (!pixels.empty()) {
        BITMAPV5HEADER bi = {};
        bi.bV5Size = sizeof(bi);
        bi.bV5Width = width;
        bi.bV5Height = -height; /* top down */
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00ff0000;
        bi.bV5GreenMask = 0x0000ff00;
        bi.bV5BlueMask = 0x000000ff;
        bi.bV5AlphaMask = 0xff000000;
        void *bits = nullptr;
        HDC hdc = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(hdc,
                                         reinterpret_cast<BITMAPINFO *>(&bi),
                                         DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, hdc);
        /* the alpha channel decides; the mask is only required */
        std::vector<uint8_t> zeros((size_t)((width + 15) / 16) * 2 * height);
        HBITMAP mask = CreateBitmap(width, height, 1, 1, zeros.data());
        if (color != nullptr && mask != nullptr) {
            memcpy(bits, pixels.data(), (size_t)width * height * 4);
            ICONINFO info = {};
            info.fIcon = FALSE;
            info.xHotspot = hot_x;
            info.yHotspot = hot_y;
            info.hbmMask = mask;
            info.hbmColor = color;
            cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
        }
        if (color != nullptr)
            DeleteObject(color);
        if (mask != nullptr)
            DeleteObject(mask);
    }

    HCURSOR old = fHostCursor;
    fHostCursor = cursor;
    /* the cursor over the window now, rather than at the next motion */
    POINT pt;
    RECT client;
    if (GetCursorPos(&pt) && WindowFromPoint(pt) == fWindow) {
        ScreenToClient(fWindow, &pt);
        GetClientRect(fWindow, &client);
        if (PtInRect(&client, pt))
            ::SetCursor(fHostCursor);
    }
    if (old != nullptr)
        DestroyCursor(old);
}


void Win32Display::HandleSize(int width, int height)
{
    fClientWidth = width;
    fClientHeight = height;
    InvalidateRect(fWindow, nullptr, FALSE);
    /* a fixed size window only changes size when the frame buffer does */
    if (!fWindowResizable || fSettingSize)
        return;

    DeviceLocker device_locker(fDeviceLock);
    ScreenSource *source;
    {
        std::lock_guard<std::mutex> locker(fMutex);
        source = fSource;
    }
    if (source != nullptr)
        source->ScreenResized(std::max(fClientWidth, 1),
                              std::max(fClientHeight, 1));
}


/* The window moved to a monitor of another scale. It keeps its size in
   pixels, so that the frame buffer is shown one to one. */
void Win32Display::HandleDpiChanged(WPARAM wparam, const RECT *suggested)
{
    typedef BOOL (WINAPI *AdjustForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustForDpi adjust = reinterpret_cast<AdjustForDpi>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"),
                       "AdjustWindowRectExForDpi"));
    RECT rc = {0, 0, fClientWidth, fClientHeight};

    if (adjust == nullptr ||
        !adjust(&rc, fStyle, FALSE, 0, HIWORD(wparam))) {
        rc = *suggested;
        OffsetRect(&rc, -rc.left, -rc.top);
    }
    fSettingSize = true;
    SetWindowPos(fWindow, nullptr, suggested->left, suggested->top,
                 rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    fSettingSize = false;
}


//#pragma mark - input

/* With the device lock held. */
void Win32Display::SendKey(int code, bool down)
{
    if (code <= 0 || code > KEYCODE_MAX)
        return;
    fKeyPressed[code] = down;
    if (fKeyboard != nullptr)
        fKeyboard->SendKeyEvent(down, code);
}


void Win32Display::HandleKey(WPARAM vk, LPARAM lparam, bool down)
{
    unsigned scancode = (lparam >> 16) & 0xff;
    bool extended = (lparam >> 24) & 1;

    if (vk == VK_CONTROL && !extended) {
        /* AltGr comes as a left Ctrl and a right Alt at the same moment;
           the Ctrl is made up and is dropped. */
        MSG next;
        if (PeekMessageW(&next, fWindow, WM_KEYFIRST, WM_KEYLAST,
                         PM_NOREMOVE) &&
            next.wParam == VK_MENU && ((next.lParam >> 24) & 1) &&
            next.time == (DWORD)GetMessageTime())
            return;
    }
    if (scancode == 0) {
        /* made up by software, which may leave the scan code out */
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        scancode = sc & 0xff;
        extended = (sc >> 8) == 0xe0;
    }
    int code = win32_key_to_evdev(vk, scancode, extended);

    DeviceLocker locker(fDeviceLock);
    /* auto repeat; the guest repeats by itself */
    if (code > 0 && code <= KEYCODE_MAX && !(down && fKeyPressed[code]))
        SendKey(code, down);
    /* With both Shift keys down, releasing one sends nothing. */
    if (fKeyPressed[KEY_LEFTSHIFT] && !(GetKeyState(VK_LSHIFT) & 0x8000))
        SendKey(KEY_LEFTSHIFT, false);
    if (fKeyPressed[KEY_RIGHTSHIFT] && !(GetKeyState(VK_RSHIFT) & 0x8000))
        SendKey(KEY_RIGHTSHIFT, false);
}


/* The keys held now are released where the guest cannot see it. */
void Win32Display::ReleaseKeys()
{
    DeviceLocker locker(fDeviceLock);
    for (int code = 1; code <= KEYCODE_MAX; code++) {
        if (fKeyPressed[code])
            SendKey(code, false);
    }
}


/* (x, y) is in the window; 'keys' are the MK_ flags of the message. */
void Win32Display::HandleMouse(int x, int y, WPARAM keys, int dz)
{
    unsigned int state = 0;
    if (keys & MK_LBUTTON)
        state |= 1 << 0;
    if (keys & MK_RBUTTON)
        state |= 1 << 1;
    if (keys & MK_MBUTTON)
        state |= 1 << 2;

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
        fPointer->SendMouseEvent(x, y, dz, state);
    } else {
        fPointer->SendMouseEvent(dx, dy, dz, state);
    }
}


void Win32Display::HandleWheel(WPARAM wparam, LPARAM lparam)
{
    POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};

    ScreenToClient(fWindow, &pt);
    /* one step per notch; a fine grained wheel sends parts of one */
    fWheel += GET_WHEEL_DELTA_WPARAM(wparam);
    while (fWheel >= WHEEL_DELTA) {
        fWheel -= WHEEL_DELTA;
        HandleMouse(pt.x, pt.y, GET_KEYSTATE_WPARAM(wparam), 1);
    }
    while (fWheel <= -WHEEL_DELTA) {
        fWheel += WHEEL_DELTA;
        HandleMouse(pt.x, pt.y, GET_KEYSTATE_WPARAM(wparam), -1);
    }
}


//#pragma mark - messages

LRESULT CALLBACK Win32Display::WindowProc(HWND hwnd, UINT msg, WPARAM wparam,
                                          LPARAM lparam)
{
    Win32Display *display;

    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
        display = static_cast<Win32Display *>(cs->lpCreateParams);
        {
            std::lock_guard<std::mutex> locker(display->fMutex);
            display->fWindow = hwnd;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(display));
    } else {
        display = reinterpret_cast<Win32Display *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (display == nullptr)
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    return display->HandleMessage(msg, wparam, lparam);
}


LRESULT Win32Display::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_APP_UPDATE:
        HandleUpdate();
        return 0;

    case WM_APP_QUIT:
        DestroyWindow(fWindow);
        return 0;

    case WM_CLOSE:
        /* the machine's shutdown closes the window */
        fRunControl.RequestShutdown(0);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(fWindow, &ps);
        {
            std::lock_guard<std::mutex> locker(fMutex);
            const RECT &rc = ps.rcPaint;
            Paint(hdc, ScreenRect(rc.left, rc.top, rc.right - rc.left,
                                  rc.bottom - rc.top));
            /* drawn before the devices may change the pixels */
            GdiFlush();
        }
        EndPaint(fWindow, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        /* everything is painted */
        return 1;

    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
            HandleSize(LOWORD(lparam), HIWORD(lparam));
        return 0;

    case WM_DPICHANGED:
        HandleDpiChanged(wparam, reinterpret_cast<const RECT *>(lparam));
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            ::SetCursor(fHostCursor);
            return TRUE;
        }
        break;

    /* The system keys are the guest's too: Alt and F10 open no menu, and
       Alt + F4 does not close the window. */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        HandleKey(wparam, lparam, true);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        HandleKey(wparam, lparam, false);
        return 0;

    case WM_KILLFOCUS:
        ReleaseKeys();
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        /* keep the motion while a button is held outside the window */
        SetCapture(fWindow);
        HandleMouse(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), wparam, 0);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if ((wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0)
            ReleaseCapture();
        HandleMouse(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), wparam, 0);
        return 0;
    case WM_MOUSEMOVE:
        HandleMouse(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), wparam, 0);
        return 0;
    case WM_MOUSEWHEEL:
        HandleWheel(wparam, lparam);
        return 0;
    }
    return DefWindowProcW(fWindow, msg, wparam, lparam);
}


//#pragma mark - running

/* The window shows the frame buffer one to one, so it tells Windows that it
   scales nothing itself rather than be stretched on a scaled monitor. */
static void set_dpi_awareness()
{
    typedef BOOL (WINAPI *SetContext)(HANDLE);
    auto set_context = reinterpret_cast<SetContext>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"),
                       "SetProcessDpiAwarenessContext"));

    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, from Windows 10 1703 */
    if (set_context != nullptr && set_context(reinterpret_cast<HANDLE>(-4)))
        return;
    SetProcessDPIAware();
}


bool Win32Display::CreateScreenWindow(int width, int height, bool resizable)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = nullptr;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    fStyle = WS_OVERLAPPEDWINDOW;
    if (!resizable)
        fStyle &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    fWindowResizable = resizable;

    RECT rc = {0, 0, width, height};
    AdjustWindowRectEx(&rc, fStyle, FALSE, 0);
    fSettingSize = true;
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"TinyEMU", fStyle,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, wc.hInstance, this);
    fSettingSize = false;
    if (hwnd == nullptr) {
        fprintf(stderr, "Could not create the window (error %lu)\n",
                GetLastError());
        return false;
    }
    /* The keys are the guest's: no input method composes text from them. */
    ImmAssociateContext(hwnd, nullptr);
    /* the frame's size on the window's monitor may differ from the guess */
    SetClientSize(width, height);
    return true;
}


void Win32Display::Run()
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

    set_dpi_awareness();
    if (!CreateScreenWindow(width, height, resizable)) {
        fRunControl.RequestShutdown(1);
        std::unique_lock<std::mutex> locker(fMutex);
        fWindow = nullptr;
        fCond.wait(locker, [this]() {return fQuitRequested;});
        return;
    }
    {
        std::lock_guard<std::mutex> locker(fMutex);
        /* whatever arrived before the window existed */
        PostUpdate();
        if (fQuitRequested)
            PostMessageW(fWindow, WM_APP_QUIT, 0, 0);
    }
    ShowWindow(fWindow, SW_SHOWNORMAL);

    /* Keys go to the guest as scan codes, so no message is translated into
       characters. */
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        DispatchMessageW(&msg);

    std::lock_guard<std::mutex> locker(fMutex);
    fWindow = nullptr;
}


std::unique_ptr<HostDisplay> host_display_create(DeviceLock &lock,
                                                 RunControl &run_control)
{
    return std::make_unique<Win32Display>(lock, run_control);
}
