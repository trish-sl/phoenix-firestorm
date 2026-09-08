/**
 * @file fsnativepopout.cpp
 * @brief Opt-in Windows pop-outs without additional viewer GL contexts.
 *
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "llviewerprecompiledheaders.h"
#include "fsnativepopout.h"

#if LL_WINDOWS

#include "fsnativepopoutedit.h"
#include "fsnativepopoutpolicy.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfolderview.h"
#include "llpanel.h"
#include "llglslshader.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llmodaldialog.h"
#include "llmultifloater.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llrendertarget.h"
#include "llstartup.h"
#include "lltexteditor.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llwindow.h"
#include "llworldmapview.h"

#include <windowsx.h>
#include <algorithm>
#include <memory>
#include <map>
#include <set>
#include <vector>

namespace
{
constexpr wchar_t WINDOW_CLASS[] = L"FirestormExperimentalPopout";
constexpr F32 FRAME_INTERVAL = 0.1f;
constexpr S32 MAX_SURFACE_SIZE = 2048;

using FSNativePopoutPolicy::SPECS;
// Graphics changes can call reset() from a Preferences callback. Keep its
// native host alive until the callback unwinds, but release GL surfaces now.
U32 sDispatchDepth = 0;
bool sResetPending = false;
bool sFullscreen = false;
std::vector<LLHandle<LLFloater>> sToggleRequests;
std::map<LLHandle<LLFloater>, RECT> sDesktopPositions;
// A native close hides the floater but keeps its opt-in setting. Do not
// recreate it every frame; clear the suppression when the user opens it again.
std::set<LLHandle<LLFloater>> sNativeClosed;

MASK modifiers()
{
    MASK mask = MASK_NONE;
    if (GetKeyState(VK_SHIFT) & 0x8000) mask |= MASK_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mask |= MASK_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mask |= MASK_ALT;
    return mask;
}

class NativePopout
{
public:
    explicit NativePopout(LLFloater* floater) : mFloater(floater->getHandle())
    {
        const auto* spec = FSNativePopoutPolicy::find(floater->getInstanceName());
        mSetting = spec ? spec->setting : nullptr;
    }
    ~NativePopout();

    bool open();
    bool update();
    void draw();
    LLFloater* floater() const { return mFloater.get(); }
    bool hasNativeFocus() const { return GetFocus() == mWindow; }
    const char* setting() const { return mSetting; }
    void requestReturn() { mCloseRequested = true; mFocusOnReturn = true; }
    bool wasNativeClose() const { return mCloseFloater; }
    void closeFromNativeWindow()
    {
        mCloseRequested = true;
        mCloseFloater = true;
        mFocusOnReturn = false;
        if (LLFloater* floater = mFloater.get())
        {
            sNativeClosed.insert(mFloater);
            floater->closeFloater();
        }
    }
    void releaseGL() { mTarget.release(); }

private:
    // Floaters keep their registry identity. Only their parent changes. Limit
    // the legacy singleton substitutions to synchronous UI dispatch/drawing.
    class UIScope
    {
    public:
        explicit UIScope(NativePopout& host)
            : mHost(host), mRoot(LLUI::getInstance()->getRootView()),
              mFloaters(gFloaterView), mScale(LLUI::getScaleFactor()),
              mAppFocus(gFocusMgr.getAppHasFocus())
        {
            ++sDispatchDepth;
            LLUI::getInstance()->setRootView(host.mRoot.get());
            gFloaterView = host.mRoot.get();
            LLUI::setScaleFactor(host.mDisplayScale);
            if (GetFocus() == host.mWindow)
            {
                gFocusMgr.setAppHasFocus(true);
            }
        }

        ~UIScope()
        {
            LLUI::getInstance()->setRootView(mRoot);
            gFloaterView = mFloaters;
            LLUI::setScaleFactor(mScale);
            gFocusMgr.setAppHasFocus(mAppFocus);

            // Profiles and other secondary floaters still belong to the main
            // viewer. Do not accidentally take ownership of them at teardown.
            auto children = *mHost.mRoot->getChildList();
            for (LLView* child : children)
            {
                if (child != mHost.mFloater.get())
                {
                    mFloaters->addChild(child);
                }
            }
            --sDispatchDepth;
        }

    private:
        NativePopout& mHost;
        LLView* mRoot;
        LLFloaterView* mFloaters;
        LLVector2 mScale;
        bool mAppFocus;
    };

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam);
    void mouse(UINT message, WPARAM wparam, LPARAM lparam);
    void key(UINT message, WPARAM wparam, LPARAM lparam);
    void restoreFocus();
    void editMenu(LPARAM position);
    LLFolderView* focusedInventory() const;
    void releaseCapture();
    void paint();
    void reshape();

    const char* mSetting = nullptr;
    HWND mWindow = nullptr;
    RECT mDesktopRect = {};
    std::string mTitle;
    LLHandle<LLFloater> mFloater;
    LLHandle<LLFloater> mOriginalHost;
    std::unique_ptr<LLFloaterView> mRoot;
    LLRect mOriginalRect;
    LLVector2 mDisplayScale;
    S32 mSurfaceWidth = 1;
    S32 mSurfaceHeight = 1;
    bool mCanDrag = false;
    bool mCanResize = false;
    bool mCanMinimize = false;
    bool mCloseRequested = false;
    bool mCloseFloater = false;
    bool mFocusOnReturn = false;
    bool mReady = false;
    bool mMapDragged = false;
    S32 mLastX = 0;
    S32 mLastY = 0;
    S32 mWheelRemainder = 0;
    WCHAR mHighSurrogate = 0;
    LLHandle<LLView> mCapture;
    LLHandle<LLUICtrl> mKeyboardFocus;
    LLRenderTarget mTarget;
    LLTimer mFrameTimer;
    std::vector<U8> mPixels;
    S32 mPixelWidth = 0;
    S32 mPixelHeight = 0;
};

std::vector<std::unique_ptr<NativePopout>> sWindows;
bool sShuttingDown = false;
LLVector2 sDisplayScale(1.f, 1.f);

bool NativePopout::open()
{
    // Never call openFloater/onOpen on an existing editor: doing so can reload
    // its document or reset a Preferences editing session.
    LLFloater* floater = mFloater.get();
    if (!floater || floater->isDead() || !floater->isInVisibleChain()
        || !FSNativePopoutPolicy::find(floater->getInstanceName())
        || !LLFloaterReg::canShowInstance(floater->getInstanceName(), floater->getKey()))
    {
        return false;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LL_WARNS("NativePopout") << "Cannot register native window class: " << GetLastError() << LL_ENDL;
        return false;
    }

    floater->setMinimized(false);
    mFloater = floater->getHandle();
    mOriginalRect = floater->getRect();
    mDisplayScale = sDisplayScale;
    mTitle = floater->getTitle();
    const std::wstring title = ll_convert<std::wstring>(mTitle + " - Firestorm [Ctrl+Shift+F12 to return]");
    if (gFocusMgr.childHasKeyboardFocus(floater))
    {
        if (auto* control = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus()))
        {
            mKeyboardFocus = control->getHandle();
        }
    }

    RECT rect = { 0, 0,
        llclamp(llceil(mOriginalRect.getWidth() * mDisplayScale.mV[VX]), 100, MAX_SURFACE_SIZE),
        llclamp(llceil(mOriginalRect.getHeight() * mDisplayScale.mV[VY]), 100, MAX_SURFACE_SIZE) };
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
    S32 left = CW_USEDEFAULT;
    S32 top = CW_USEDEFAULT;
    if (auto saved = sDesktopPositions.find(mFloater); saved != sDesktopPositions.end())
    {
        // Restore in desktop coordinates, including monitors left of the main
        // monitor. Recover onto a remaining monitor if one was disconnected.
        rect = saved->second;
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info))
        {
            const LONG width = llmin(rect.right - rect.left, info.rcWork.right - info.rcWork.left);
            const LONG height = llmin(rect.bottom - rect.top, info.rcWork.bottom - info.rcWork.top);
            left = llclamp(rect.left, info.rcWork.left, info.rcWork.right - width);
            top = llclamp(rect.top, info.rcWork.top, info.rcWork.bottom - height);
            rect = { 0, 0, width, height };
        }
    }
    mWindow = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST, WINDOW_CLASS, title.c_str(),
        WS_OVERLAPPEDWINDOW, left, top,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, wc.hInstance, this);
    if (!mWindow)
    {
        LL_WARNS("NativePopout") << "Cannot create native window: " << GetLastError() << LL_ENDL;
        return false;
    }

    LLFloaterView::Params params;
    params.name = "native_popout_root";
    params.rect = LLRect(0, mOriginalRect.getHeight(), mOriginalRect.getWidth(), 0);
    mRoot.reset(LLUICtrlFactory::create<LLFloaterView>(params));
    if (LLMultiFloater* host = floater->getHost())
    {
        mOriginalHost = host->getHandle();
        host->removeFloater(floater);
    }
    // removeFloater restores the standalone geometry and capability flags.
    // Save those, not the temporary tab-container settings.
    mOriginalRect = floater->getRect();
    mCanDrag = floater->getCanDrag();
    mCanResize = floater->isResizable();
    mCanMinimize = floater->isMinimizeable();
    mRoot->addChild(floater);
    floater->setCanDrag(false);
    floater->setCanResize(false);
    floater->setCanMinimize(false);
    // Leave close/tear-off controls intact. Rehosting from the usual UI is
    // detected by update(), which retires this native host without stealing it.
    floater->setVisible(true);
    mReady = true;
    reshape();
    ShowWindow(mWindow, SW_SHOWNORMAL);
    SetWindowPos(mWindow, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

NativePopout::~NativePopout()
{
    if (mReady && mDesktopRect.right > mDesktopRect.left)
    {
        sDesktopPositions[mFloater] = mDesktopRect;
    }
    const bool restore_focus = mFocusOnReturn && !sShuttingDown && hasNativeFocus();
    if (mRoot)
    {
        releaseCapture();
        if (gFocusMgr.childHasKeyboardFocus(mRoot.get()))
        {
            if (auto* control = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus()))
            {
                mKeyboardFocus = control->getHandle();
            }
            gFocusMgr.setKeyboardFocus(nullptr);
        }
        if (LLFloater* floater = mFloater.get(); floater && !floater->isDead())
        {
            const bool visible = floater->getVisible();
            const bool returning = floater->getParent() == mRoot.get();
            LLMultiFloater* destination = floater->getHost();
            if (destination)
            {
                // A tear-off/tab action may have already rehosted the floater.
                // Refresh the host's saved flags instead of leaving it with
                // the prototype's disabled resize/minimize capabilities.
                destination->removeFloater(floater);
            }
            else if (returning)
            {
                destination = dynamic_cast<LLMultiFloater*>(mOriginalHost.get());
            }
            floater->setCanDrag(mCanDrag);
            floater->setCanResize(mCanResize);
            floater->setCanMinimize(mCanMinimize);
            if (returning || !floater->getParent())
            {
                gFloaterView->addChild(floater);
            }
            floater->reshape(mOriginalRect.getWidth(), mOriginalRect.getHeight());
            floater->setRect(mOriginalRect);
            // Persist through the public reshape path. Restore the rectangle
            // first so this notification does not move dependent floaters.
            floater->setShape(mOriginalRect, false);
            if (destination)
            {
                destination->addFloater(floater, visible && !sShuttingDown);
            }
            // A close/restriction must not be undone by reattachment.
            floater->setVisible(visible);
        }
    }
    mReady = false;
    if (mWindow)
    {
        DestroyWindow(mWindow); // Never post WM_QUIT for a satellite window.
    }
    if (restore_focus)
    {
        LLUI::getInstance()->getWindow()->bringToFront();
        if (LLUICtrl* control = mKeyboardFocus.get(); control && control->isInVisibleChain())
        {
            control->setFocus(true);
        }
    }
}

void NativePopout::reshape()
{
    LLFloater* floater = mFloater.get();
    if (!mReady || sResetPending || mCloseRequested || !floater || floater->isDead() || IsIconic(mWindow))
    {
        return;
    }
    RECT rect;
    GetClientRect(mWindow, &rect);
    mSurfaceWidth = llclamp(static_cast<S32>(rect.right), 1, MAX_SURFACE_SIZE);
    mSurfaceHeight = llclamp(static_cast<S32>(rect.bottom), 1, MAX_SURFACE_SIZE);
    const S32 width = llmax(1, llfloor(mSurfaceWidth / mDisplayScale.mV[VX]));
    const S32 height = llmax(1, llfloor(mSurfaceHeight / mDisplayScale.mV[VY]));
    UIScope scope(*this);
    mRoot->reshape(width, height);
    floater->reshape(width, height);
    floater->setOrigin(0, 0);
}

bool NativePopout::update()
{
    LLFloater* floater = mFloater.get();
    if (mCloseRequested || !floater || floater->isDead() || !floater->getVisible()
        || floater->getParent() != mRoot.get()
        || !LLFloaterReg::canShowInstance(floater->getInstanceName(), floater->getKey()))
    {
        return false;
    }
    if (mTitle != floater->getTitle())
    {
        mTitle = floater->getTitle();
        const std::wstring title = ll_convert<std::wstring>(mTitle + " - Firestorm [Ctrl+Shift+F12 to return]");
        SetWindowTextW(mWindow, title.c_str());
    }
    // Keep the satellite above the viewer even after the main window is
    // activated or another satellite is clicked.
    SetWindowPos(mWindow, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (mDisplayScale != sDisplayScale)
    {
        mDisplayScale = sDisplayScale;
        reshape();
    }
    MSG msg;
    // The main viewer HWND lives on its window thread. Pump only this host's
    // messages here; all LLUI operations remain on the viewer/main thread.
    for (U32 count = 0; count < 128 && !sResetPending && PeekMessageW(&msg, mWindow, 0, 0, PM_REMOVE); ++count)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    floater = mFloater.get();
    return !mCloseRequested && floater && !floater->isDead() && floater->getVisible()
        && floater->getParent() == mRoot.get();
}

void NativePopout::releaseCapture()
{
    LLView* capture = mCapture.get();
    // Capture-loss callbacks can themselves release Win32 capture, which
    // synchronously dispatches WM_CAPTURECHANGED. Clear our state first.
    mCapture.markDead();
    mMapDragged = false;
    if (capture)
    {
        capture->onMouseCaptureLost();
    }
    if (GetCapture() == mWindow)
    {
        ReleaseCapture();
    }
}

void NativePopout::mouse(UINT message, WPARAM wparam, LPARAM lparam)
{
    if (LLModalDialog::activeCount() || gDisconnected)
    {
        releaseCapture();
        return;
    }
    UIScope scope(*this);
    POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    if (message == WM_MOUSEWHEEL)
    {
        ScreenToClient(mWindow, &point);
    }
    const S32 x = llfloor(point.x / mDisplayScale.mV[VX]);
    const S32 y = llfloor((mSurfaceHeight - 1 - point.y) / mDisplayScale.mV[VY]);
    const MASK mask = modifiers();

    // Never expose a satellite's capture to the main viewer hover/pick loop.
    if (gFocusMgr.getMouseCapture())
    {
        return;
    }
    gFocusMgr.setMouseCapture(mCapture.get());
    LLMouseHandler* target = gFocusMgr.getMouseCapture();
    S32 local_x = x;
    S32 local_y = y;
    if (target)
    {
        target->screenPointToLocal(x, y, &local_x, &local_y);
    }
    else
    {
        target = mRoot.get();
    }

    auto* map = dynamic_cast<LLWorldMapView*>(target);
    switch (message)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(mWindow);
        mMapDragged = false;
        if (message == WM_LBUTTONDBLCLK)
        {
            target->handleDoubleClick(local_x, local_y, mask);
        }
        else
        {
            target->handleMouseDown(local_x, local_y, mask);
        }
        break;
    case WM_LBUTTONUP:
        if (map && mMapDragged)
        {
            // Pan locally, without the map's main-window cursor warp/deltas.
            gFocusMgr.setMouseCapture(nullptr);
        }
        else
        {
            target->handleMouseUp(local_x, local_y, mask);
        }
        break;
    case WM_MOUSEMOVE:
        if (map)
        {
            const S32 dx = x - mLastX;
            const S32 dy = y - mLastY;
            if (dx || dy)
            {
                map->translatePan(dx, dy);
                mMapDragged = true;
            }
        }
        else
        {
            target->handleHover(local_x, local_y, mask);
        }
        break;
    case WM_MOUSEWHEEL:
        mWheelRemainder += GET_WHEEL_DELTA_WPARAM(wparam);
        if (const S32 clicks = mWheelRemainder / WHEEL_DELTA)
        {
            target->handleScrollWheel(local_x, local_y, -clicks);
            mWheelRemainder %= WHEEL_DELTA;
        }
        break;
    }
    mLastX = x;
    mLastY = y;

    LLMouseHandler* capture = gFocusMgr.getMouseCapture();
    if (auto* view = dynamic_cast<LLView*>(capture); view && view->hasAncestor(mRoot.get()))
    {
        mCapture = view->getHandle();
        gFocusMgr.removeMouseCaptureWithoutCallback(capture);
        SetCapture(mWindow);
    }
    else
    {
        mCapture.markDead();
        if (capture)
        {
            // Drag tools and main-window popups are outside this prototype.
            gFocusMgr.setMouseCapture(nullptr);
        }
        if (GetCapture() == mWindow)
        {
            ReleaseCapture();
        }
    }
}

void NativePopout::key(UINT message, WPARAM wparam, LPARAM lparam)
{
    if (LLModalDialog::activeCount() || gDisconnected)
    {
        return;
    }
    KEY key;
    if (message == WM_KEYDOWN && gKeyboard->translateKey(static_cast<U16>(wparam), &key)
        && FSNativePopout::handleKey(key, modifiers(), (lparam & (1LL << 30)) != 0))
    {
        return;
    }
    UIScope scope(*this);
    if (!gFocusMgr.childHasKeyboardFocus(mRoot.get()))
    {
        return;
    }
    LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
    if (message == WM_CHAR)
    {
        const WCHAR ch = static_cast<WCHAR>(wparam);
        if (ch >= 0xD800 && ch <= 0xDBFF)
        {
            mHighSurrogate = ch;
            return;
        }
        llwchar codepoint = ch;
        if (ch >= 0xDC00 && ch <= 0xDFFF)
        {
            if (!mHighSurrogate) return;
            codepoint = 0x10000 + ((mHighSurrogate - 0xD800) << 10) + ch - 0xDC00;
        }
        mHighSurrogate = 0;
        if (codepoint >= 32 && codepoint != 127)
        {
            focus->handleUnicodeChar(codepoint, false);
        }
        return;
    }
    KEY translated;
    if (gKeyboard->translateKey(static_cast<U16>(wparam), &translated))
    {
        const MASK mask = modifiers();
        if (message == WM_KEYUP)
        {
            focus->handleKeyUp(translated, mask, false);
        }
        else
        {
            if (translated == KEY_CONTEXT_MENU || (translated == KEY_F10 && mask == MASK_SHIFT))
            {
                editMenu(static_cast<LPARAM>(-1));
                return;
            }
            const auto command = FSNativePopoutEdit::fromKey(translated, mask);
            if (auto* editor = dynamic_cast<LLTextEditor*>(focus))
            {
                if (FSNativePopoutEdit::execute(*editor, command)) return;
            }
            else if (auto* editor = dynamic_cast<LLLineEditor*>(focus))
            {
                if (FSNativePopoutEdit::execute(*editor, command)) return;
            }
            else if (FSNativePopoutEdit::isClipboardCommand(command))
            {
                if (LLFolderView* inventory = focusedInventory())
                {
                    if (FSNativePopoutEdit::execute(*inventory, command)) return;
                }
            }
            if (!focus->handleKey(translated, mask, false)
                && translated == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
            {
                if (LLFloater* floater = mFloater.get())
                {
                    if (mask == MASK_SHIFT) floater->focusPrevItem(false);
                    else floater->focusNextItem(false);
                }
            }
        }
    }
}

void NativePopout::restoreFocus()
{
    if (LLModalDialog::activeCount() || gDisconnected) return;
    UIScope scope(*this);
    if (gFocusMgr.childHasKeyboardFocus(mRoot.get())) return;
    LLUICtrl* previous = mKeyboardFocus.get();
    if (previous && previous->hasAncestor(mRoot.get())
        && previous->isInVisibleChain() && previous->isInEnabledChain())
    {
        previous->setFocus(true);
    }
    else if (LLFloater* floater = mFloater.get())
    {
        floater->focusFirstItem(true, false);
    }
}

LLFolderView* NativePopout::focusedInventory() const
{
    // Inventory focuses its parent panel, not its folder view. Find the
    // visible tree belonging to that focus; the global Edit handler may still
    // refer to a selection in another native window or another inventory tab.
    std::vector<LLView*> pending{ mRoot.get() };
    while (!pending.empty())
    {
        LLView* view = pending.back();
        pending.pop_back();
        if (!view->getVisible() || !view->getEnabled()) continue;
        if (auto* inventory = dynamic_cast<LLFolderView*>(view))
        {
            LLPanel* panel = inventory->getParentPanel();
            if (panel && gFocusMgr.childHasKeyboardFocus(panel)) return inventory;
            continue; // Do not scan individual inventory items.
        }
        const auto& children = *view->getChildList();
        pending.insert(pending.end(), children.begin(), children.end());
    }
    return nullptr;
}

void NativePopout::editMenu(LPARAM position)
{
    if (LLModalDialog::activeCount() || gDisconnected) return;
    UIScope scope(*this);
    POINT point = { GET_X_LPARAM(position), GET_Y_LPARAM(position) };
    LLView* control = nullptr;
    if (point.x == -1 && point.y == -1)
    {
        // Keyboard invocation (Shift+F10 or the context-menu key).
        if (!gFocusMgr.childHasKeyboardFocus(mRoot.get())) return;
        control = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
        if (!dynamic_cast<LLTextEditor*>(control) && !dynamic_cast<LLLineEditor*>(control))
        {
            control = focusedInventory();
        }
        if (!control) return;
        const LLRect rect = control->calcScreenRect();
        point.x = ll_round(rect.getCenterX() * mDisplayScale.mV[VX]);
        point.y = mSurfaceHeight - ll_round(rect.getCenterY() * mDisplayScale.mV[VY]);
        point.x = llclamp(point.x, 0L, static_cast<LONG>(mSurfaceWidth - 1));
        point.y = llclamp(point.y, 0L, static_cast<LONG>(mSurfaceHeight - 1));
        ClientToScreen(mWindow, &point);
    }
    else
    {
        POINT local = point;
        ScreenToClient(mWindow, &local);
        const S32 x = llfloor(local.x / mDisplayScale.mV[VX]);
        const S32 y = llfloor((mSurfaceHeight - 1 - local.y) / mDisplayScale.mV[VY]);
        for (LLView* view = mRoot->childFromPoint(x, y, true);
             view && view != mRoot.get(); view = view->getParent())
        {
            if (dynamic_cast<LLTextEditor*>(view) || dynamic_cast<LLLineEditor*>(view)
                || dynamic_cast<LLFolderView*>(view))
            {
                control = view;
                break;
            }
        }
    }
    if (!control || !control->hasAncestor(mRoot.get()) || !control->isInEnabledChain()) return;

    auto show_menu = [&](auto& editor)
    {
        using FSNativePopoutEdit::Command;
        struct Item { Command command; const wchar_t* label; };
        constexpr Item items[] = {
            { Command::Undo, L"Undo\tCtrl+Z" }, { Command::Redo, L"Redo\tCtrl+Y" },
            { Command::Cut, L"Cut\tCtrl+X" }, { Command::Copy, L"Copy\tCtrl+C" },
            { Command::Paste, L"Paste\tCtrl+V" }, { Command::Delete, L"Delete" },
            { Command::SelectAll, L"Select all\tCtrl+A" }
        };
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        for (const auto& item : items)
        {
            // Folder views expose clipboard operations; Delete and selection
            // shortcuts belong to their inventory panel's normal key handler.
            if (dynamic_cast<LLFolderView*>(control)
                && !FSNativePopoutEdit::isClipboardCommand(item.command)) continue;
            const UINT flags = MF_STRING | (FSNativePopoutEdit::enabled(editor, item.command)
                ? MF_ENABLED : MF_GRAYED);
            AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.command), item.label);
        }
        // Do not fake a mouse click: doing so would discard the text selection.
        LLUICtrl* focus_control = dynamic_cast<LLUICtrl*>(control);
        if (auto* inventory = dynamic_cast<LLFolderView*>(control))
        {
            focus_control = inventory->getParentPanel();
        }
        if (!focus_control)
        {
            DestroyMenu(menu);
            return;
        }
        focus_control->setFocus(true);
        const LLHandle<LLUICtrl> focus_handle = focus_control->getHandle();
        const LLHandle<LLView> control_handle = control->getHandle();
        const UINT selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, mWindow, nullptr);
        DestroyMenu(menu);
        if (selected && !sResetPending && !mCloseRequested && control_handle.get() == control
            && focus_handle.get() == focus_control && GetFocus() == mWindow
            && gFocusMgr.childHasKeyboardFocus(focus_control) && control->isInEnabledChain()
            && control->hasAncestor(mRoot.get()) && control->isInVisibleChain()
            && !LLModalDialog::activeCount() && !gDisconnected)
        {
            FSNativePopoutEdit::execute(editor, static_cast<Command>(selected));
        }
    };
    if (auto* editor = dynamic_cast<LLTextEditor*>(control)) show_menu(*editor);
    else if (auto* editor = dynamic_cast<LLLineEditor*>(control)) show_menu(*editor);
    else if (auto* inventory = dynamic_cast<LLFolderView*>(control)) show_menu(*inventory);
}

LRESULT CALLBACK NativePopout::windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* host = reinterpret_cast<NativePopout*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        host = static_cast<NativePopout*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        host->mWindow = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    return host ? host->dispatch(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT NativePopout::dispatch(UINT message, WPARAM wparam, LPARAM lparam)
{
    LLFloater* floater = mFloater.get();
    const bool interactive = mReady && !sResetPending && floater && !floater->isDead() && floater->getVisible()
        && floater->getParent() == mRoot.get() && !mCloseRequested;
    switch (message)
    {
    case WM_CLOSE:
        closeFromNativeWindow();
        return 0;
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        reshape();
        [[fallthrough]];
    case WM_MOVE:
        if (!IsIconic(mWindow) && !IsZoomed(mWindow)) GetWindowRect(mWindow, &mDesktopRect);
        return 0;
    case WM_GETMINMAXINFO:
        if (LLFloater* floater = mFloater.get())
        {
            RECT rect = { 0, 0,
                llceil(floater->getMinWidth() * mDisplayScale.mV[VX]),
                llceil(floater->getMinHeight() * mDisplayScale.mV[VY]) };
            AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = { rect.right - rect.left, rect.bottom - rect.top };
            RECT maximum = { 0, 0, MAX_SURFACE_SIZE, MAX_SURFACE_SIZE };
            AdjustWindowRectEx(&maximum, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
            limits->ptMaxTrackSize = { maximum.right - maximum.left, maximum.bottom - maximum.top };
            limits->ptMaxSize = limits->ptMaxTrackSize;
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lparam) != mWindow)
        {
            releaseCapture();
        }
        return 0;
    case WM_KILLFOCUS:
        if (mReady)
        {
            releaseCapture();
            if (gFocusMgr.childHasKeyboardFocus(mRoot.get()))
            {
                if (auto* control = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus()))
                {
                    mKeyboardFocus = control->getHandle();
                }
                gFocusMgr.setKeyboardFocus(nullptr);
            }
        }
        mHighSurrogate = 0;
        return 0;
    case WM_SETFOCUS:
        if (interactive) restoreFocus();
        return 0;
    case WM_CONTEXTMENU:
        if (interactive) editMenu(lparam);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
        if (interactive) mouse(message, wparam, lparam);
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
        if (interactive) key(message, wparam, lparam);
        return 0;
    }
    return DefWindowProcW(mWindow, message, wparam, lparam);
}

void NativePopout::paint()
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(mWindow, &ps);
    RECT rect;
    GetClientRect(mWindow, &rect);
    if (mPixels.empty())
    {
        FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    else
    {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = mPixelWidth;
        info.bmiHeader.biHeight = mPixelHeight; // GL and a positive DIB are both bottom-up.
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, 0, 0, rect.right, rect.bottom, 0, 0,
            mPixelWidth, mPixelHeight, mPixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
    EndPaint(mWindow, &ps);
}

void NativePopout::draw()
{
    LLFloater* floater = mFloater.get();
    if (!floater || floater->isDead() || mCloseRequested || floater->getParent() != mRoot.get() || !floater->getVisible()
        || IsIconic(mWindow) || mFrameTimer.getElapsedTimeF32() < FRAME_INTERVAL)
    {
        return;
    }
    mFrameTimer.reset();
    const S32 width = mSurfaceWidth;
    const S32 height = mSurfaceHeight;
    gGL.flush();
    if (mTarget.getWidth() != static_cast<U32>(width) || mTarget.getHeight() != static_cast<U32>(height))
    {
        if (!mTarget.allocate(width, height, GL_RGBA8))
        {
            LL_WARNS("NativePopout") << "Cannot allocate pop-out surface" << LL_ENDL;
            mCloseRequested = true;
            return;
        }
    }

    UIScope scope(*this);
    const LLRect dirty = LLView::sDirtyRect;
    const bool rect_dirty = LLView::sIsRectDirty;
    const bool drawing = LLView::sIsDrawing;
    LLView::sDirtyRect = mRoot->getLocalRect();
    LLView::sIsDrawing = true;
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLint scissor_box[4];
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
    GLfloat clear_color[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
    GLboolean color_mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    const auto matrix_mode = gGL.getMatrixMode();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    {
        LLGLSUIDefault state;
        LLGLDisable scissor(GL_SCISSOR_TEST);
        mTarget.bindTarget();
        gGL.setColorMask(true, true);
        glClearColor(0.05f, 0.05f, 0.05f, 1.f);
        mTarget.clear(GL_COLOR_BUFFER_BIT);
        gl_state_for_2d(width, height);
        gUIProgram.bind();
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        LLUI::pushMatrix();
        LLRender2D::loadIdentity();
        gGL.scaleUI(mDisplayScale.mV[VX], mDisplayScale.mV[VY], 1.f);
        // No desktop-edge snapping or floater repositioning during rendering.
        mRoot->LLView::draw();
        LLUI::popMatrix();
        gGL.flush();

        // Avoid creating/sharing GL contexts in the platform backends. This
        // deliberately bounded readback is a prototype transport, not the
        // intended high-refresh production presentation path.
        GLint pack_buffer, pack_alignment, pack_row_length, pack_skip_rows, pack_skip_pixels;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        mPixels.resize(static_cast<size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, mPixels.data());
        mPixelWidth = width;
        mPixelHeight = height;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
        glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
        mTarget.flush();
    }
    if (shader) shader->bind();
    else gUIProgram.unbind();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(matrix_mode);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    gGL.setColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    LLView::sDirtyRect = dirty;
    LLView::sIsRectDirty = rect_dirty;
    LLView::sIsDrawing = drawing;
    InvalidateRect(mWindow, nullptr, FALSE);
}
}

bool FSNativePopout::handleKey(KEY key, MASK mask, bool repeated)
{
    if (!FSNativePopoutPolicy::isToggleShortcut(key, mask)) return false;
    if (repeated || sShuttingDown || sResetPending || sFullscreen || gDisconnected
        || LLModalDialog::activeCount() || LLStartUp::getStartupState() != STATE_STARTED)
    {
        return true;
    }
    LLFloater* selected = nullptr;
    for (const auto& window : sWindows)
    {
        if (window->hasNativeFocus()) selected = window->floater();
    }
    if (!selected)
    {
        for (LLView* view = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus()); view; view = view->getParent())
        {
            if ((selected = dynamic_cast<LLFloater*>(view))) break;
        }
    }
    if (selected && !selected->isDead() && FSNativePopoutPolicy::find(selected->getInstanceName()))
    {
        sToggleRequests.push_back(selected->getHandle());
    }
    return true;
}

void FSNativePopout::update(bool fullscreen, const LLVector2& display_scale)
{
    sFullscreen = fullscreen;
    if (sShuttingDown || LLStartUp::getStartupState() != STATE_STARTED) return;
    sDisplayScale = display_scale;
    if (fullscreen || gDisconnected || sResetPending)
    {
        reset();
        return;
    }
    for (auto it = sWindows.begin(); it != sWindows.end();)
    {
        const char* setting = (*it)->setting();
        const bool keep = (!setting || gSavedSettings.getBOOL(setting)) && (*it)->update();
        // Preferences may have reset graphics inside the dispatched callback.
        if (sResetPending)
        {
            reset();
            return;
        }
        if (!keep)
        {
            if (setting && !(*it)->wasNativeClose()) gSavedSettings.setBOOL(setting, false);
            it = sWindows.erase(it);
        }
        else ++it;
    }

    auto open = [](LLFloater* floater)
    {
        if (!floater) return false;
        auto candidate = std::make_unique<NativePopout>(floater);
        if (!candidate->open()) return false;
        if (const char* setting = candidate->setting()) gSavedSettings.setBOOL(setting, true);
        sWindows.push_back(std::move(candidate));
        return true;
    };
    std::vector<LLHandle<LLFloater>> requests;
    requests.swap(sToggleRequests);
    for (const auto& handle : requests)
    {
        LLFloater* floater = handle.get();
        if (!floater || floater->isDead()) continue;
        auto existing = std::find_if(sWindows.begin(), sWindows.end(),
            [floater](const auto& window) { return window->floater() == floater; });
        if (existing != sWindows.end())
        {
            (*existing)->requestReturn();
            if (const char* setting = (*existing)->setting()) gSavedSettings.setBOOL(setting, false);
            sWindows.erase(existing);
        }
        else open(floater);
    }
    for (const auto& spec : SPECS)
    {
        if (!spec.setting || !gSavedSettings.getBOOL(spec.setting)) continue;
        const bool hosted = std::any_of(sWindows.begin(), sWindows.end(),
            [&spec](const auto& window) { return window->setting() == spec.setting; });
        if (hosted) continue;
        const std::string name(spec.name);
        LLFloater* floater = LLFloaterReg::findInstance(name);
        if (floater && floater->isInVisibleChain())
        {
            sNativeClosed.erase(floater->getHandle());
        }
        if (floater && sNativeClosed.find(floater->getHandle()) != sNativeClosed.end())
        {
            continue;
        }
        if (!floater || !floater->isInVisibleChain()) floater = LLFloaterReg::showInstance(name);
        if (!open(floater)) gSavedSettings.setBOOL(spec.setting, false);
    }
    for (auto it = sDesktopPositions.begin(); it != sDesktopPositions.end();)
    {
        if (it->first.isDead()) it = sDesktopPositions.erase(it);
        else ++it;
    }
}

void FSNativePopout::draw()
{
    if (sShuttingDown || sResetPending || gDisconnected || gGLManager.mIsDisabled
        || !gUIProgram.isComplete() || LLStartUp::getStartupState() != STATE_STARTED)
    {
        return;
    }
    for (auto& window : sWindows)
    {
        if (window) window->draw();
    }
}

void FSNativePopout::shutdown()
{
    sShuttingDown = true;
    reset();
}

void FSNativePopout::reset()
{
    sToggleRequests.clear();
    for (const auto& spec : SPECS)
    {
        if (spec.setting) gSavedSettings.setBOOL(spec.setting, false);
    }
    if (sDispatchDepth)
    {
        sResetPending = true;
        for (auto& window : sWindows) window->releaseGL();
        return;
    }
    sResetPending = false;
    // Empty the manager first, so focus callbacks during teardown cannot find
    // a partially destroyed host.
    std::vector<std::unique_ptr<NativePopout>> retiring;
    retiring.swap(sWindows);
}

#else

bool FSNativePopout::handleKey(KEY, MASK, bool) { return false; }
void FSNativePopout::update(bool, const LLVector2&) {}
void FSNativePopout::draw() {}
void FSNativePopout::reset() {}
void FSNativePopout::shutdown() {}

#endif
