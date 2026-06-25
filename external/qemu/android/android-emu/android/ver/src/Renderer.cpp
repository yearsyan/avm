/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Renderer.h"
#include "ScenesManager.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <libyuv.h>

#include "aemu/base/files/PathUtils.h"
#include "aemu/base/synchronization/MessageChannel.h"
#include "aemu/base/threads/WorkerThread.h"
#include "android/base/system/System.h"

#include "OpenGLESDispatch/EGLDispatch.h"
#include "OpenGLESDispatch/GLESv2Dispatch.h"

#include "RenderTarget.h"
#include "TextureUtils.h"

using namespace android::base;

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) dprint(__VA_ARGS__)

// The T(...) macro is used to dump extra verbose information.
#define T_ACTIVE 0

#if T_ACTIVE
#define T(...) VERBOSE_PRINT(virtualscene, __VA_ARGS__)
#else
#define T(...) ((void)0)
#endif

struct ScopeTimer {
    ScopeTimer(const char* name) {
#if T_ACTIVE
        mFuncName = name;
        mStartTime = get_uptime_ms();
#endif
    }

#if T_ACTIVE
    ~ScopeTimer() {
        const uint64_t scopeTime = get_uptime_ms() - mStartTime;
        dinfo("ScopeTimer: '%s' took %llu ms", mFuncName, scopeTime);
    }
    const char* mFuncName;
    int64_t mStartTime;
#endif
};

static constexpr int kSuperSampleMultiple = 2;

// TODO(virtualscene-perf): Implement proper offscreen rendering and remove
// backing framebuffer dimensions from the renderer.
static constexpr int kRendererDefaultFramebufferWidth = 1024;
static constexpr int kRendererDefaultFramebufferHeight = 1024;

static constexpr char kTexturedVertexShader[] = R"(
attribute vec3 in_position;
attribute vec2 in_uv;

uniform mat4 u_modelViewProj;

varying vec2 uv;

void main() {
    uv = in_uv;
    gl_Position = u_modelViewProj * vec4(in_position, 1.0);
}
)";

static constexpr char kTexturedFragmentShader[] = R"(
precision mediump float;
varying vec2 uv;

uniform sampler2D tex_sampler;

void main() {
  gl_FragColor = texture2D(tex_sampler, uv);
}
)";

static constexpr char kScreenSpaceVertexShader[] = R"(
attribute vec3 in_position;
attribute vec2 in_uv;

varying vec2 uv;

void main() {
    uv = in_uv;
    gl_Position = vec4(in_position, 1.0);
}
)";

static constexpr char kBlitFragmentShader[] = R"(
precision mediump float;
uniform vec2 resolution;

uniform sampler2D tex_sampler;

void main() {
  gl_FragColor = texture2D(tex_sampler, gl_FragCoord.xy * resolution);
}
)";

static constexpr char kCheckerboardFragmentShader[] = R"(
precision mediump float;
varying vec2 uv;
uniform float u_time;

#define CHECKERBOARD_SQUARE_WIDTH 16.0 // Number of squares across the board.
#define MOVING_SQUARE_SIZE 2.5 // Width of moving square in checkerboard squares.
#define VISIBLE_HEIGHT 9.0 // Height where we want the moving square to bounce.
                           // in checkerboard squares.
#define COLOR_BLACK vec3(0.0,0.0,0.0)
#define COLOR_GREY vec3(0.5,0.5,0.5)
#define COLOR_RED vec3(0.82422,0.03125,0.0)
#define COLOR_GREEN vec3(0.01172,0.65625,0.0)
#define MOVING_SQUARE_VELOCITY vec2(1.5,1.5) // Velocity of the moving
                                             // square expressed in
                                             // checkerboard squares
                                             // per second.
#define CHECKERBOARD_VELOCITY vec2(1.0,1.0/3.0) // Velocity of the checkerboard
                                                // expressed in checkerboard
                                                // squares per second.
#define COLOR_CHANGE_TIME 5.0 // Time between red/green color changes.

void main() {
  vec2 staticBoardSpacePosition =
        vec2(uv.x * CHECKERBOARD_SQUARE_WIDTH, uv.y * CHECKERBOARD_SQUARE_WIDTH);
  vec2 movingBoardSpacePosition =
        staticBoardSpacePosition +
        u_time * CHECKERBOARD_VELOCITY;
  vec3 checkerboardColor = COLOR_BLACK + (mod(
        floor(mod(movingBoardSpacePosition.x, 2.0)) +
        floor(mod(movingBoardSpacePosition.y, 2.0)), 2.0)) *
        (COLOR_GREY - COLOR_BLACK);
  vec2 totalSquareMovement = MOVING_SQUARE_VELOCITY * u_time;
  float squareXTravel = CHECKERBOARD_SQUARE_WIDTH - MOVING_SQUARE_SIZE;
  float squareYTravel = VISIBLE_HEIGHT - MOVING_SQUARE_SIZE;
  vec2 squarePosition = vec2(
        mod(totalSquareMovement.x, squareXTravel * 2.0) > squareXTravel ?
        squareXTravel - mod(totalSquareMovement.x, squareXTravel) :
        mod(totalSquareMovement.x, squareXTravel),
        ((CHECKERBOARD_SQUARE_WIDTH - VISIBLE_HEIGHT) / 2.0) +
        (mod(totalSquareMovement.y, squareYTravel * 2.0) > squareYTravel ?
        squareYTravel - mod(totalSquareMovement.y, squareYTravel) :
        mod(totalSquareMovement.y, squareYTravel)));
  vec3 squareColor = COLOR_GREEN +
        floor(mod(u_time, COLOR_CHANGE_TIME * 2.0) / COLOR_CHANGE_TIME) *
        (COLOR_RED - COLOR_GREEN);
  vec2 posFromSquareOrigin = staticBoardSpacePosition - squarePosition;

  if (posFromSquareOrigin.x >= 0.0 &&
      posFromSquareOrigin.y >= 0.0 &&
      posFromSquareOrigin.x < MOVING_SQUARE_SIZE &&
      posFromSquareOrigin.y < MOVING_SQUARE_SIZE) {
    gl_FragColor = vec4(squareColor, 1.0);
  } else {
    gl_FragColor = vec4(checkerboardColor, 1.0);
  }
}
)";

static constexpr char kFxaaFragmentShader[] = R"(
precision mediump float;
uniform sampler2D tex_sampler;
uniform vec2 resolution;

#define FXAA_MUL  (1.0/8.0)
#define FXAA_MIN  (1.0/32.0)
#define FXAA_SPAN 8.0

void main() {
  vec3 rgbNW = texture2D(tex_sampler, (gl_FragCoord.xy + vec2(-1.0, -1.0)) * resolution).xyz;
  vec3 rgbNE = texture2D(tex_sampler, (gl_FragCoord.xy + vec2( 1.0, -1.0)) * resolution).xyz;
  vec3 rgbM = texture2D(tex_sampler, gl_FragCoord.xy * resolution).xyz;
  vec3 rgbSW = texture2D(tex_sampler, (gl_FragCoord.xy + vec2(-1.0,  1.0)) * resolution).xyz;
  vec3 rgbSE = texture2D(tex_sampler, (gl_FragCoord.xy + vec2( 1.0,  1.0)) * resolution).xyz;

  vec3 luma = vec3(0.299, 0.587, 0.114);
  float lumaNW = dot(rgbNW, luma);
  float lumaNE = dot(rgbNE, luma);
  float lumaM = dot(rgbM, luma);
  float lumaSW = dot(rgbSW, luma);
  float lumaSE = dot(rgbSE, luma);

  vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)),
                  ((lumaNW + lumaSW) - (lumaNE + lumaSE)));

  float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_MUL), FXAA_MIN);

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

  dir = min(vec2( FXAA_SPAN, FXAA_SPAN), max(vec2(-FXAA_SPAN, -FXAA_SPAN), dir * rcpDirMin)) * resolution;

  vec4 rgbA = 0.5 * (texture2D(tex_sampler, gl_FragCoord.xy * resolution + dir * (1.0 / 3.0 - 0.5)) +
                     texture2D(tex_sampler, gl_FragCoord.xy * resolution + dir * (2.0 / 3.0 - 0.5)));
  vec4 rgbB = rgbA * 0.5 + 0.25 * (texture2D(tex_sampler, gl_FragCoord.xy * resolution + dir * (0.0 / 3.0 - 0.5)) +
                                   texture2D(tex_sampler, gl_FragCoord.xy * resolution + dir * (3.0 / 3.0 - 0.5)));
  float lumaB = dot(rgbB.xyz, luma);

  if ((lumaB < lumaMin) || (lumaB > lumaMax)) {
      gl_FragColor = rgbA;
  } else {
      gl_FragColor = rgbB;
  }
}
)";

