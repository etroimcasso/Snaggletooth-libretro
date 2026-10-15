#include "session.h"

#include "frontend.h"
#include "sound.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace snaggletooth_libretro {

Session::Session(std::vector<std::uint8_t> image, snaggletooth::Region region,
                 MachineThreads threads)
    : rom(std::move(image)),
      config{.rom = rom,
             .region = region,
             .apuThread = threads.audio,
             .ppuThread = threads.picture},
      machine(std::make_unique<snaggletooth::Snes>(config)) {
  screen.machine = machine.get();
  machine->setFrameObserver(&screen);
}

void Session::runFrame() {
  const std::uint64_t frameCycles =
      snaggletooth::consoleClock(machine->region()).masterCyclesPerFrame;
  try {
    for (;;) machine->run(frameCycles);
  } catch (const FrameFinished&) {
  }
  save.copyFromMachine(*machine);
  workRam.copyFromMachine(*machine);
}

void Session::showPicture() {
  if (screen.width != shownWidth || screen.height != shownHeight || screen.lines != shownLines ||
      aspect != shownAspect) {
    shownWidth = screen.width;
    shownHeight = screen.height;
    shownLines = screen.lines;
    shownAspect = aspect;
    retro_game_geometry geometry{.base_width = screen.width,
                                 .base_height = screen.height,
                                 .max_width = kMaxWidth,
                                 .max_height = kMaxHeight,
                                 .aspect_ratio = aspectOf(screen.lines, aspect)};
    if (environment != nullptr) environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
  }
  if (videoRefresh != nullptr) {
    videoRefresh(screen.pixels.data(), screen.width, screen.height,
                 static_cast<std::size_t>(screen.width) * sizeof(std::uint32_t));
  }
}

void Session::playSound() {
  for (;;) {
    const std::size_t made = machine->takeFrames(sound);
    handOver(std::span<const snaggletooth::StereoFrame>(sound).first(made));
    if (made < sound.size()) break;
  }
}

void Session::runOn(MachineThreads threads) {
  if (threads == MachineThreads{.audio = config.apuThread, .picture = config.ppuThread}) return;
  // A machine's state is a quarter of a megabyte, so the copy is made on the heap. The running
  // machine stops its threads before the new one starts its own.
  const auto state = std::make_unique<snaggletooth::SnesState>(machine->state());
  config.apuThread = threads.audio;
  config.ppuThread = threads.picture;
  machine.reset();
  machine = std::make_unique<snaggletooth::Snes>(config);
  screen.machine = machine.get();
  machine->setFrameObserver(&screen);
  machine->restore(*state);
}

}  // namespace snaggletooth_libretro
