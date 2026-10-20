#include "session.h"

#include "frontend.h"
#include "sound.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

namespace snaggletooth_libretro {

namespace {

// The device a machine on `threads` draws on: `gpu` where the picture is drawn on the GPU.
GpuDevice* drawingOn(MachineThreads threads, GpuDevice* gpu) {
  return threads.picture == PictureDrawing::Gpu ? gpu : nullptr;
}

}  // namespace

Session::Session(std::vector<std::uint8_t> image, snaggletooth::Region region,
                 MachineThreads asked, GpuDevice* gpu)
    : rom(std::move(image)),
      threads(asked),
      device(drawingOn(asked, gpu)),
      config{.rom = rom,
             .region = region,
             .apuThread = asked.audio,
             .ppuThread = asked.picture == PictureDrawing::OwnThread,
             .gpuPpu = device},
      machine(std::make_unique<snaggletooth::Snes>(config)) {
  watch();
}

void Session::watch() {
  screen.machine = machine.get();
  gpuScreen.machine = machine.get();
  gpuScreen.device = device;
  // A frame observer has the processor draw the frame too, so a machine drawing on the GPU is
  // given only the timeline observer.
  if (device != nullptr) {
    machine->setPpuTimelineObserver(&gpuScreen);
  } else {
    machine->setFrameObserver(&screen);
  }
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
  if (picture.width != shownWidth || picture.height != shownHeight ||
      picture.lines != shownLines || aspect != shownAspect) {
    shownWidth = picture.width;
    shownHeight = picture.height;
    shownLines = picture.lines;
    shownAspect = aspect;
    retro_game_geometry geometry{.base_width = picture.width,
                                 .base_height = picture.height,
                                 .max_width = kMaxWidth,
                                 .max_height = kMaxHeight,
                                 .aspect_ratio = aspectOf(picture.lines, aspect)};
    if (environment != nullptr) environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
  }
  if (videoRefresh != nullptr) {
    videoRefresh(picture.pixels.data(), picture.width, picture.height,
                 static_cast<std::size_t>(picture.width) * sizeof(std::uint32_t));
  }
}

void Session::playSound() {
  for (;;) {
    const std::size_t made = machine->takeFrames(sound);
    handOver(std::span<const snaggletooth::StereoFrame>(sound).first(made));
    if (made < sound.size()) break;
  }
}

void Session::runOn(MachineThreads asked, GpuDevice* gpu) {
  GpuDevice* const drawOn = drawingOn(asked, gpu);
  if (asked == threads && drawOn == device) return;
  // A machine's state is a quarter of a megabyte, so the copy is made on the heap. The running
  // machine stops its threads before the new one starts its own.
  const auto state = std::make_unique<snaggletooth::SnesState>(machine->state());
  config.apuThread = asked.audio;
  config.ppuThread = asked.picture == PictureDrawing::OwnThread;
  config.gpuPpu = drawOn;
  machine.reset();
  try {
    machine = std::make_unique<snaggletooth::Snes>(config);
  } catch (const std::runtime_error&) {
    threads = MachineThreads{.audio = asked.audio, .picture = PictureDrawing::CallingThread};
    device = nullptr;
    config.ppuThread = false;
    config.gpuPpu = nullptr;
    machine = std::make_unique<snaggletooth::Snes>(config);
    watch();
    machine->restore(*state);
    throw;
  }
  threads = asked;
  device = drawOn;
  watch();
  machine->restore(*state);
}

bool Session::loadState(std::span<const std::uint8_t> bytes) {
  if (!states.load(*machine, bytes)) return false;
  save.copyFromMachine(*machine);
  workRam.copyFromMachine(*machine);
  return true;
}

}  // namespace snaggletooth_libretro