static constexpr android::ver::VertexPositionUV kScreenQuadVerts[] = {
        {glm::vec3(-1.f, -1.f, 0.f), glm::vec2(0.f, 0.f)},
        {glm::vec3(3.f, -1.f, 0.f), glm::vec2(2.f, 0.f)},
        {glm::vec3(-1.f, 3.f, 0.f), glm::vec2(0.f, 2.f)},
};

static constexpr GLuint kScreenQuadIndices[] = {
        0,
        1,
        2,
};

namespace android {
namespace ver {

struct MaterialData {
    GLuint program = 0;

    GLint positionLocation = -1;
    GLint uvLocation = -1;
    GLint texSamplerLocation = -1;
    GLint mvpLocation = -1;
    GLint resolutionLocation = -1;
    GLint timeLocation = -1;
};

struct MeshData {
    VertexInfo mVertexInfo;
    GLuint mVertexBuffer = 0;
    GLuint mIndexBuffer = 0;
    size_t mIndexCount = 0;
};

enum class TextureState { Placeholder, Loaded };

struct TextureData {
    size_t mRefCount = 0;
    TextureState mState = TextureState::Loaded;
    GLuint mTextureId = 0;
    std::string mFilename;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
};

struct RendererHeldResources {
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Texture> textures;
};

enum class LoaderCommandType { Shutdown, LoadTexture };

struct LoaderCommand {
    LoaderCommandType mType;
    int mHandle = -1;

    LoaderCommand(LoaderCommandType type, int handle)
        : mType(type), mHandle(handle) {}
};

using RenderableParameterCallback =
        std::function<void(const MaterialData& material)>;

static uint64_t durationUsToMs(uint64_t startUs, uint64_t endUs) {
    assert(startUs < endUs);
    const uint64_t durationUs = (endUs - startUs);
    return durationUs / 1000;
}

// Defines an RAII object to automatically unset the EGL context when the scope
// exits.
class ScopedEglContext : public RendererContext {
public:
    ScopedEglContext() {
        mEglDispatch = nullptr;
        mEglDisplay = EGL_NO_DISPLAY;
    }

    // Unset the EGL context on scope exit.
    // |eglDispatch| - If non-null, the EGLDispatch object will be used to call
    //                 eglMakeCurrent with EGL_NO_CONTEXT. If this is non-null,
    //                 |eglDisplay| must also be EGL_NO_DISPLAY.
    // |eglDisplay| - EGLDisplay handle, if |eglDispatch| is null, this must be
    //                set to EGL_NO_DISPLAY.
    ScopedEglContext(const gfxstream::host::gl::EGLDispatch* eglDispatch,
                     const EGLDisplay eglDisplay)
        : mEglDispatch(eglDispatch), mEglDisplay(eglDisplay) {
        if ((eglDispatch && eglDisplay != EGL_NO_DISPLAY) ||
            (!eglDispatch && eglDisplay == EGL_NO_DISPLAY)) {
            // valid
        } else {
            derror("Invalid ScopedEglContext");
        }
    }

    ScopedEglContext(ScopedEglContext&& other)
        : mEglDispatch(other.mEglDispatch), mEglDisplay(other.mEglDisplay) {
        other.mEglDispatch = nullptr;
        other.mEglDisplay = nullptr;
    }

    ~ScopedEglContext() {
        if (mEglDispatch) {
            const EGLBoolean eglResult = mEglDispatch->eglMakeCurrent(
                    mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                    EGL_NO_CONTEXT);
            if (eglResult == EGL_FALSE) {
                LOG(WARNING) << "Could not unset eglMakeCurrent error "
                             << mEglDispatch->eglGetError();
            }
        }
    }

