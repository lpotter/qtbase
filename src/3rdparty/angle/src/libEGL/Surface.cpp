//
// Copyright (c) 2002-2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// Surface.cpp: Implements the egl::Surface class, representing a drawing surface
// such as the client area of a window, including any back buffers.
// Implements EGLSurface and related functionality. [EGL 1.4] section 2.2 page 3.

#include <tchar.h>

#include <algorithm>

#include "libEGL/Surface.h"

#include "common/debug.h"
#include "libGLESv2/Texture.h"
#include "libGLESv2/renderer/SwapChain.h"
#include "libGLESv2/main.h"

#include "libEGL/main.h"
#include "libEGL/Display.h"

#if defined(ANGLE_PLATFORM_WINRT)
#  include "wrl.h"
#  include "windows.graphics.display.h"
#  include "windows.ui.core.h"
using namespace ABI::Windows::Graphics::Display;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::UI::Core;
using namespace Microsoft::WRL;
#endif

namespace egl
{

Surface::Surface(Display *display, const Config *config, EGLNativeWindowType window, EGLint fixedSize, EGLint width, EGLint height, EGLint postSubBufferSupported)
    : mDisplay(display), mConfig(config), mWindow(window), mPostSubBufferSupported(postSubBufferSupported)
{
    mRenderer = mDisplay->getRenderer();
    mSwapChain = NULL;
    mShareHandle = NULL;
    mTexture = NULL;
    mTextureFormat = EGL_NO_TEXTURE;
    mTextureTarget = EGL_NO_TEXTURE;

    mPixelAspectRatio = (EGLint)(1.0 * EGL_DISPLAY_SCALING);   // FIXME: Determine actual pixel aspect ratio
    mRenderBuffer = EGL_BACK_BUFFER;
    mSwapBehavior = EGL_BUFFER_PRESERVED;
    mSwapInterval = -1;
    mWidth = width;
    mHeight = height;
    setSwapInterval(1);
    mFixedSize = fixedSize;
    mSwapFlags = rx::SWAP_NORMAL;
#if defined(ANGLE_PLATFORM_WINRT)
    if (mWindow)
        mWindow->AddRef();
    mScaleFactor = 1.0;
    mSizeToken.value = 0;
    mDpiToken.value = 0;
#   if WINAPI_FAMILY==WINAPI_FAMILY_PHONE_APP
    mOrientationToken.value = 0;
#   endif
#endif

    subclassWindow();
}

Surface::Surface(Display *display, const Config *config, HANDLE shareHandle, EGLint width, EGLint height, EGLenum textureFormat, EGLenum textureType)
    : mDisplay(display), mWindow(NULL), mConfig(config), mShareHandle(shareHandle), mWidth(width), mHeight(height), mPostSubBufferSupported(EGL_FALSE)
{
    mRenderer = mDisplay->getRenderer();
    mSwapChain = NULL;
    mWindowSubclassed = false;
    mTexture = NULL;
    mTextureFormat = textureFormat;
    mTextureTarget = textureType;

    mPixelAspectRatio = (EGLint)(1.0 * EGL_DISPLAY_SCALING);   // FIXME: Determine actual pixel aspect ratio
    mRenderBuffer = EGL_BACK_BUFFER;
    mSwapBehavior = EGL_BUFFER_PRESERVED;
    mSwapInterval = -1;
    setSwapInterval(1);
    // This constructor is for offscreen surfaces, which are always fixed-size.
    mFixedSize = EGL_TRUE;
    mSwapFlags = rx::SWAP_NORMAL;
#if defined(ANGLE_PLATFORM_WINRT)
    mScaleFactor = 1.0;
    mSizeToken.value = 0;
    mDpiToken.value = 0;
#   if WINAPI_FAMILY==WINAPI_FAMILY_PHONE_APP
    mOrientationToken.value = 0;
#   endif
#endif
}

Surface::~Surface()
{
#if defined(ANGLE_PLATFORM_WINRT)
    if (mSizeToken.value) {
        ComPtr<ICoreWindow> coreWindow;
        HRESULT hr = mWindow->QueryInterface(coreWindow.GetAddressOf());
        ASSERT(SUCCEEDED(hr));

        hr = coreWindow->remove_SizeChanged(mSizeToken);
        ASSERT(SUCCEEDED(hr));
    }
    if (mDpiToken.value) {
        ComPtr<IDisplayInformation> displayInformation;
        HRESULT hr = mDisplay->getDisplayId()->QueryInterface(displayInformation.GetAddressOf());
        ASSERT(SUCCEEDED(hr));

        hr = displayInformation->remove_DpiChanged(mDpiToken);
        ASSERT(SUCCEEDED(hr));
    }
#   if WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
    if (mOrientationToken.value) {
        ComPtr<IDisplayInformation> displayInformation;
        HRESULT hr = mDisplay->getDisplayId()->QueryInterface(displayInformation.GetAddressOf());
        ASSERT(SUCCEEDED(hr));

        hr = displayInformation->remove_OrientationChanged(mOrientationToken);
        ASSERT(SUCCEEDED(hr));
    }
#   endif
#endif
    unsubclassWindow();
    release();
}

bool Surface::initialize()
{
#if defined(ANGLE_PLATFORM_WINRT)
    if (!mFixedSize) {
        HRESULT hr;
        ComPtr<IDisplayInformation> displayInformation;
        hr = mDisplay->getDisplayId()->QueryInterface(displayInformation.GetAddressOf());
        ASSERT(SUCCEEDED(hr));
        onDpiChanged(displayInformation.Get(), 0);
        hr = displayInformation->add_DpiChanged(Callback<ITypedEventHandler<DisplayInformation *, IInspectable *>>(this, &Surface::onDpiChanged).Get(),
                                                &mDpiToken);
        ASSERT(SUCCEEDED(hr));

#   if WINAPI_FAMILY==WINAPI_FAMILY_PHONE_APP
        onOrientationChanged(displayInformation.Get(), 0);
        hr = displayInformation->add_OrientationChanged(Callback<ITypedEventHandler<DisplayInformation *, IInspectable *>>(this, &Surface::onOrientationChanged).Get(),
                                                        &mOrientationToken);
        ASSERT(SUCCEEDED(hr));
#   endif

        ComPtr<ICoreWindow> coreWindow;
        hr = mWindow->QueryInterface(coreWindow.GetAddressOf());
        ASSERT(SUCCEEDED(hr));

        Rect rect;
        hr = coreWindow->get_Bounds(&rect);
        ASSERT(SUCCEEDED(hr));
        mWidth = rect.Width * mScaleFactor;
        mHeight = rect.Height * mScaleFactor;
        hr = coreWindow->add_SizeChanged(Callback<ITypedEventHandler<CoreWindow *, WindowSizeChangedEventArgs *>>(this, &Surface::onSizeChanged).Get(),
                                         &mSizeToken);
        ASSERT(SUCCEEDED(hr));
    }
#endif

    if (!resetSwapChain())
      return false;

    return true;
}

void Surface::release()
{
    delete mSwapChain;
    mSwapChain = NULL;

    if (mTexture)
    {
        mTexture->releaseTexImage();
        mTexture = NULL;
    }

#if defined(ANGLE_PLATFORM_WINRT)
    if (mWindow)
        mWindow->Release();
#endif
}

bool Surface::resetSwapChain()
{
    ASSERT(!mSwapChain);

    int width;
    int height;

#if !defined(ANGLE_PLATFORM_WINRT)
    if (!mFixedSize)
    {
        RECT windowRect;
        if (!GetClientRect(getWindowHandle(), &windowRect))
        {
            ASSERT(false);

            ERR("Could not retrieve the window dimensions");
            return error(EGL_BAD_SURFACE, false);
        }

        width = windowRect.right - windowRect.left;
        height = windowRect.bottom - windowRect.top;
    }
    else
#endif
    {
        // non-window surface - size is determined at creation
        width = mWidth;
        height = mHeight;
    }

    mSwapChain = mRenderer->createSwapChain(mWindow, mShareHandle,
                                            mConfig->mRenderTargetFormat,
                                            mConfig->mDepthStencilFormat);
    if (!mSwapChain)
    {
        return error(EGL_BAD_ALLOC, false);
    }

    if (!resetSwapChain(width, height))
    {
        delete mSwapChain;
        mSwapChain = NULL;
        return false;
    }

    return true;
}

bool Surface::resizeSwapChain(int backbufferWidth, int backbufferHeight)
{
    ASSERT(backbufferWidth >= 0 && backbufferHeight >= 0);
    ASSERT(mSwapChain);

    EGLint status = mSwapChain->resize(std::max(1, backbufferWidth), std::max(1, backbufferHeight));

    if (status == EGL_CONTEXT_LOST)
    {
        mDisplay->notifyDeviceLost();
        return false;
    }
    else if (status != EGL_SUCCESS)
    {
        return error(status, false);
    }

    mWidth = backbufferWidth;
    mHeight = backbufferHeight;

    return true;
}

bool Surface::resetSwapChain(int backbufferWidth, int backbufferHeight)
{
    ASSERT(backbufferWidth >= 0 && backbufferHeight >= 0);
    ASSERT(mSwapChain);

    EGLint status = mSwapChain->reset(std::max(1, backbufferWidth), std::max(1, backbufferHeight), mSwapInterval);

    if (status == EGL_CONTEXT_LOST)
    {
        mRenderer->notifyDeviceLost();
        return false;
    }
    else if (status != EGL_SUCCESS)
    {
        return error(status, false);
    }

    mWidth = backbufferWidth;
    mHeight = backbufferHeight;
    mSwapIntervalDirty = false;

    return true;
}

bool Surface::swapRect(EGLint x, EGLint y, EGLint width, EGLint height)
{
    if (!mSwapChain)
    {
        return true;
    }

    if (x + width > mWidth)
    {
        width = mWidth - x;
    }

    if (y + height > mHeight)
    {
        height = mHeight - y;
    }

    if (width == 0 || height == 0)
    {
        return true;
    }

    EGLint status = mSwapChain->swapRect(x, y, width, height, mSwapFlags);

    if (status == EGL_CONTEXT_LOST)
    {
        mRenderer->notifyDeviceLost();
        return false;
    }
    else if (status != EGL_SUCCESS)
    {
        return error(status, false);
    }

    checkForOutOfDateSwapChain();

    return true;
}

EGLNativeWindowType Surface::getWindowHandle()
{
    return mWindow;
}


#define kSurfaceProperty _TEXT("Egl::SurfaceOwner")
#define kParentWndProc _TEXT("Egl::SurfaceParentWndProc")

#if !defined(ANGLE_PLATFORM_WINRT)
static LRESULT CALLBACK SurfaceWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
  if (message == WM_SIZE)
  {
      Surface* surf = reinterpret_cast<Surface*>(GetProp(hwnd, kSurfaceProperty));
      if(surf)
      {
          surf->checkForOutOfDateSwapChain();
      }
  }
  WNDPROC prevWndFunc = reinterpret_cast<WNDPROC >(GetProp(hwnd, kParentWndProc));
  return CallWindowProc(prevWndFunc, hwnd, message, wparam, lparam);
}
#endif

