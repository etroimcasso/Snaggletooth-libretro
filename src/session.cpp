#include "session.h"

#include "frontend.h"
#include "sound.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace snaggletooth_libretro {

Session::Session(const snaggletooth::SnesConfig& config) : machine(config) {
  screen.machine = &machine;
  machine.setFrameObserver(&screen);
}

void Session::runFrame() {
  const std::uint64_t frameCycles =
      snaggletooth::consoleClock(machine.region()).masterCyclesPerFrame;
  try {
    for (;;) machine.run(frameCycles);
  } catch (const FrameFinished&) {
  }
  save.copyFromMachine(machine);
}

void Session::showPicture() {
  if (screen.width != shownWidth || screen.height != shownHeight || screen.lines != shownLines) {
    shownWidth = screen.width;
    shownHeight = screen.height;
    shownLines = screen.lines;
    retro_game_geometry geometry{.base_width = screen.width,
                                 .base_height = screen.height,
                                 .max_width = kMaxWidth,
                                 .max_height = kMaxHeight,
                                 .aspect_ratio = aspectOf(screen.lines)};
    if (environment != nullptr) environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
  }
  if (videoRefresh != nullptr) {
    videoRefresh(screen.pixels.data(), screen.width, screen.height,
                 static_cast<std::size_t>(screen.width) * sizeof(std::uint32_t));
  }
}

void Session::playSound() {
  for (;;) {
    const std::size_t made = machine.takeFrames(sound);
    handOver(std::span<const snaggletooth::StereoFrame>(sound).first(made));
    if (made < sound.size()) break;
  }
}

}  // namespace snaggletooth_libretro
