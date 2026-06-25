/*
 * Copyright (C) 2026 The Android Open Source Project
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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "android/utils/compiler.h"
#include "ver/export.h"
#include "ver/virtual_environment_renderer_types.h"

using android::ver::VerWebcamHandle;

/**
 * @file virtual_environment_renderer.h
 * @brief Main entry point for the Virtual Environment Renderer (VER).
 *
 * This header defines the API for managing virtual scenes and render views.
 */

// --- Global Initialization & Cleanup ---

/**
 * @brief Initializes the renderer, sets the resource search paths.
 *
 * Paths will be searched in the order they are provided in the vector.
 *
 * @param resourceBasePaths A vector of filesystem paths to search for
 * resources.
 * @param eglDispatch Pointer to the EGL dispatch table.
 * @param gles2Dispatch Pointer to the GLESv2 dispatch table.
 */
VER_EXPORT void ver_initialize(
        const std::vector<std::filesystem::path>& resourceBasePaths,
        const void* eglDispatch,
        const void* gles2Dispatch);

/**
 * @brief Removes all virtual scenes and cleans up the renderer resources.
 *
 * @return true if cleanup was successful, false otherwise.
 */
VER_EXPORT bool ver_cleanup(void);

// --- Scene Management ---

/**
 * @brief Validates the scene configuration
 *
 * @param config The configuration for the new scene.
 * @return true if the file argument referenced in the config is present based
 * on current base resource paths.
 */
VER_EXPORT bool ver_scene_config_file_exists(const VerSceneConfig& config);

/**
 * @brief Creates a new virtual scene based on the provided configuration.
 *
 * @param config The configuration for the new scene.
 * @return A handle to the newly created scene, or VER_INVALID_HANDLE on
 * failure.
 */
VER_EXPORT VerSceneHandle ver_create_scene(const VerSceneConfig& config);

/**
 * @brief Removes a specific virtual scene.
 *
 * @param scene The handle of the scene to destroy.
 */
VER_EXPORT void ver_destroy_scene(VerSceneHandle scene);

/**
 * @brief Renders the virtual scene to the given view.
 *
 * @param scene The handle of the scene to render.
 * @param view The handle of the render view to render into.
 * @param finish_cb Callback function to be called when rendering is complete.
 * @param out_frame_time Pointer to receive the calculated frame time
 * (optional).
 * @return true if rendering was initiated successfully, false otherwise.
 */
VER_EXPORT bool ver_render_view(VerSceneHandle scene,
                                VerRenderViewHandle view,
                                VerRenderFinishCallback finish_cb,
                                uint64_t* out_frame_time);

/**
 * @brief Loads user-specific resources for the scene.
 * TODO(virtualscene-library): consider removing.
 *
 * @param scene The handle of the scene.
 * @param loadRendererResourcesCallback Callback called when resources are
 * loaded.
 */
VER_EXPORT void ver_scene_load_user_resources(
        VerSceneHandle scene,
        std::function<void()> loadRendererResourcesCallback);

/**
 * @brief Unloads user-specific resources from the scene.
 *
 * @param scene The handle of the scene.
 */
VER_EXPORT void ver_scene_unload_user_resources(VerSceneHandle scene);

// --- Scene Properties & State ---

/**
 * @brief Retrieves the configuration of the given scene.
 *
 * @param scene The handle of the scene.
 * @return A pointer to the scene's configuration.
 */
VER_EXPORT const VerSceneConfig* ver_scene_get_config(VerSceneHandle scene);

/**
 * @brief Retrieves the mode of the given scene.
 *
 * @param scene The handle of the scene.
 * @return The mode of the scene.
 */
VER_EXPORT VerSceneConfig::Mode ver_scene_get_mode(VerSceneHandle scene);

/**
 * @brief Updates the scene state.
 *
 * @param scene The handle of the scene.
 * @param update_time If true, the scene's internal time will be advanced.
 */
VER_EXPORT void ver_scene_update(VerSceneHandle scene, bool update_time);

/**
 * @brief Retrieves the scene's base rotation.
 *
 * @param scene The handle of the scene.
 * @return The rotation value (e.g., in degrees).
 */
VER_EXPORT int ver_scene_get_scene_rotation(VerSceneHandle scene);