    bool isValid() const override { return mEglDispatch; }

private:
    const gfxstream::host::gl::EGLDispatch* mEglDispatch;
    EGLDisplay mEglDisplay;
};

/*******************************************************************************
 *                     ImageScaler routines
 ******************************************************************************/

ImageScaler::ImageScaler(int width, int height, uint8_t* buffer)
    : mFrameWidth(width), mFrameHeight(height), mOutputRgba(buffer) {}

bool ImageScaler::updateImage(int inputWidth,
                              int inputHeight,
                              const uint8_t* inputRgba,
                              ScaleMode mode) {
    // Basic validation
    if (!inputRgba || !mOutputRgba || inputWidth <= 0 || inputHeight <= 0 ||
        mFrameWidth <= 0 || mFrameHeight <= 0) {
        derror("%s: Invalid input dimensions or pointers", __func__);
        return false;
    }

    int result = -1;
    switch (mode) {
        case ScaleMode::AspectFitLetterbox:
            result = aspectFitLetterbox(inputWidth, inputHeight, inputRgba);
            break;
        case ScaleMode::AspectFitZoom:
            result = aspectFitZoom(inputWidth, inputHeight, inputRgba);
            break;
        case ScaleMode::ScaleToFill:
            result = scaleToFill(inputWidth, inputHeight, inputRgba);
            break;
        default:
            derror("%s: Unknown ScaleMode encountered", __func__);
            return false;
    }

    if (result != 0) {
        derror("%s: libyuv::ARGBScale failed! error: %d", __func__, result);
        return false;
    }

    return true;
}

int ImageScaler::aspectFitLetterbox(int inputWidth,
                                    int inputHeight,
                                    const uint8_t* inputRgba) {
    float scaleX = static_cast<float>(mFrameWidth) / inputWidth;
    float scaleY = static_cast<float>(mFrameHeight) / inputHeight;

    // Use the smaller scale factor to ensure the whole image fits inside
    float scale = std::min(scaleX, scaleY);

    int targetWidth = static_cast<int>(inputWidth * scale);
    int targetHeight = static_cast<int>(inputHeight * scale);

    // Calculate offsets to center the image
    int offsetX = (mFrameWidth - targetWidth) / 2;
    int offsetY = (mFrameHeight - targetHeight) / 2;

    // Clear the entire output buffer to black (0) to handle the letterboxing
    // 4 bytes per pixel for RGBA
    std::memset(mOutputRgba, 0, mFrameWidth * mFrameHeight * 4);

    int inputStride = inputWidth * 4;
    int outputStride = mFrameWidth * 4;

    // Offset the destination pointer
    uint8_t* dstPtr = mOutputRgba + (offsetY * outputStride) + (offsetX * 4);
    return libyuv::ARGBScale(inputRgba, inputStride, inputWidth, inputHeight,
                             dstPtr, outputStride, targetWidth, targetHeight,
                             libyuv::kFilterBilinear);
}

int ImageScaler::aspectFitZoom(int inputWidth,
                               int inputHeight,
                               const uint8_t* inputRgba) {
    float scaleX = static_cast<float>(mFrameWidth) / inputWidth;
    float scaleY = static_cast<float>(mFrameHeight) / inputHeight;

    // Use the larger scale factor so the image fills the entire area (cropping
    // excess)
    float scale = std::max(scaleX, scaleY);

    // Calculate the crop window in the input image
    int srcCropWidth = static_cast<int>(mFrameWidth / scale);
    int srcCropHeight = static_cast<int>(mFrameHeight / scale);

    // Calculate offsets to center the crop window
    int srcOffsetX = (inputWidth - srcCropWidth) / 2;
    int srcOffsetY = (inputHeight - srcCropHeight) / 2;

    int inputStride = inputWidth * 4;
    int outputStride = mFrameWidth * 4;

    // Offset the source pointer
    const uint8_t* srcPtr =
            inputRgba + (srcOffsetY * inputStride) + (srcOffsetX * 4);
    return libyuv::ARGBScale(srcPtr, inputStride, srcCropWidth, srcCropHeight,
                             mOutputRgba, outputStride, mFrameWidth,
                             mFrameHeight, libyuv::kFilterBilinear);
}

int ImageScaler::scaleToFill(int inputWidth,
                             int inputHeight,
                             const uint8_t* inputRgba) {
    int inputStride = inputWidth * 4;
    int outputStride = mFrameWidth * 4;

    // Simply map full source to full destination
    return libyuv::ARGBScale(inputRgba, inputStride, inputWidth, inputHeight,
                             mOutputRgba, outputStride, mFrameWidth,
                             mFrameHeight, libyuv::kFilterBilinear);
}

/*******************************************************************************
 *                     RendererView routines
 ******************************************************************************/

void RendererView::updateTarget(VerImageFormat format,
                                int frameWidth,
                                int frameHeight) {
    std::lock_guard lock(mLock);
    if (mFormat == format && mFrameWidth == frameWidth &&
        mFrameHeight == frameHeight) {
        // Setting the same values should not invalidate the cache
        return;
    }

    mFormat = format;
    mFrameWidth = frameWidth;
    mFrameHeight = frameHeight;
    mCache.invalidate();
}

void RendererView::updateViewProjection(const glm::mat4& viewProj) {
    std::lock_guard lock(mLock);
    if (mViewProjection == viewProj) {
        // Setting the same values should not invalidate the cache
        return;
    }

    mViewProjection = viewProj;
    mCache.invalidate();
}

void RendererView::setBlurFactor(float factor) {
    std::lock_guard lock(mLock);
    if (mBlurFactor == factor) {
        // Setting the same value should not invalidate the cache
        return;
    }

    mBlurFactor = factor;
    mCache.invalidate();
}

void RendererView::preRenderLocked() {
    // Allocate space to cache render results on the CPU
    auto viewCacheSize = mFrameWidth * mFrameHeight * 4;
    mCache.mFramebufferRGBA8.resize(viewCacheSize);

    // Caller should allocate CumulativeSum table of width * height * 16
    // bytes aligned to 16 byte boundary. height can be radius * 2 + 2 to
    // save memory as the buffer is treated as circular.
    auto scratchBufferNumItems =
            (mFrameWidth * (mFrameHeight + 1) * 16) / sizeof(uint32_t);
    if (mCache.mBlurScratchBuffer.size() < scratchBufferNumItems) {
        mCache.mBlurScratchBuffer.resize(scratchBufferNumItems);
    }
}

void RendererView::postRenderLocked() {
    // TODO(virtualscene-perf): apply blur on the GPU side
    if (mBlurFactor > 0) {
        applyBlurInPlaceCPU(mFrameWidth, mFrameHeight,
                            mCache.mFramebufferRGBA8.data(),
                            mCache.mBlurScratchBuffer.data(), mBlurFactor);
    }
}

void RendererView::applyBlurInPlaceCPU(int width,
                                       int height,
                                       uint8_t* rgbaDataInOut,
                                       int32_t* scratchBuffer,
                                       float sigma) {
    // No blur, return
    if (sigma <= 0 || !scratchBuffer) {
        return;
    }

    ScopeTimer timerObj(__func__);

    // Limit blur factor to avoid large kernels with low performance
    const int blurRadius = std::min((int)sigma, 16);
    const int stride = width * 4;

    std::vector<uint8_t> tempData(width * height * 4);
    int result =
            libyuv::ARGBBlur(rgbaDataInOut, stride, tempData.data(), stride,
                             scratchBuffer, stride, width, height, blurRadius);
    if (result != 0) {
        derror("%s: libyuv::ARGBBlur failed for radius=%d (result = %d)",
               __func__, blurRadius, result);
        return false;
    }
    memcpy(rgbaDataInOut, tempData.data(), tempData.size());
    return true;
}

/*******************************************************************************
 *                     Renderer routines
 ******************************************************************************/

class RendererImpl : public Renderer {
    DISALLOW_COPY_AND_ASSIGN(RendererImpl);

public:
    RendererImpl(int width, int height);
    ~RendererImpl();

    bool initialize();

    // Renderer public API.
    float getAspectRatio() override;
    bool isTextureLoaded(Texture texture) override;
    void getTextureInfo(Texture texture,
                        uint32_t* outWidth,
                        uint32_t* outHeight) override;

    void releaseTexture(Texture texture) override;
    void releaseMaterial(Material material) override;
    void releaseMesh(Mesh mesh) override;

    Material createMaterialCheckerboard() override;
    Material createMaterialTextured() override;
    Material createMaterialScreenSpace(const char* frag) override;

    Mesh createMesh(const VertexPositionUV* vertices,
                    size_t verticesSize,
                    const GLuint* indices,
                    size_t indicesSize) override;

    Texture loadTexture(const char* filename) override;
    Texture loadTextureAsync(const char* filename) override;
    Texture duplicateTexture(Texture texture) override;

    bool render(RendererView* view,
                const std::vector<RenderableObject>& renderables,
                float time) override;

    std::unique_ptr<RendererContext> makeCurrent() override;

private:
    void dispatchToRenderThread(std::function<void()>&& workItem);

    WorkerProcessingResult onLoaderCommand(LoaderCommand&& command);

    // Called by releaseMaterial, does not check to see if the material is a
    // standard material before releasing it.
    void releaseMaterialInternal(Material material);

    // Returns true if the material is a standard material.
    bool isStandardMaterial(Material material);

    // Helper to determine if GLES supports a given texture size.
    bool isTextureSizeValid(uint32_t width, uint32_t height);

    // Gets a cached texture, if it has already been loaded.
    //
    // Returns an invalid texture if the texture has not been cached, which can
    // be checked with Texture::isValid().
    Texture tryGetCachedTexture(const char* filename);

    // Create an OpenGL texture with the given width and height.
    Texture createEmptyTexture(uint32_t width, uint32_t height);

    // Create a texture.
    //
    // |state| - Texture state, if the texture is a placeholder or loaded data.
    // |filename| - If this texture was loaded from a file, the filename it was
    //              loaded from so that it can be cached, otherwise null.
    // |data| - TextureUtils result data.
    Texture createTextureInternal(TextureState state,
                                  const char* filename,
                                  const TextureUtils::Result& data);

    // Helper to load texture data from file and schedule an import to OpenGL on
    // the worker thread.
    //
    // |texture| - Texture handle.
    void onLoaderLoadTexture(Texture texture);

    // Replace texture with new data, used for async texture loading.
    //
    // |texture| - Texture handle.
    // |data| - TextureUtils result data.
    bool replaceTextureInternal(Texture texture,
                                const TextureUtils::Result& data);

    // Compile a shader from source.
    // |type| - GL shader type, such as GL_VERTEX_SHADER or GL_FRAGMENT_SHADER.
    // |shaderSource| - Shader source code.
    //
    // Returns a non-zero shader handle on success, or zero on failure.
    // The result must be released with glDeleteShader.
    GLuint compileShader(GLenum type, const char* shaderSource);

    // Link a vertex and fragment shader and return a program.
    // |vertexId| - GL vertex shader handle, must be non-zero.
    // |fragmentId| - GL fragment shader handle, must be non-zero.
    //
    // After this call, it is valid to call glDeleteShader; if the program was
    // successfully linked the program will keep them alive.
    //
    // Returns a non-zero program handle on success, or zero on failure.
    // The result must be released with glDeleteProgram.
    GLuint linkShaders(GLuint vertexId, GLuint fragmentId);

