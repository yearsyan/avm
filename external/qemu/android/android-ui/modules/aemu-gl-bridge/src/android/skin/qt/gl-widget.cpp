// Copyright 2016 The Android Open Source Project
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/skin/qt/gl-widget.h"

#include "host-common/opengles.h"

#include <qloggingcategory.h>                   // for qCWarning
#include <qnamespace.h>                         // for WA_DontCreateNativeAn...
#include <QImage>
#include <QPainter>
#include <QRegion>                              // for QRegion
#include <QResizeEvent>                         // for QResizeEvent
#include <QSize>                                // for QSize
#include <QWindow>                              // for QWindow

#include "EGL/egl.h"                            // for EGL_FALSE, EGL_NONE
#include "EGL/eglplatform.h"                    // for EGLint, EGLNativeWind...
#include "GLES3/gl3.h"                          // for GL_TEXTURE_2D
#include "OpenGLESDispatch/EGLDispatch.h"       // for EGLDispatch
#include "OpenGLESDispatch/GLESv2Dispatch.h"    // for GLESv2Dispatch
#include "android/skin/qt/gl-canvas.h"          // for GLCanvas
#include "android/skin/qt/gl-texture-draw.h"    // for TextureDraw
#include "android/utils/debug.h"

using namespace gfxstream::host::gl;

class QPaintEvent;
class QResizeEvent;
class QScreen;
class QShowEvent;
class QWidget;

struct EGLState {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
};

// Helper function to get the nearest power-of-two.
// It's needed to create a GLCanvas with correct dimensions
// to allow generating mipmaps (GLES 2.0 doesn't support
// mipmaps for NPOT textures)
static int nearestPOT(int value) {
    return pow(2, ceil(log(value)/log(2)));
}

GLWidget::GLWidget(QWidget* parent) :
        QWidget(parent),
        mEGL((const EGLDispatch*)android_getEGLDispatch()),
        mGLES2((const GLESv2Dispatch*)android_getGLESv2Dispatch()),
        mEGLState(nullptr),
        mValid(false),
        mEnableAA(false) {
    setAutoFillBackground(true);
    // We use a Pbuffer + QPainter for all rendering to avoid platform-specific issues with native
    // window surfaces. Standard widget attributes are required for standard Qt painting.
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAttribute(Qt::WA_PaintOnScreen, false);
    setAttribute(Qt::WA_NoSystemBackground, false);
    setAttribute(Qt::WA_NativeWindow, false);
}

QPaintEngine* GLWidget::paintEngine() const {
    return QWidget::paintEngine();
}

void GLWidget::handleScreenChange(QScreen*) {
    // Destroy the context, forcing its re-creation on
    // next paint event.
    destroyContext();
    update();
}

