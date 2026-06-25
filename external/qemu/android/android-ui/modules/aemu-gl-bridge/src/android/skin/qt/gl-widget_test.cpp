// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/skin/qt/gl-widget.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QImage>

#include "android/opengles-overrides.h"
#include "host-common/opengles.h"
#include "OpenGLESDispatch/GLESv2Dispatch.h"
#include "OpenGLESDispatch/OpenGLDispatchLoader.h"
#include "android/emulation/control/AndroidAgentFactory.h"
#include "render-utils/virtio_gpu_ops.h"

using namespace gfxstream::host::gl;
using namespace android::emulation;

namespace {

static struct AndroidOpenglesFuncs sTestFuncs = {
    .getVirtioGpuOps = []() -> struct AndroidVirtioGpuOps* { return nullptr; },
    .getEGLDispatch = []() -> const void* { return (const void*)LazyLoadedEGLDispatch::get(); },
    .getGLESv2Dispatch = []() -> const void* { return (const void*)LazyLoadedGLESv2Dispatch::get(); },
};

class MockConsoleFactory : public AndroidConsoleFactory {
public:
    const QAndroidSensorsAgent* android_get_QAndroidSensorsAgent() const override {
        return nullptr;
    }
};

class GLWidgetTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char* argv[] = {(char*)"test"};
            new QApplication(argc, argv);
        }

        android_setOpenglesFuncs(&sTestFuncs);

        static MockConsoleFactory sFactory;
        // Inject agents, ignoring warnings about repeated injection.
        injectConsoleAgents(sFactory);
    }
};

class TestGLWidget : public GLWidget {
public:
    using GLWidget::GLWidget;

    bool initGL() override {
        mInitCalled = true;
        return true;
    }

    void repaintGL() override {
        mRepaintCalled = true;
        // Draw something simple: RED
        if (mGLES2) {
            mGLES2->glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
            mGLES2->glClear(0x00004000); // GL_COLOR_BUFFER_BIT
        }
    }

    const QImage& offscreenImage() const { return mOffscreenImage; }

    bool mInitCalled = false;
    bool mRepaintCalled = false;
};

TEST_F(GLWidgetTest, AlwaysUsePbufferInitialization) {
    fprintf(stderr, "Starting AlwaysUsePbufferInitialization test\n");
    auto widget = std::make_unique<TestGLWidget>(nullptr);
    widget->resize(100, 100);
    fprintf(stderr, "Calling widget->show()\n");
    widget->show();

    // Trigger initialization
    fprintf(stderr, "Calling widget->ensureInit()\n");
    bool success = widget->ensureInit();
    fprintf(stderr, "ensureInit returned %d\n", success);
    EXPECT_TRUE(success);
    EXPECT_TRUE(widget->mInitCalled);
}

TEST_F(GLWidgetTest, PbufferRendering) {
    fprintf(stderr, "Starting PbufferRendering test\n");
    auto widget = std::make_unique<TestGLWidget>(nullptr);
    widget->resize(100, 100);
    fprintf(stderr, "Calling widget->show()\n");
    widget->show();

    fprintf(stderr, "Calling widget->ensureInit()\n");
    bool success = widget->ensureInit();
    ASSERT_TRUE(success);

    fprintf(stderr, "Calling widget->renderFrame()\n");
    widget->renderFrame();
    fprintf(stderr, "renderFrame finished\n");
    EXPECT_TRUE(widget->mRepaintCalled);

    // Pbuffer mode should always be active now.
    EXPECT_FALSE(widget->offscreenImage().isNull());

    // Verify that the pixel in the middle is red.
    QRgb pixel = widget->offscreenImage().pixel(50, 50);
    fprintf(stderr, "Middle pixel color: %08x\n", pixel);

    // Red in QRgb (ARGB) is 0xffff0000.
    EXPECT_EQ(qRed(pixel), 255);
    EXPECT_EQ(qGreen(pixel), 0);
    EXPECT_EQ(qBlue(pixel), 0);
}

}  // namespace
