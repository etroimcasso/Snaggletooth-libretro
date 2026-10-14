#include "frontend.h"

namespace snaggletooth_libretro {

namespace {

// How long a message the frontend shows on screen stays there.
constexpr unsigned kMessageMilliseconds = 6000u;
constexpr unsigned kMessageFrames = 360u;

}  // namespace

void tellPlayer(const std::string& text) {
  if (logPrintf != nullptr) logPrintf(RETRO_LOG_ERROR, "%s\n", text.c_str());
  if (environment == nullptr) return;
  unsigned version = 0;
  if (!environment(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version)) version = 0;
  if (version >= 1u) {
    retro_message_ext message{.msg = text.c_str(),
                              .duration = kMessageMilliseconds,
                              .priority = 3u,
                              .level = RETRO_LOG_ERROR,
                              .target = RETRO_MESSAGE_TARGET_ALL,
                              .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
                              .progress = -1};
    environment(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
  } else {
    retro_message message{.msg = text.c_str(), .frames = kMessageFrames};
    environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
  }
}

}  // namespace snaggletooth_libretro