bool GLWidget::ensureInit() {
    // If an error occured when loading the EGL/GLESv2 libraries, return false.
    if (!mEGL || !mGLES2) {
        return false;
    }

    // If already initialized, return mValid to indicate if an error occured.
    if (mEGLState) {
        return mValid;
    }

    // On macOS, creating a surface for a hidden widget often succeeds but
    // never actually renders to the screen. Force visibility check here.
    if (!isVisible() || visibleRegion().isNull()) {
        return false;
    }

    dwarning("GLWidget::ensureInit: starting initialization for winId %p", (void*)winId());
    mEGLState = new EGLState();
    mValid = false;

    mEGLState->display = mEGL->eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mEGLState->display == EGL_NO_DISPLAY) {
        derror("Failed to get EGL display: EGL error %d",
                 mEGL->eglGetError());
        destroyContext();
        return false;
    }

    EGLint egl_maj, egl_min;
    EGLConfig egl_config;

    // Try to initialize EGL display.
    // Initializing an already-initialized display is OK.
    if (mEGL->eglInitialize(mEGLState->display, &egl_maj, &egl_min) ==
        EGL_FALSE) {
        dwarning("Failed to initialize EGL display: EGL error %d",
                 mEGL->eglGetError());
        destroyContext();
        return false;
    }

    // Get an EGL config for pbuffer surface.
    const EGLint config_attribs[] = {EGL_RED_SIZE, 8,
                                     EGL_GREEN_SIZE, 8,
                                     EGL_BLUE_SIZE, 8,
                                     EGL_ALPHA_SIZE, 8,
                                     EGL_DEPTH_SIZE, 24,
                                     EGL_STENCIL_SIZE, 8,
                                     EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_NONE};

    EGLint num_configs_returned = 0;
    if (mEGL->eglChooseConfig(mEGLState->display, config_attribs, &egl_config, 1, &num_configs_returned) == EGL_FALSE || num_configs_returned < 1) {
        dwarning("Failed to choose EGL config for Pbuffer: EGL error %d", mEGL->eglGetError());
        destroyContext();
        return false;
    }

    // Create a context.
    EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    AndroidVirtioGpuOps* ops = android_getVirtioGpuOps();
    mEGLState->context = mEGL->eglCreateContext(
        mEGLState->display,
        egl_config,
        EGL_NO_CONTEXT,
        context_attribs);

    if (mEGLState->context == EGL_NO_CONTEXT) {
        dwarning("Failed to create EGL context %d", mEGL->eglGetError());
        destroyContext();
        return false;
    }

    // Finally, create a pbuffer surface associated with this widget.
    const EGLint pbuffer_attribs[] = {
        EGL_WIDTH, realPixelsWidth(),
        EGL_HEIGHT, realPixelsHeight(),
        EGL_NONE
    };
    mEGLState->surface = mEGL->eglCreatePbufferSurface(
            mEGLState->display, egl_config, pbuffer_attribs);

    if (mEGLState->surface == EGL_NO_SURFACE) {
        dwarning("Failed to create an EGL pbuffer surface %d", mEGL->eglGetError());
        destroyContext();
        return false;
    }

    if (!makeContextCurrent()) {
        destroyContext();
        return false;
    }
    mCanvas.reset(new GLCanvas(
        nearestPOT(realPixelsWidth()),
        nearestPOT(realPixelsHeight()),
        mGLES2));
    mTextureDraw.reset(new TextureDraw(mGLES2));
    mValid = initGL();
    if (!mValid) {
        destroyContext();
    }
    return mValid;
}

bool GLWidget::makeContextCurrent() {
    if (mEGLState && mEGLState->surface != EGL_NO_SURFACE) {
        return mEGL->eglMakeCurrent(mEGLState->display, mEGLState->surface,
                                 mEGLState->surface, mEGLState->context) == EGL_TRUE;

    } else {
        return false;
    }
}

void GLWidget::renderFrame() {
    // On macOS, the window might briefly be detached during screen moves.
    // Initialization will fail if we don't have a valid window or screen.
    if (!isVisible() || visibleRegion().isNull() || !window()->windowHandle() ||
        !window()->windowHandle()->screen()) {
        return;
    }

    if (!ensureInit()) {
        return;
    }
    if (!readyForRendering()) {
        return;
    }
    if (!makeContextCurrent()) {
        return;
    }

    // Render 3D scene to texture.
    if (mEnableAA) {
        mCanvas->bind();
        repaintGL();
        mCanvas->unbind();
    } else {
        mGLES2->glViewport(0, 0, realPixelsWidth(), realPixelsHeight());
        repaintGL();
    }

    if (mEnableAA) {
        mGLES2->glBindTexture(GL_TEXTURE_2D, mCanvas->texture());
        mGLES2->glGenerateMipmap(GL_TEXTURE_2D);
        mTextureDraw->draw(mCanvas->texture(),
                           realPixelsWidth(),
                           realPixelsHeight());
    }

    int w = realPixelsWidth();
    int h = realPixelsHeight();
    if (mOffscreenImage.size() != QSize(w, h)) {
        mOffscreenImage = QImage(w, h, QImage::Format_RGBA8888);
        mOffscreenImage.setDevicePixelRatio(devicePixelRatioF());
    }
    mGLES2->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, mOffscreenImage.bits());
    // OpenGL is bottom-up, Qt is top-down.
    // NOTE: Mirroring here is correct, but we must not call update() here
    // to avoid infinite recursion when called from paintEvent.
    mOffscreenImage = mOffscreenImage.mirrored(false, true);
}

