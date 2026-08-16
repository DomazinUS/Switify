#pragma once

#include <cstdint>
#include <string>

namespace switify::artwork {

// Keeps network and JPEG work off Ultrahand's render thread. Only the image
// being displayed and one pending replacement can exist at once.
bool initialize();
void shutdown();
void request(const std::string& url, int targetSize);
void borrow(const uint8_t** pixels, int* width, int* height);

}  // namespace switify::artwork
