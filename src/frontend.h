// The frontend as the core reaches it: the callbacks it hands over, and the way the core tells
// the player something.

#pragma once

#include <libretro.h>

#include <string>

namespace snaggletooth_libretro {

// The frontend's callbacks, as it hands them over. Each is absent until the frontend sets it.
inline retro_environment_t environment = nullptr;
inline retro_video_refresh_t videoRefresh = nullptr;
inline retro_audio_sample_t audioSample = nullptr;
inline retro_audio_sample_batch_t audioSampleBatch = nullptr;
inline retro_input_poll_t inputPoll = nullptr;
inline retro_input_state_t inputState = nullptr;
inline retro_log_printf_t logPrintf = nullptr;

// Tells the player something they need to know, the way the frontend shows messages, and
// writes it to the frontend's log as well. A frontend that offers only the first message
// interface is handed the message that way.
void tellPlayer(const std::string& text);

}  // namespace snaggletooth_libretro