/**
 * @brief Calculates a version hash for a specific scene and view pair.
 *
 * This can be used to determine if a cached render is still valid.
 *
 * @param scene The handle of the scene.
 * @param view The handle of the render view.
 * @return The calculated 64-bit version hash.
 */
VER_EXPORT uint64_t
ver_scene_get_version_hash_for_view(VerSceneHandle scene,
                                    VerRenderViewHandle view);

/**
 * @brief Retrieves the current frame time of the scene in microseconds.
 *
 * @param scene The handle of the scene.
 * @return The frame time in microseconds.
 */
VER_EXPORT uint64_t ver_scene_get_frame_time_us(VerSceneHandle scene);

// --- Poster Management ---

/**
 * @brief Updates the scale of a specific poster within the scene.
 *
 * @param scene The handle of the scene.
 * @param posterName The unique name of the poster.
 * @param scale The new scale factor for the poster.
 */
VER_EXPORT void ver_scene_update_poster_scale(VerSceneHandle scene,
                                              const char* posterName,
                                              float scale);

/**
 * @brief Loads a poster into the scene from a file.
 *
 * @param scene The handle of the scene.
 * @param posterName The unique name to assign to the poster.
 * @param filename The path to the poster image file.
 * @param scale The initial scale for the poster.
 * @return A unique identifier for the loaded poster.
 */
VER_EXPORT uint64_t ver_scene_load_poster(VerSceneHandle scene,
                                          const char* posterName,
                                          const char* filename,
                                          float scale);

/**
 * @brief Creates a location for a poster within the scene.
 *
 * @param scene The handle of the scene.
 * @param info Information about the poster's location and properties.
 * @return true if the poster location was created successfully.
 */
VER_EXPORT bool ver_scene_create_poster_location(
        VerSceneHandle scene,
        const android::ver::PosterInfo& info);

// --- Render View Management ---

/**
 * @brief Creates a new render view.
 *
 * @return A handle to the newly created render view.
 */
VER_EXPORT VerRenderViewHandle ver_create_render_view();

/**
 * @brief Destroys a render view.
 *
 * @param view The handle of the render view to destroy.
 */
VER_EXPORT void ver_destroy_render_view(VerRenderViewHandle view);

/**
 * @brief Sets the dimensions of a render view.
 *
 * @param view The handle of the render view.
 * @param frameWidth The width of the frame in pixels.
 * @param frameHeight The height of the frame in pixels.
 */
VER_EXPORT void ver_render_view_set_dimensions(VerRenderViewHandle view,
                                               int32_t frameWidth,
                                               int32_t frameHeight);

/**
 * @brief Retrieves the dimensions of a render view.
 *
 * @note This function requires the view to be locked.
 *
 * @param view The handle of the render view.
 * @param frameWidth Pointer to receive the width.
 * @param frameHeight Pointer to receive the height.
 */
VER_EXPORT void ver_render_view_get_dimensions(VerRenderViewHandle view,
                                               int32_t* frameWidth,
                                               int32_t* frameHeight);

/**
 * @brief Sets the view projection matrix for a render view.
 *
 * @param view The handle of the render view.
 * @param viewProjMatrix Pointer to a 4x4 matrix (16 floats).
 */
VER_EXPORT void ver_render_view_set_view_projection(
        VerRenderViewHandle view,
        const float* viewProjMatrix);

/**
 * @brief Sets the blur factor for a render view.
 *
 * @param view The handle of the render view.
 * @param factor The blur factor (0.0 for no blur).
 */
VER_EXPORT void ver_render_view_set_blur_factor(VerRenderViewHandle view,
                                                float factor);

/**
 * @brief Retrieves the CPU framebuffer cache for the render view.
 *
 * @note This function requires the view to be locked.
 *
 * @param view The handle of the render view.
 * @param out_fb_data_ptr Pointer to receive the pointer to the framebuffer
 * data.
 * @param out_fb_data_size Pointer to receive the size of the framebuffer data.
 */
VER_EXPORT void ver_render_view_get_framebuffer(VerRenderViewHandle view,
                                                const uint8_t** out_fb_data_ptr,
                                                uint64_t* out_fb_data_size);