    GLint getAttribLocation(GLuint program,
                            const char* name,
                            bool optional = false);
    GLint getUniformLocation(GLuint program, const char* name);

    GLuint getTextureId(Texture texture) const;

    // Executes the given renderable, used internally by Renderer::render.
    // Should be called under mResourceLock.
    void processRenderable(const Renderable& renderable,
                           RenderableParameterCallback parameterCallback);

    const int mRenderWidth;
    const int mRenderHeight;

    std::unique_ptr<RenderTarget> mRenderTargets[2];
    std::unique_ptr<RenderTarget> mScreenRenderTarget;
    Mesh mEffectsMesh;
    std::vector<Material> mEffectsChain;

    WorkerThread<LoaderCommand> mLoaderThread;
    MessageChannel<std::function<void()>, 10> mRenderThreadDispatcherQueue;

    Lock mResourceLock;
    // {{ Protected by mResourceLock.
    int mNextResourceId = 0;
    std::unordered_map<int, MaterialData> mMaterials;
    std::unordered_map<int, MeshData> mMeshes;
    std::unordered_map<int, TextureData> mTextures;
    std::unordered_map<std::string, int> mTextureCache;
    // }} End protected by mResourceLock.

    RendererHeldResources mRendererResources;

    // Standard materials.
    Material mMaterialTextured;

    struct EglState {
        ~EglState() { destroy(); }

        bool initialize(int frameWidth, int frameHeight);
        void destroy();
        ScopedEglContext makeEglCurrent();

        void swapBuffers() {
            mEglDispatch->eglSwapBuffers(mEglDisplay, mEglSurface);
        }

        // Dispatch tables for EGL and GLESv2 APIs. Note that these will be
        // nullptr if there was a problem when loading the host libraries.
        const gfxstream::host::gl::EGLDispatch* mEglDispatch = nullptr;
        const gfxstream::host::gl::GLESv2Dispatch* mGles2 = nullptr;

