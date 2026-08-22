#pragma once

#include <cstddef>

class GfxRenderer;
class HalDisplay;

namespace SleepFrameStore {

void discard();
bool save(const GfxRenderer& renderer);
bool ready(bool deviceIsX3);
bool load(HalDisplay& display, bool consume = true);

}  // namespace SleepFrameStore