/**
 * @brief Checks if the framebuffer cache of the view is valid for a given scene
 * state.
 *
 * @param view The handle of the render view.
 * @param sceneHash The version hash of the scene.
 * @param frameTime The frame time of the scene.
 * @return true if the cache is valid, false if a re-render is required.
 */
VER_EXPORT bool ver_render_view_cache_is_valid_for(VerRenderViewHandle view,
                                                   uint64_t sceneHash,
                                                   uint64_t frameTime);

/**
 * @brief Loads a png file.
 * TODO(virtualscene-library): legacy functionality, consider removing.
 *
 * @param filename The filename to be loaded
 * @param outWidth The width of the image.
 * @param outHeight The height of the image.
 * @param outFormatBpp The bit per pixel of the image.
 * @param outBuffer The data of the image.
 * @return true if image loaded successfull, false otherwise.
 */
VER_EXPORT bool ver_texture_utils_load_png(const char* filename,
                                           int* outWidth,
                                           int* outHeight,
                                           int* outFormatBpp,
                                           std::vector<uint8_t>* outBuffer);

ANDROID_BEGIN_HEADER

/**
 * @brief Retrieves the number of webcams available.
 * @return The number of webcams.
 */
VER_EXPORT uint32_t ver_get_webcam_count();

/**
 * @brief Retrieves the webcam info for the webcam at index.
 * The returned handle must be freed with ver_free_webcam_info.
 * @param index The index of the webcam.
 * @return A handle to the webcam info, or nullptr if index is out of bounds.
 */
VER_EXPORT VerWebcamHandle ver_get_webcam_info(uint32_t index);

/**
 * @brief Frees a webcam info handle.
 * @param handle The handle to free.
 */
VER_EXPORT void ver_free_webcam_info(VerWebcamHandle handle);

/**
 * @brief Retrieves the user-facing name of the webcam.
 * The returned string is owned by the handle and valid until the handle is
 * freed.
 * @param handle The webcam info handle.
 * @return The user-facing name of the webcam.
 */
VER_EXPORT const char* ver_webcam_info_get_user_facing_name(
        VerWebcamHandle handle);

/**
 * @brief Retrieves the OS-specific identifier of the webcam.
 * The returned string is owned by the handle and valid until the handle is
 * freed.
 * @param handle The webcam info handle.
 * @return The OS-specific identifier of the webcam.
 */
VER_EXPORT const char* ver_webcam_info_get_id(VerWebcamHandle handle);

/**
 * @brief Retrieves the index of the preferred format for the webcam.
 * @param handle The webcam info handle.
 * @return The index of the preferred format, or -1 if no format is supported.
 */
VER_EXPORT int ver_webcam_info_get_preferred_format_index(
        VerWebcamHandle handle);

/**
 * @brief Retrieves the number of formats supported by the webcam.
 * @param handle The webcam info handle.
 * @return The number of supported formats.
 */
VER_EXPORT uint32_t ver_webcam_info_get_format_count(VerWebcamHandle handle);

/**
 * @brief Retrieves the pixel format (fourcc) for the format at index.
 * @param handle The webcam info handle.
 * @param format_index The index of the format.
 * @return The pixel format fourcc.
 */
VER_EXPORT uint32_t
ver_webcam_info_get_pixel_format_fourcc(VerWebcamHandle handle,
                                        uint32_t format_index);

/**
 * @brief Retrieves the number of resolutions supported by this format.
 * @param handle The webcam info handle.
 * @param format_index The index of the format.
 * @return The number of supported resolutions.
 */
VER_EXPORT uint32_t
ver_webcam_info_get_format_resolution_count(VerWebcamHandle handle,
                                            uint32_t format_index);

/**
 * @brief Retrieves the width and height of the resolution at index for the
 * format at index.
 * @param handle The webcam info handle.
 * @param format_index The index of the format.
 * @param res_index The index of the resolution.
 * @param out_width Pointer to receive the width.
 * @param out_height Pointer to receive the height.
 */
VER_EXPORT bool ver_webcam_info_get_format_resolution(VerWebcamHandle handle,
                                                      uint32_t format_index,
                                                      uint32_t res_index,
                                                      int* out_width,
                                                      int* out_height);

ANDROID_END_HEADER