        EGLDisplay mEglDisplay = EGL_NO_DISPLAY;
        EGLContext mEglContext = EGL_NO_CONTEXT;
        EGLSurface mEglSurface = EGL_NO_SURFACE;
        bool mEglInitialized = false;
    } mGL;
    const gfxstream::host::gl::GLESv2Dispatch* mGles2 =
            nullptr;  // Same as mGL.mGles2, kept for legacy reasons
};

std::unique_ptr<Renderer> Renderer::create() {
    dprint("virtualscene: Creating renderer.");
    std::unique_ptr<RendererImpl> renderer;
    renderer.reset(new RendererImpl(kRendererDefaultFramebufferWidth,
                                    kRendererDefaultFramebufferHeight));
    if (!renderer->initialize()) {
        derror("virtualscene: could not create renderer.");
        return nullptr;
    }
    return std::move(renderer);
}

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

RendererImpl::RendererImpl(int width, int height)
    : mRenderWidth(width),
      mRenderHeight(height),
      mLoaderThread([this](LoaderCommand&& command) {
          return onLoaderCommand(std::move(command));
      }) {}

bool RendererImpl::initialize() {
    if (!mGL.initialize(mRenderWidth, mRenderHeight)) {
        LOG(ERROR) << "Cannot initilize Egl";
        return false;
    }

    auto context = mGL.makeEglCurrent();
    if (!context.isValid()) {
        return false;
    }

    mGles2 = mGL.mGles2;

    mScreenRenderTarget = RenderTarget::createDefault(
            *this, mGles2, mRenderWidth, mRenderHeight);
    if (!mScreenRenderTarget) {
        E("%s: Failed creating screen render target.", __FUNCTION__);
        return false;
    }

    for (size_t i = 0; i < 2; ++i) {
        Texture renderTargetTexture =
                createEmptyTexture(mRenderWidth * kSuperSampleMultiple,
                                   mRenderHeight * kSuperSampleMultiple);
        mRenderTargets[i] = RenderTarget::createTextureTarget(
                *this, mGles2, getTextureId(renderTargetTexture),
                renderTargetTexture, mRenderWidth * kSuperSampleMultiple,
                mRenderHeight * kSuperSampleMultiple);
    }
    if (!mRenderTargets[0] || !mRenderTargets[1]) {
        E("%s: Failed creating texture render targets.", __FUNCTION__);
        return false;
    }

    Material fxaaEffect = createMaterialScreenSpace(kFxaaFragmentShader);
    if (!fxaaEffect.isValid()) {
        E("%s: Failed creating fxaa effect.", __FUNCTION__);
        return false;
    }
    mRendererResources.materials.push_back(fxaaEffect);
    mEffectsChain.push_back(std::move(fxaaEffect));

    Material blitEffect = createMaterialScreenSpace(kBlitFragmentShader);
    if (!blitEffect.isValid()) {
        E("%s: Failed creating blit effect.", __FUNCTION__);
        return false;
    }
    mRendererResources.materials.push_back(blitEffect);
    mEffectsChain.push_back(std::move(blitEffect));

    mEffectsMesh = Renderer::createMesh(kScreenQuadVerts, kScreenQuadIndices);
    if (!mEffectsMesh.isValid()) {
        E("%s: Failed creating effects mesh.", __FUNCTION__);
        return false;
    }
    mRendererResources.meshes.push_back(mEffectsMesh);

    mLoaderThread.start();

    return true;
}

RendererImpl::~RendererImpl() {
    auto context = mGL.makeEglCurrent();
    if (!context.isValid()) {
        E("%s: Failed to setup EGL context for cleanup.", __FUNCTION__);
        return;
    }

    mLoaderThread.enqueue(LoaderCommand(LoaderCommandType::Shutdown, -1));
    mLoaderThread.join();

    // Release Renderer-internal resources.
    for (auto& target : mRenderTargets) {
        target.reset();
    }
    mScreenRenderTarget.reset();

    for (auto& material : mRendererResources.materials) {
        // Call releaseMaterialInternal so that we also release the standard
        // materials.
        releaseMaterialInternal(material);
    }

    for (auto& mesh : mRendererResources.meshes) {
        releaseMesh(mesh);
    }

    for (auto& texture : mRendererResources.textures) {
        releaseTexture(texture);
    }

    // Report leak warnings for resources that weren't released.
    for (const auto& materialIt : mMaterials) {
        W("%s: Leaked material with id %d", __FUNCTION__, materialIt.first);
    }

    for (const auto& meshIt : mMeshes) {
        W("%s: Leaked mesh with id %d", __FUNCTION__, meshIt.first);
    }

    for (const auto& textureIt : mTextures) {
        W("%s: Leaked texture with id %d", __FUNCTION__, textureIt.first);
    }
}

float RendererImpl::getAspectRatio() {
    return static_cast<float>(mRenderWidth) / mRenderHeight;
}

bool RendererImpl::isTextureLoaded(Texture texture) {
    AutoLock lock(mResourceLock);
    auto textureIt = mTextures.find(texture.id);

    if (textureIt == mTextures.end()) {
        E("%s: Could not find texture id %d", __FUNCTION__, texture.id);
        return false;
    }

    return textureIt->second.mState == TextureState::Loaded;
}

void RendererImpl::getTextureInfo(Texture texture,
                                  uint32_t* outWidth,
                                  uint32_t* outHeight) {
    *outWidth = 0;
    *outHeight = 0;

    AutoLock lock(mResourceLock);
    auto textureIt = mTextures.find(texture.id);

    if (textureIt == mTextures.end()) {
        E("%s: Could not find texture id %d", __FUNCTION__, texture.id);
        return;
    }

    *outWidth = textureIt->second.mWidth;
    *outHeight = textureIt->second.mHeight;
}

void RendererImpl::releaseTexture(Texture texture) {
    if (!texture.isValid()) {
        return;
    }

    AutoLock lock(mResourceLock);
    auto textureIt = mTextures.find(texture.id);

    if (textureIt == mTextures.end()) {
        E("%s: Could not find texture id %d", __FUNCTION__, texture.id);
        return;
    }

    TextureData& textureData = textureIt->second;

    if (textureData.mRefCount == 0) {
        E("%s: Texture handle %d refcount error.", __FUNCTION__, texture.id);
        return;
    }

    T("%s: Release texture handle %d", __FUNCTION__, texture.id);

    if (--textureData.mRefCount == 0) {
        // Clean up outstanding OpenGL handles; releasing a 0 handle no-ops.
        mGles2->glDeleteTextures(1, &textureData.mTextureId);

        if (!textureData.mFilename.empty()) {
            mTextureCache.erase(textureData.mFilename);
        }

        mTextures.erase(textureIt);
    }
}

void RendererImpl::releaseMaterial(Material material) {
    if (!material.isValid()) {
        return;
    }

    AutoLock lock(mResourceLock);
    if (!isStandardMaterial(material)) {
        releaseMaterialInternal(material);
    }
}

void RendererImpl::releaseMesh(Mesh mesh) {
    if (!mesh.isValid()) {
        return;
    }

    AutoLock lock(mResourceLock);
    const auto meshIt = mMeshes.find(mesh.id);

    if (meshIt == mMeshes.end()) {
        E("%s: Could not find mesh id %d", __FUNCTION__, mesh.id);
        return;
    }

    const MeshData& meshData = meshIt->second;

    // Clean up outstanding OpenGL handles; releasing a 0 handle no-ops.
    mGles2->glDeleteBuffers(1, &meshData.mVertexBuffer);
    mGles2->glDeleteBuffers(1, &meshData.mIndexBuffer);

    mMeshes.erase(meshIt);
}

Material RendererImpl::createMaterialCheckerboard() {
    // Compile and setup shaders.
    const GLuint vertexId =
            compileShader(GL_VERTEX_SHADER, kTexturedVertexShader);
    const GLuint fragmentId =
            compileShader(GL_FRAGMENT_SHADER, kCheckerboardFragmentShader);

    GLuint program = 0;
    if (vertexId && fragmentId) {
        program = linkShaders(vertexId, fragmentId);
    }

    // Release the shaders, the program will keep their reference alive.
    mGles2->glDeleteShader(vertexId);
    mGles2->glDeleteShader(fragmentId);

    if (!program) {
        // Initialization failed, no program loaded.
        return Material();
    }

    MaterialData material;
    material.program = program;
    material.positionLocation = getAttribLocation(program, "in_position");
    material.uvLocation = getAttribLocation(program, "in_uv");
    material.mvpLocation = getUniformLocation(program, "u_modelViewProj");
    material.timeLocation = getUniformLocation(program, "u_time");

    {
        AutoLock lock(mResourceLock);
        const int id = mNextResourceId++;
        mMaterials[id] = material;

        Material materialHandle;
        materialHandle.id = id;
        return materialHandle;
    }
}

Material RendererImpl::createMaterialTextured() {
    // Return cached instance if it exists.
    if (mMaterialTextured.isValid()) {
        return mMaterialTextured;
    }

    // Compile and setup shaders.
    const GLuint vertexId =
            compileShader(GL_VERTEX_SHADER, kTexturedVertexShader);
    const GLuint fragmentId =
            compileShader(GL_FRAGMENT_SHADER, kTexturedFragmentShader);

    GLuint program = 0;
    if (vertexId && fragmentId) {
        program = linkShaders(vertexId, fragmentId);
    }

    // Release the shaders, the program will keep their reference alive.
    mGles2->glDeleteShader(vertexId);
    mGles2->glDeleteShader(fragmentId);

    if (!program) {
        // Initialization failed, no program loaded.
        return Material();
    }

    MaterialData material;
    material.program = program;
    material.positionLocation = getAttribLocation(program, "in_position");
    material.uvLocation = getAttribLocation(program, "in_uv");
    material.texSamplerLocation = getUniformLocation(program, "tex_sampler");
    material.mvpLocation = getUniformLocation(program, "u_modelViewProj");

    {
        AutoLock lock(mResourceLock);
        const int id = mNextResourceId++;
        mMaterials[id] = material;

        mMaterialTextured.id = id;
        mRendererResources.materials.push_back(mMaterialTextured);
        return mMaterialTextured;
    }
}

Material RendererImpl::createMaterialScreenSpace(const char* frag) {
    // Compile and setup shaders.
    const GLuint vertexId =
            compileShader(GL_VERTEX_SHADER, kScreenSpaceVertexShader);
    const GLuint fragmentId = compileShader(GL_FRAGMENT_SHADER, frag);

    GLuint program = 0;
    if (vertexId && fragmentId) {
        program = linkShaders(vertexId, fragmentId);
    }

    // Release the shaders, the program will keep their reference alive.
    mGles2->glDeleteShader(vertexId);
    mGles2->glDeleteShader(fragmentId);

    if (!program) {
        // Initialization failed, no program loaded.
        return Material();
    }

    MaterialData material;
    material.program = program;
    material.positionLocation = getAttribLocation(program, "in_position");
    material.uvLocation = getAttribLocation(program, "in_uv", true);
    material.texSamplerLocation = getUniformLocation(program, "tex_sampler");
    material.resolutionLocation = getUniformLocation(program, "resolution");

    {
        AutoLock lock(mResourceLock);
        const int id = mNextResourceId++;
        mMaterials[id] = material;

        Material materialHandle;
        materialHandle.id = id;
        return materialHandle;
    }
}

Mesh RendererImpl::createMesh(const VertexPositionUV* vertices,
                              size_t verticesSize,
                              const GLuint* indices,
                              size_t indicesSize) {
    GLuint vertexBuffer = 0;
    GLuint indexBuffer = 0;

    mGles2->glGenBuffers(1, &vertexBuffer);
    mGles2->glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    mGles2->glBufferData(GL_ARRAY_BUFFER,
                         verticesSize * sizeof(VertexPositionUV), vertices,
                         GL_STATIC_DRAW);
    mGles2->glBindBuffer(GL_ARRAY_BUFFER, 0);

    mGles2->glGenBuffers(1, &indexBuffer);
    mGles2->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
    mGles2->glBufferData(GL_ELEMENT_ARRAY_BUFFER, indicesSize * sizeof(GLuint),
                         indices, GL_STATIC_DRAW);
    mGles2->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    VertexInfo info;
    info.hasUV = true;
    info.vertexSize = sizeof(VertexPositionUV);
    info.posOffset = offsetof(VertexPositionUV, pos);
    info.uvOffset = offsetof(VertexPositionUV, uv);

    MeshData mesh;
    mesh.mVertexInfo = info;
    mesh.mVertexBuffer = vertexBuffer;
    mesh.mIndexBuffer = indexBuffer;
    mesh.mIndexCount = indicesSize;

    {
        AutoLock lock(mResourceLock);
        const int id = mNextResourceId++;
        mMeshes[id] = mesh;

        Mesh meshHandle;
        meshHandle.id = id;
        return meshHandle;
    }
}

Texture RendererImpl::loadTexture(const char* filename) {
    const uint64_t loadStartUs = System::get()->getHighResTimeUs();

    std::string path;
    if (!PathUtils::isAbsolute(filename)) {
        path = PathUtils::join(System::get()->getLauncherDirectory(),
                               "resources", filename);
    } else {
        path = filename;
    }

    Texture cachedTexture = tryGetCachedTexture(path.c_str());
    if (cachedTexture.isValid()) {
        return cachedTexture;
    }

    std::optional<TextureUtils::Result> result = TextureUtils::load(path.c_str());
    if (!result) {
        E("%s: Failed to load texture from file '%s'", __FUNCTION__,
          path.c_str());
        return Texture();
    }

    const uint64_t loadEndUs = System::get()->getHighResTimeUs();

    Texture texture = createTextureInternal(TextureState::Loaded, path.c_str(),
                                            result.value());
    // The texture may be invalid here, but the error is already logged so pass
    // it through as-is.

    const uint64_t importEndUs = System::get()->getHighResTimeUs();
    D("%s: Sync load texture %d in %" PRIu64 "ms, [read: %" PRIu64
      "ms, import: %" PRIu64 "ms]",
      __FUNCTION__, texture.id, durationUsToMs(loadStartUs, importEndUs),
      durationUsToMs(loadStartUs, loadEndUs),
      durationUsToMs(loadEndUs, importEndUs));

    return texture;
}

Texture RendererImpl::loadTextureAsync(const char* filename) {
    std::string path;
    if (!PathUtils::isAbsolute(filename)) {
        path = PathUtils::join(System::get()->getLauncherDirectory(),
                               "resources", filename);
    } else {
        path = filename;
    }

    Texture cachedTexture = tryGetCachedTexture(path.c_str());
    if (cachedTexture.isValid()) {
        return cachedTexture;
    }

    Texture texture =
            createTextureInternal(TextureState::Placeholder, path.c_str(),
                                  TextureUtils::createPlaceholder());
    mLoaderThread.enqueue(
            LoaderCommand(LoaderCommandType::LoadTexture, texture.id));
    return texture;
}

Texture RendererImpl::duplicateTexture(Texture texture) {
    if (!texture.isValid()) {
        return Texture();
    }

    AutoLock lock(mResourceLock);
    auto textureIt = mTextures.find(texture.id);

    if (textureIt == mTextures.end()) {
        D("%s: Could not find texture id %d.", __FUNCTION__, texture.id);
        return Texture();
    }

    TextureData& textureData = textureIt->second;
    ++textureData.mRefCount;

    // The texture reference stays the same, but now it has an additional
    // reference on it.
    return texture;
}

bool RendererImpl::render(RendererView* lockedView,
                          const std::vector<RenderableObject>& renderables,
                          float time) {
    ScopeTimer timerObj(__func__);
    mRenderTargets[0]->bind();

    mGles2->glEnable(GL_CULL_FACE);
    mGles2->glEnable(GL_DEPTH_TEST);
    // For basic transparency of posters, always rendered after the main scene.
    mGles2->glEnable(GL_BLEND);
    mGles2->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    mGles2->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // Unpremultiplied, as supplied by image decoders.
    mGles2->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    mGles2->glCullFace(GL_BACK);
    // The SceneCamera adds a y-flip and actually renders the scene upside-down,
    // because that's what is expected by the camera stack.  The y-flip changes
    // the winding order, so the front-face is actually clockwise in window
    // space.
    mGles2->glFrontFace(GL_CW);

    {
        // Acquire the loader lock for the rest of rendering, it will involve
        // accessing mTextures, mMeshes, and mMaterials to map the IDs to OpenGL
        // resources.
        AutoLock lock(mResourceLock);

        // Render scene objects.
        for (auto& renderObject : renderables) {
            processRenderable(
                    renderObject.renderable, [&](const MaterialData& material) {
                        mGles2->glUniformMatrix4fv(
                                material.mvpLocation, 1, GL_FALSE,
                                &renderObject.modelViewProj[0][0]);
                        mGles2->glUniform1f(material.timeLocation, time);
                    });
        }

        mGles2->glDisable(GL_DEPTH_TEST);
        mGles2->glDisable(GL_BLEND);
        // Disable culling for screen-space rendering, since that is done in the
        // default CCW order.
        mGles2->glDisable(GL_CULL_FACE);

        assert(!mEffectsChain.empty());
        for (size_t i = 0; i < mEffectsChain.size(); i++) {
            int superSampleMultiple = kSuperSampleMultiple;
            if (i == mEffectsChain.size() - 1) {
                superSampleMultiple = 1;
                mScreenRenderTarget->bind();
            } else {
                mRenderTargets[(i + 1) % 2]->bind();
            }

            Renderable renderable;
            renderable.texture = mRenderTargets[i % 2]->getTexture();
            renderable.material = mEffectsChain[i];
            renderable.mesh = mEffectsMesh;

            processRenderable(renderable, [&](const MaterialData& material) {
                mGles2->glUniform2f(
                        material.resolutionLocation,
                        1.f / (superSampleMultiple * mRenderWidth),
                        1.f / (superSampleMultiple * mRenderHeight));
                mGles2->glUniform1f(material.timeLocation, time);
            });
        }

        mGles2->glBindBuffer(GL_ARRAY_BUFFER, 0);
        mGles2->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        mGles2->glBindTexture(GL_TEXTURE_2D, 0);
        mGles2->glUseProgram(0);
    }

    // Process render thread dispatcher queue, do this after rendering to avoid
    // changing any dependencies after Renderables have been updated for the
    // frame.
    std::function<void()> workItem;
    while (mRenderThreadDispatcherQueue.tryReceive(&workItem)) {
        workItem();
    }

    // Finish rendering and readback the framebuffer
    mGL.swapBuffers();

    // TODO(virtualscene-perf): Implement non blocking readback
    auto readbackSize = mRenderWidth * mRenderHeight * 4;
    if (mRenderWidth == lockedView->getWidthLocked() &&
        mRenderHeight == lockedView->getHeightLocked()) {
        // Same size, readback directly into the cache
        mGles2->glReadPixels(0, 0, mRenderWidth, mRenderHeight, GL_RGBA,
                             GL_UNSIGNED_BYTE,
                             lockedView->mCache.mFramebufferRGBA8.data());
    } else {
        // Renderer is created with a different size, readback full and resize
        // for the cache
        T("%s: Renderer resolution mismatch (%dx%d), resizing virtual "
          "scene output to (%dx%d)",
          __FUNCTION__, mRenderWidth, mRenderHeight,
          lockedView->getWidthLocked(), lockedView->getHeightLocked());
        std::vector<uint8_t> readback_r8g8b8a8;
        readback_r8g8b8a8.resize(readbackSize);
        mGles2->glReadPixels(0, 0, mRenderWidth, mRenderHeight, GL_RGBA,
                             GL_UNSIGNED_BYTE, readback_r8g8b8a8.data());

        // Resize an RGBA image using Bilinear Interpolation.
        ImageScaler scaler(lockedView->getWidthLocked(),
                           lockedView->getHeightLocked(),
                           lockedView->getFramebufferLocked().data());
        if (!scaler.updateImage(mRenderWidth, mRenderHeight,
                                readback_r8g8b8a8.data(),
                                ImageScaler::ScaleMode::ScaleToFill)) {
            derror("%s: Failed to resize the framebuffer for the view",
                   __FUNCTION__);
            return false;
        }
    }

    return true;
}

std::unique_ptr<RendererContext> RendererImpl::makeCurrent() {
    return std::make_unique<ScopedEglContext>(mGL.makeEglCurrent());
}

void RendererImpl::dispatchToRenderThread(std::function<void()>&& workItem) {
    mRenderThreadDispatcherQueue.send(std::move(workItem));
}

WorkerProcessingResult RendererImpl::onLoaderCommand(LoaderCommand&& command) {
    if (command.mType == LoaderCommandType::Shutdown) {
        return WorkerProcessingResult::Stop;
    } else if (command.mType == LoaderCommandType::LoadTexture) {
        Texture texture;
        texture.id = command.mHandle;
        onLoaderLoadTexture(std::move(texture));
    } else {
        E("%s: Invalid command %d", __FUNCTION__,
          static_cast<int>(command.mType));
    }

    return WorkerProcessingResult::Continue;
}

void RendererImpl::releaseMaterialInternal(Material material) {
    const auto materialIt = mMaterials.find(material.id);

    if (materialIt == mMaterials.end()) {
        E("%s: Could not find material id %d", __FUNCTION__, material.id);
        return;
    }

    const MaterialData& materialData = materialIt->second;

    // Clean up outstanding OpenGL handles; releasing a 0 handle no-ops.
    mGles2->glDeleteProgram(materialData.program);

    mMaterials.erase(materialIt);
}

bool RendererImpl::isStandardMaterial(Material material) {
    if (material.id == mMaterialTextured.id) {
        return true;
    }

    return false;
}

bool RendererImpl::isTextureSizeValid(uint32_t width, uint32_t height) {
    GLint maxTextureSize = 0;
    mGles2->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    return width != 0 && height != 0 && width <= maxTextureSize &&
           height <= maxTextureSize;
}

Texture RendererImpl::tryGetCachedTexture(const char* filename) {
    AutoLock lock(mResourceLock);
    const auto it = mTextureCache.find(filename);
    if (it == mTextureCache.end()) {
        return Texture();
    }

    const int id = it->second;
    ++mTextures[id].mRefCount;

    Texture texture;
    texture.id = id;
    return texture;
}

Texture RendererImpl::createEmptyTexture(uint32_t width, uint32_t height) {
    if (!isTextureSizeValid(width, height)) {
        E("createEmptyTexture: Invalid texture size, %d x %d, "
          "GL_MAX_TEXTURE_SIZE = %d",
          width, height, GL_MAX_TEXTURE_SIZE);
        return Texture();
    }

    return createTextureInternal(TextureState::Loaded, nullptr,
                                 TextureUtils::createEmpty(width, height));
}

Texture RendererImpl::createTextureInternal(TextureState state,
                                            const char* filename,
                                            const TextureUtils::Result& data) {
    const GLenum textureFormat =
            data.mFormat == TextureUtils::Format::RGBA32 ? GL_RGBA : GL_RGB;

    GLuint textureId = 0;
    mGles2->glGenTextures(1, &textureId);
    mGles2->glBindTexture(GL_TEXTURE_2D, textureId);
    mGles2->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    mGles2->glTexImage2D(GL_TEXTURE_2D, 0, textureFormat, data.mWidth,
                         data.mHeight, 0, textureFormat, GL_UNSIGNED_BYTE,
                         data.mBuffer.empty() ? 0 : data.mBuffer.data());
    mGles2->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    mGles2->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    mGles2->glBindTexture(GL_TEXTURE_2D, 0);

#ifdef DEBUG
    // Only check for GL error on debug builds to avoid a potential synchronous
    // flush.
    const GLenum error = mGles2->glGetError();
    if (error != GL_NO_ERROR) {
        E("%s: GL error %d", __FUNCTION__, error);
        mGles2->glDeleteTextures(1, &textureId);
        return Texture();
    }
#endif

    AutoLock lock(mResourceLock);
    const int id = mNextResourceId++;
    T("%s: Create texture handle %d for file '%s'", __FUNCTION__, id,
      filename ? filename : "<none>");

    TextureData textureData;
    textureData.mRefCount = 1;
    textureData.mState = state;
    textureData.mTextureId = textureId;
    if (filename) {
        textureData.mFilename = filename;
        mTextureCache[filename] = id;
    }
    textureData.mWidth = data.mWidth;
    textureData.mHeight = data.mHeight;

    mTextures[id] = textureData;

    Texture texture;
    texture.id = id;
    return texture;
}

void RendererImpl::onLoaderLoadTexture(Texture texture) {
    const uint64_t loadStartUs = System::get()->getHighResTimeUs();
    std::string filename;
    {
        // Scope the lock so that TextureUtils::load happens outside of the
        // lock.
        AutoLock lock(mResourceLock);
        auto textureIt = mTextures.find(texture.id);

        if (textureIt == mTextures.end()) {
            D("%s: Could not find texture id %d, was the load canceled?",
              __FUNCTION__, texture.id);
            return;
        }

        if (textureIt->second.mState == TextureState::Loaded) {
            D("%s: Texture %d already loaded.", __FUNCTION__, texture.id);
            return;
        }

        filename = textureIt->second.mFilename;
    }

    std::optional<TextureUtils::Result> resultOpt =
            TextureUtils::load(filename.c_str());
    if (!resultOpt) {
        E("%s: Failed to load texture %d from file '%s'", __FUNCTION__,
          texture.id, filename.c_str());
        return;
    }

    const uint64_t loadEndUs = System::get()->getHighResTimeUs();

    dispatchToRenderThread([this, texture, loadStartUs, loadEndUs,
                            result = std::move(resultOpt.value())]() {
        const uint64_t importStartUs = System::get()->getHighResTimeUs();
        if (!replaceTextureInternal(texture, result)) {
            return;
        }
        const uint64_t importEndUs = System::get()->getHighResTimeUs();

        D("onLoaderLoadTexture: Async load texture %d in %" PRIu64
          "ms, [read: %" PRIu64 "ms, import: %" PRIu64 "ms]",
          texture.id, durationUsToMs(loadStartUs, importEndUs),
          durationUsToMs(loadStartUs, loadEndUs),
          durationUsToMs(importStartUs, importEndUs));
    });
}

bool RendererImpl::replaceTextureInternal(Texture texture,
                                          const TextureUtils::Result& data) {
    if (!isTextureSizeValid(data.mWidth, data.mHeight)) {
        E("Invalid texture size, %d x %d, GL_MAX_TEXTURE_SIZE = %d",
          data.mWidth, data.mHeight, GL_MAX_TEXTURE_SIZE);
        return false;
    }

    AutoLock lock(mResourceLock);
    auto textureIt = mTextures.find(texture.id);

    if (textureIt == mTextures.end()) {
        D("%s: Could not find texture id %d, was the load canceled?",
          __FUNCTION__, texture.id);
        return false;
    }

    TextureData& textureData = textureIt->second;

    const GLenum textureFormat =
            data.mFormat == TextureUtils::Format::RGBA32 ? GL_RGBA : GL_RGB;

    mGles2->glBindTexture(GL_TEXTURE_2D, textureData.mTextureId);
    mGles2->glTexImage2D(GL_TEXTURE_2D, 0, textureFormat, data.mWidth,
                         data.mHeight, 0, textureFormat, GL_UNSIGNED_BYTE,
                         data.mBuffer.data());
    mGles2->glGenerateMipmap(GL_TEXTURE_2D);
    mGles2->glBindTexture(GL_TEXTURE_2D, 0);

#ifdef DEBUG
    // Only check for GL error on debug builds to avoid a potential synchronous
    // flush.
    const GLenum error = mGles2->glGetError();
    if (error != GL_NO_ERROR) {
        E("%s: GL error %d", __FUNCTION__, error);
        return false;
    }
#endif

    textureData.mState = TextureState::Loaded;
    textureData.mWidth = data.mWidth;
    textureData.mHeight = data.mHeight;
    return true;
}

GLuint RendererImpl::compileShader(GLenum type, const char* shaderSource) {
    const GLuint shaderId = mGles2->glCreateShader(type);

    mGles2->glShaderSource(shaderId, 1, &shaderSource, nullptr);
    mGles2->glCompileShader(shaderId);

    // Output the error message if the compile failed.
    GLint result = GL_FALSE;
    mGles2->glGetShaderiv(shaderId, GL_COMPILE_STATUS, &result);
    if (result != GL_TRUE) {
        int logLength = 0;
        mGles2->glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &logLength);

        if (logLength) {
            std::vector<char> message(logLength + 1);
            mGles2->glGetShaderInfoLog(shaderId, logLength, nullptr,
                                       message.data());
            E("%s: Failed to compile shader: %s", __FUNCTION__, message.data());
        } else {
            E("%s: Failed to compile shader", __FUNCTION__);
        }

        mGles2->glDeleteShader(shaderId);
        return 0;
    }

