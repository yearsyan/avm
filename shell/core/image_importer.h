// SPDX-License-Identifier: MIT

#ifndef MACMU_IMAGE_IMPORTER_H
#define MACMU_IMAGE_IMPORTER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "shell_options.h"

enum class ImageImportPhase {
  kAcquiringObjects,
  kReconstructingImage,
};

struct ImageImportProgress {
  ImageImportPhase phase = ImageImportPhase::kAcquiringObjects;
  uint64_t completedBytes = 0;
  uint64_t totalBytes = 0;
  size_t completedItems = 0;
  size_t totalItems = 0;
  bool network = false;
};

using ImageImportProgressCallback =
    std::function<void(const ImageImportProgress &progress)>;

// Extracts either a complete MacMu image ZIP or a v2 chunk manifest into
// |destination_root|. Chunk objects can be local siblings of the manifest or
// HTTPS resources. Remote objects are resumed and cached under appDataDir.
bool macmu_extract_system_image_source(
    const ShellOptions &options, const std::string &source,
    const std::string &destination_root,
    const ImageImportProgressCallback &progress, std::string *error);

// Stops curl/ditto/unzip child processes owned by an in-progress import. This
// is called during application termination so downloads are not orphaned.
void macmu_cancel_system_image_import();

#endif // MACMU_IMAGE_IMPORTER_H