void Surface::subclassWindow()
{
#if !defined(ANGLE_PLATFORM_WINRT)
    if (!mWindow)
    {
        return;
    }

    DWORD processId;
    DWORD threadId = GetWindowThreadProcessId(mWindow, &processId);
    if (processId != GetCurrentProcessId() || threadId != GetCurrentThreadId())
    {
        return;
    }

    SetLastError(0);
    LONG_PTR oldWndProc = SetWindowLongPtr(mWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SurfaceWindowProc));
    if(oldWndProc == 0 && GetLastError() != ERROR_SUCCESS)
    {
        mWindowSubclassed = false;
        return;
    }

    SetProp(mWindow, kSurfaceProperty, reinterpret_cast<HANDLE>(this));
    SetProp(mWindow, kParentWndProc, reinterpret_cast<HANDLE>(oldWndProc));
    mWindowSubclassed = true;
#else
    mWindowSubclassed = false;
#endif
}

void Surface::unsubclassWindow()
{
#if !defined(ANGLE_PLATFORM_WINRT)
    if(!mWindowSubclassed)
    {
        return;
    }

    // un-subclass
    LONG_PTR parentWndFunc = reinterpret_cast<LONG_PTR>(GetProp(mWindow, kParentWndProc));

    // Check the windowproc is still SurfaceWindowProc.
    // If this assert fails, then it is likely the application has subclassed the
    // hwnd as well and did not unsubclass before destroying its EGL context. The
    // application should be modified to either subclass before initializing the
    // EGL context, or to unsubclass before destroying the EGL context.
    if(parentWndFunc)
    {
        LONG_PTR prevWndFunc = SetWindowLongPtr(mWindow, GWLP_WNDPROC, parentWndFunc);
        UNUSED_ASSERTION_VARIABLE(prevWndFunc);
        ASSERT(prevWndFunc == reinterpret_cast<LONG_PTR>(SurfaceWindowProc));
    }

    RemoveProp(mWindow, kSurfaceProperty);
    RemoveProp(mWindow, kParentWndProc);
    mWindowSubclassed = false;
#endif
}