    return shaderId;
}

GLuint RendererImpl::linkShaders(GLuint vertexId, GLuint fragmentId) {
    const GLuint programId = mGles2->glCreateProgram();
    mGles2->glAttachShader(programId, vertexId);
    mGles2->glAttachShader(programId, fragmentId);
    mGles2->glLinkProgram(programId);

    // Output the error message if the program failed to link.
    GLint result = GL_FALSE;
    mGles2->glGetProgramiv(programId, GL_LINK_STATUS, &result);
    if (result != GL_TRUE) {
        int logLength = 0;
        mGles2->glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &logLength);

        if (logLength) {
            std::vector<char> message(logLength + 1);
            mGles2->glGetProgramInfoLog(programId, logLength, nullptr,
                                        message.data());
            E("%s: Failed to link program: %s", __FUNCTION__, message.data());
        } else {
            E("%s: Failed to link program", __FUNCTION__);
        }

        mGles2->glDeleteProgram(programId);
        return 0;
    }

    return programId;
}

GLint RendererImpl::getAttribLocation(GLuint program,
                                      const char* name,
                                      bool optional) {
    GLint location = mGles2->glGetAttribLocation(program, name);
    if (location < 0 && !optional) {
        W("%s: Program attrib '%s' not found", __FUNCTION__, name);
    }

    return location;
}

