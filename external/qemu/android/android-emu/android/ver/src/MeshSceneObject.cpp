// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "MeshSceneObject.h"

#define E(...) derror(__VA_ARGS__)

namespace android {
namespace ver {

MeshSceneObject::MeshSceneObject(Renderer& renderer) : SceneObject(renderer) {}

std::unique_ptr<MeshSceneObject> MeshSceneObject::load(Renderer& renderer,
                                                       const char* filename) {
    (void)renderer;
    E("%s: external OBJ scene loading is disabled in AEMU core-only (%s)",
      __FUNCTION__, filename ? filename : "<null>");
    return nullptr;
}

// Must match MeshSceneObject::load for the error returning paths
bool MeshSceneObject::canLoad(const char* filename) {
    (void)filename;
    return false;
}

std::unique_ptr<MeshSceneObject> MeshSceneObject::createSphere(
        Renderer& renderer) {
    // Number of segments horizontally and vertically
    const int segments = 64;

    // Generate vertices
    std::vector<VertexPositionUV> vertices;
    vertices.reserve((segments + 1) * (segments + 1));
    for (int i = 0; i <= segments; ++i) {
        float v_coord = (float)i / segments;
        float phi = v_coord * M_PI;

        for (int j = 0; j <= segments; ++j) {
            float u_coord = (float)j / segments;
            float theta = u_coord * 2.0f * M_PI;  // Longitude

            VertexPositionUV v;
            v.pos.x = std::sin(phi) * std::cos(theta);
            v.pos.y = std::cos(phi);
            v.pos.z = std::sin(phi) * std::sin(theta);
            v.uv.x = u_coord;
            v.uv.y = 1.0f - v_coord;

            vertices.push_back(v);
        }
    }

    // Generate faces
    std::vector<GLuint> indices;
    indices.reserve(segments * segments * 2);
    for (int i = 0; i < segments; ++i) {
        int row1 = i * (segments + 1);
        int row2 = (i + 1) * (segments + 1);

        for (int j = 0; j < segments; ++j) {
            int p1 = row1 + j;
            int p2 = p1 + 1;
            int p3 = row2 + j;
            int p4 = p3 + 1;

            // two triangles per segment
            indices.push_back(p1);
            indices.push_back(p3);
            indices.push_back(p2);

            indices.push_back(p2);
            indices.push_back(p3);
            indices.push_back(p4);
        }
    }

    // Generate renderable
    Renderable renderable;
    renderable.material = renderer.createMaterialTextured();
    renderable.mesh = renderer.createMesh(vertices, indices);

    // Generate scene object
    std::unique_ptr<MeshSceneObject> result(new MeshSceneObject(renderer));
    result->mRenderables.emplace_back(std::move(renderable));

    return result;
}

}  // namespace ver
}  // namespace android