bool Surface::checkForOutOfDateSwapChain()
{
    int clientWidth = getWidth();
    int clientHeight = getHeight();
    bool sizeDirty = false;
#if !defined(ANGLE_PLATFORM_WINRT)
    if (!mFixedSize && !IsIconic(getWindowHandle()))
    {
        RECT client;
        // The window is automatically resized to 150x22 when it's minimized, but the swapchain shouldn't be resized
        // because that's not a useful size to render to.
        if (!GetClientRect(getWindowHandle(), &client))
        {
            ASSERT(false);
            return false;
        }

        // Grow the buffer now, if the window has grown. We need to grow now to avoid losing information.
        clientWidth = client.right - client.left;
        clientHeight = client.bottom - client.top;
        sizeDirty = clientWidth != getWidth() || clientHeight != getHeight();
    }
#endif

    bool wasDirty = (mSwapIntervalDirty || sizeDirty);

    if (mSwapIntervalDirty)
    {
        resetSwapChain(clientWidth, clientHeight);
    }
    else if (sizeDirty)
    {
        resizeSwapChain(clientWidth, clientHeight);
    }

    if (wasDirty)
    {
        if (static_cast<egl::Surface*>(getCurrentDrawSurface()) == this)
        {
            glMakeCurrent(glGetCurrentContext(), static_cast<egl::Display*>(getCurrentDisplay()), this);
        }

        return true;
    }

    return false;
}