GLint RendererImpl::getUniformLocation(GLuint program, const char* name) {
    GLint location = mGles2->glGetUniformLocation(program, name);
    if (location < 0) {
        W("%s: Program uniform '%s' not found", __FUNCTION__, name);
    }

    return location;
}

GLuint RendererImpl::getTextureId(Texture texture) const {
    return mTextures.at(texture.id).mTextureId;
}

// Called under mResourceLock.
void RendererImpl::processRenderable(
        const Renderable& renderable,
        RenderableParameterCallback parameterCallback) {
    const auto materialIt = mMaterials.find(renderable.material.id);
    const auto meshIt = mMeshes.find(renderable.mesh.id);

    if (materialIt == mMaterials.end()) {
        E("%s: Could not find material id %d", __FUNCTION__,
          renderable.material.id);
        return;
    }

    if (meshIt == mMeshes.end()) {
        E("%s: Could not find mesh id %d", __FUNCTION__, renderable.mesh.id);
        return;
    }

    const MaterialData& material = materialIt->second;
    const MeshData& mesh = meshIt->second;
    const VertexInfo& vertexInfo = mesh.mVertexInfo;

    mGles2->glUseProgram(material.program);

    // Bind the texture if it exists.
    if (renderable.texture.isValid()) {
        mGles2->glActiveTexture(GL_TEXTURE0);
        mGles2->glUniform1i(material.texSamplerLocation, 0);

        mGles2->glBindTexture(GL_TEXTURE_2D, getTextureId(renderable.texture));
    } else {
        mGles2->glBindTexture(GL_TEXTURE_2D, 0);
    }

    parameterCallback(material);

    mGles2->glBindBuffer(GL_ARRAY_BUFFER, mesh.mVertexBuffer);
    mGles2->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.mIndexBuffer);

    // glVertexAttribPointer requires the GL_ARRAY_BUFFER to be bound, keep
    // it bound until all of the vertex attribs are initialized.
    if (material.positionLocation >= 0) {
        mGles2->glEnableVertexAttribArray(material.positionLocation);
        mGles2->glVertexAttribPointer(
                material.positionLocation,
                3,                                               // size
                GL_FLOAT,                                        // type
                GL_FALSE,                                        // normalized?
                vertexInfo.vertexSize,                           // stride
                reinterpret_cast<void*>(vertexInfo.posOffset));  // offset
    }

    if (material.uvLocation >= 0 && vertexInfo.hasUV) {
        mGles2->glEnableVertexAttribArray(material.uvLocation);
        mGles2->glVertexAttribPointer(
                material.uvLocation,
                2,                                              // size
                GL_FLOAT,                                       // type
                GL_FALSE,                                       // normalized?
                vertexInfo.vertexSize,                          // stride
                reinterpret_cast<void*>(vertexInfo.uvOffset));  // offset
    }

    mGles2->glDrawElements(GL_TRIANGLES, mesh.mIndexCount, GL_UNSIGNED_INT,
                           nullptr);

    if (material.positionLocation >= 0) {
        mGles2->glDisableVertexAttribArray(material.positionLocation);
    }
    if (material.uvLocation >= 0 && vertexInfo.hasUV) {
        mGles2->glDisableVertexAttribArray(material.uvLocation);
    }
}

