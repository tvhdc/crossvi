#pragma once

#include <string>

namespace LegacySettingsTestSupport {

void resetAtomicJson();
void failNextJsonSave();
int jsonSaveCalls();
const std::string& lastSavedJson();

}  // namespace LegacySettingsTestSupport