void GLWidget::paintEvent(QPaintEvent*) {
    // Force a fresh render to the Pbuffer and read-back.
    renderFrame();
    if (!mOffscreenImage.isNull()) {
        QPainter painter(this);
        painter.drawImage(rect(), mOffscreenImage);
    }
}

void GLWidget::showEvent(QShowEvent*) {
    // When the widget first becomes visible, we ask it to render a frame,
    // which will force the necessary initialization.
    // It is important to make sure that the widget is actually
    // visible on screen (at least for OS X) before doing any
    // initialization at all.
    // However, show events may be delivered when the widget
    // isn't visible yet, so we need an additional check.
    if (isVisible() && !visibleRegion().isNull() && mFirstShow) {
        mFirstShow = false;
        connect(window()->windowHandle(), SIGNAL(screenChanged(QScreen*)),
                this, SLOT(handleScreenChange(QScreen*)));
        renderFrame();
        update();
    }
}

void GLWidget::resizeEvent(QResizeEvent* e) {
    if (mEGLState && mValid) {
        // Re-create the Pbuffer surface with new size.
        EGLConfig egl_config;
        EGLint num_config;
        const EGLint config_attribs[] = {EGL_RED_SIZE, 8,
                                         EGL_GREEN_SIZE, 8,
                                         EGL_BLUE_SIZE, 8,
                                         EGL_ALPHA_SIZE, 8,
                                         EGL_DEPTH_SIZE, 24,
                                         EGL_STENCIL_SIZE, 8,
                                         EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                         EGL_NONE};
        mEGL->eglChooseConfig(mEGLState->display, config_attribs, &egl_config, 1, &num_config);

        mEGL->eglDestroySurface(mEGLState->display, mEGLState->surface);

        const EGLint pbuffer_attribs[] = {
            EGL_WIDTH, realPixelsWidth(),
            EGL_HEIGHT, realPixelsHeight(),
            EGL_NONE
        };
        mEGLState->surface = mEGL->eglCreatePbufferSurface(
                mEGLState->display, egl_config, pbuffer_attribs);

        if (makeContextCurrent()) {
            // Re-create the framebuffer with new size.
            mCanvas.reset(new GLCanvas(
                nearestPOT(e->size().width() * devicePixelRatioF()),
                nearestPOT(e->size().height() * devicePixelRatioF()),
                mGLES2));
            resizeGL(e->size().width() * devicePixelRatioF(),
                     e->size().height() * devicePixelRatioF());
            update();
        }
    }
}

GLWidget::~GLWidget() {
    destroyContext();
}

void GLWidget::destroyContext() {
    if (mGLES2 && mEGL && mEGLState) {
        // Destroy canvas and texturedraw state
        // within this context.
        makeContextCurrent();
        mCanvas.reset(nullptr);
        mTextureDraw.reset(nullptr);

        // Make sure the context isn't active before destroying it.
        mEGL->eglMakeCurrent(mEGLState->display, 0, 0, 0);

        // Reset EGL state and set mEGLState to null, which will force
        // re-initialization when attempting to render the next frame.
        if (mEGLState->surface != EGL_NO_SURFACE) {
            mEGL->eglDestroySurface(mEGLState->display, mEGLState->surface);
        }
        if (mEGLState->context != EGL_NO_CONTEXT) {
            mEGL->eglDestroyContext(mEGLState->display, mEGLState->context);
        }
        delete mEGLState;
        mEGLState = nullptr;
        mOffscreenImage = QImage();
    }
}