bool RendererImpl::EglState::initialize(int frameWidth, int frameHeight) {
    mEglDispatch = (const EGLDispatch*)ScenesManager::getEglDispatch();
    mGles2 = (const GLESv2Dispatch*)ScenesManager::getGles2Dispatch();
    if (!mEglDispatch || !mGles2) {
        LOG(ERROR) << "EglState::initialize failed, cannot get GL dispatch.";
        return false;
    }

    mEglDisplay = mEglDispatch->eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mEglDisplay == EGL_NO_DISPLAY) {
        LOG(ERROR) << "eglGetDisplay failed, error "
                   << mEglDispatch->eglGetError();
        return false;
    }

    EGLint eglMaj = 0;
    EGLint eglMin = 0;

    // Try to initialize EGL display.
    // Initializing an already-initialized display is OK.
    if (mEglDispatch->eglInitialize(mEglDisplay, &eglMaj, &eglMin) ==
        EGL_FALSE) {
        LOG(ERROR) << "eglInitialize failed, error "
                   << mEglDispatch->eglGetError();
        return false;
    }

    // Get an EGL config.
    const EGLint attribs[] = {EGL_SURFACE_TYPE,
                              EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES_BIT,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_RED_SIZE,
                              8,
                              EGL_DEPTH_SIZE,
                              16,
                              EGL_NONE};
    EGLint numConfig = 0;
    EGLConfig eglConfig;
    EGLBoolean chooseResult = mEglDispatch->eglChooseConfig(
            mEglDisplay, attribs, &eglConfig, 1, &numConfig);
    if (chooseResult == EGL_FALSE || numConfig < 1) {
        LOG(ERROR) << "eglChooseConfig failed, error "
                   << mEglDispatch->eglGetError();
        return false;
    }

    // Create a context.
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    mEglContext = mEglDispatch->eglCreateContext(
            mEglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (mEglContext == EGL_NO_CONTEXT) {
        LOG(ERROR) << "eglCreateContext failed, error "
                   << mEglDispatch->eglGetError();
        return false;
    }

    // Finally, create a window surface associated with this widget.
    const EGLint pbufferAttribs[] = {EGL_WIDTH, frameWidth, EGL_HEIGHT,
                                     frameHeight, EGL_NONE};
    mEglSurface = mEglDispatch->eglCreatePbufferSurface(mEglDisplay, eglConfig,
                                                        pbufferAttribs);
    if (mEglSurface == EGL_NO_SURFACE) {
        LOG(ERROR) << "eglCreatePbufferSurface failed, error "
                   << mEglDispatch->eglGetError();
        return false;
    }

    mEglInitialized = true;
    return true;
}

ScopedEglContext RendererImpl::EglState::makeEglCurrent() {
    if (!mEglDispatch) {
        LOG(ERROR) << "eglMakeCurrent failed, no EGL";
        return ScopedEglContext(nullptr, EGL_NO_DISPLAY);
    }
    const EGLBoolean result = mEglDispatch->eglMakeCurrent(
            mEglDisplay, mEglSurface, mEglSurface, mEglContext);
    if (result == EGL_FALSE) {
        LOG(ERROR) << "eglMakeCurrent failed, error "
                   << mEglDispatch->eglGetError();

        // Explicitly set mEglInitialized to false here to avoid an infinite
        // loop with stopCapturing.
        mEglInitialized = false;

        // Return an empty ScopedEglContext because we failed to call
        // eglMakeCurrent.
        return ScopedEglContext(nullptr, EGL_NO_DISPLAY);
    }

    return ScopedEglContext(mEglDispatch, mEglDisplay);
}

void RendererImpl::EglState::destroy() {
    if (mEglDispatch && mEglDisplay != EGL_NO_DISPLAY) {
        mEglDispatch->eglDestroySurface(mEglDisplay, mEglSurface);
        mEglDispatch->eglDestroyContext(mEglDisplay, mEglContext);
        // Don't eglTerminate the display, we don't own the instance.
        mEglDisplay = EGL_NO_DISPLAY;
    }
}

}  // namespace ver
}  // namespace android