bool Surface::swap()
{
    return swapRect(0, 0, mWidth, mHeight);
}

bool Surface::postSubBuffer(EGLint x, EGLint y, EGLint width, EGLint height)
{
    if (!mPostSubBufferSupported)
    {
        // Spec is not clear about how this should be handled.
        return true;
    }

    return swapRect(x, y, width, height);
}

EGLint Surface::isPostSubBufferSupported() const
{
    return mPostSubBufferSupported;
}

rx::SwapChain *Surface::getSwapChain() const
{
    return mSwapChain;
}

void Surface::setSwapInterval(EGLint interval)
{
    if (mSwapInterval == interval)
    {
        return;
    }

    mSwapInterval = interval;
    mSwapInterval = std::max(mSwapInterval, mRenderer->getMinSwapInterval());
    mSwapInterval = std::min(mSwapInterval, mRenderer->getMaxSwapInterval());

    mSwapIntervalDirty = true;
}

EGLint Surface::getConfigID() const
{
    return mConfig->mConfigID;
}

EGLint Surface::getWidth() const
{
    return mWidth;
}

EGLint Surface::getHeight() const
{
    return mHeight;
}

EGLint Surface::getPixelAspectRatio() const
{
    return mPixelAspectRatio;
}

EGLenum Surface::getRenderBuffer() const
{
    return mRenderBuffer;
}

EGLenum Surface::getSwapBehavior() const
{
    return mSwapBehavior;
}

EGLenum Surface::getTextureFormat() const
{
    return mTextureFormat;
}

EGLenum Surface::getTextureTarget() const
{
    return mTextureTarget;
}

void Surface::setBoundTexture(gl::Texture2D *texture)
{
    mTexture = texture;
}

gl::Texture2D *Surface::getBoundTexture() const
{
    return mTexture;
}

EGLint Surface::isFixedSize() const
{
    return mFixedSize;
}

EGLenum Surface::getFormat() const
{
    return mConfig->mRenderTargetFormat;
}

#if defined(ANGLE_PLATFORM_WINRT)

HRESULT Surface::onSizeChanged(ICoreWindow *, IWindowSizeChangedEventArgs *args)
{
    HRESULT hr;
    Size size;
    hr = args->get_Size(&size);
    ASSERT(SUCCEEDED(hr));

    resizeSwapChain(std::floor(size.Width * mScaleFactor + 0.5),
                    std::floor(size.Height * mScaleFactor + 0.5));

    if (static_cast<egl::Surface*>(getCurrentDrawSurface()) == this)
    {
        glMakeCurrent(glGetCurrentContext(), static_cast<egl::Display*>(getCurrentDisplay()), this);
    }

    return S_OK;
}

HRESULT Surface::onDpiChanged(IDisplayInformation *displayInformation, IInspectable *)
{
    HRESULT hr;
#   if WINAPI_FAMILY==WINAPI_FAMILY_PHONE_APP
    ComPtr<IDisplayInformation2> displayInformation2;
    hr = displayInformation->QueryInterface(displayInformation2.GetAddressOf());
    ASSERT(SUCCEEDED(hr));

    hr = displayInformation2->get_RawPixelsPerViewPixel(&mScaleFactor);
    ASSERT(SUCCEEDED(hr));
#   else
    ResolutionScale resolutionScale;
    hr = displayInformation->get_ResolutionScale(&resolutionScale);
    ASSERT(SUCCEEDED(hr));

    mScaleFactor = double(resolutionScale) / 100.0;
#   endif
    return S_OK;
}

#   if WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
HRESULT Surface::onOrientationChanged(IDisplayInformation *displayInformation, IInspectable *)
{
    HRESULT hr;
    DisplayOrientations orientation;
    hr = displayInformation->get_CurrentOrientation(&orientation);
    ASSERT(SUCCEEDED(hr));
    switch (orientation) {
    default:
    case DisplayOrientations_Portrait:
        mSwapFlags = rx::SWAP_NORMAL;
        break;
    case DisplayOrientations_Landscape:
        mSwapFlags = rx::SWAP_ROTATE_90;
        break;
    case DisplayOrientations_LandscapeFlipped:
        mSwapFlags = rx::SWAP_ROTATE_270;
        break;
    case DisplayOrientations_PortraitFlipped:
        mSwapFlags = rx::SWAP_ROTATE_180;
        break;
    }
    return S_OK;
}
#   endif

#endif

}
