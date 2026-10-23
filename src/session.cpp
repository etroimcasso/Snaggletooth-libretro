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

// The frontend's device where the picture is drawn on the GPU and the frontend has one; the
// core's own device is then not drawn on.
HardwareDevice* drawingOn(MachineThreads threads, HardwareDevice* hardware) {
  return threads.picture == PictureDrawing::Gpu ? hardware : nullptr;
}

snaggletooth::gpu::FieldPlacement placementOf(Interlace mode) {
  switch (mode) {
    case Interlace::Weave: return snaggletooth::gpu::FieldPlacement::Weave;
    case Interlace::Bob: return snaggletooth::gpu::FieldPlacement::Bob;
    case Interlace::Off: break;
  }
  return snaggletooth::gpu::FieldPlacement::Field;
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
  gpuScreen.hardware = hardware;
  // A frame observer has the processor draw the frame too, so a machine drawing on the GPU is
  // given only the timeline observer.
  if (device != nullptr || hardware != nullptr) {
    machine->setPpuTimelineObserver(&gpuScreen);
  } else {
    machine->setFrameObserver(&screen);
  }
}

void Session::runFrame() {
  if (hardware != nullptr) hardware->place(placementOf(picture.mode));
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
  if (videoRefresh == nullptr) return;
  if (hardwareContext) {
    const bool shown =
        presenter != nullptr &&
        (hardware != nullptr ? presenter->present(picture.width, picture.height)
                             : presenter->presentPixels(picture.pixels.data(), picture.width,
                                                        picture.height, picture.width));
    videoRefresh(shown ? RETRO_HW_FRAME_BUFFER_VALID : nullptr, picture.width, picture.height, 0u);
    return;
  }
  videoRefresh(picture.pixels.data(), picture.width, picture.height,
               static_cast<std::size_t>(picture.width) * sizeof(std::uint32_t));
}

void Session::playSound() {
  for (;;) {
    const std::size_t made = machine->takeFrames(sound);
    handOver(std::span<const snaggletooth::StereoFrame>(sound).first(made));
    if (made < sound.size()) break;
  }
}

bool Session::rebuildsFor(MachineThreads asked, GpuDevice* gpu, HardwareDevice* hardwareAsked) const {
  HardwareDevice* const onHardware = drawingOn(asked, hardwareAsked);
  GpuDevice* const onGpu = onHardware != nullptr ? nullptr : drawingOn(asked, gpu);
  return asked != threads || onGpu != device || onHardware != hardware;
}

void Session::runOn(MachineThreads asked, GpuDevice* gpu, HardwareDevice* hardwareAsked) {
  if (!rebuildsFor(asked, gpu, hardwareAsked)) return;
  HardwareDevice* const onHardware = drawingOn(asked, hardwareAsked);
  GpuDevice* const onGpu = onHardware != nullptr ? nullptr : drawingOn(asked, gpu);
  // The picture drawn so far, in the frontend's device's texture while the machine draws there,
  // comes out to the processor's picture when the drawing moves away.
  if (hardware != nullptr && onHardware != hardware) {
    (void)hardware->readBack(picture.width, picture.height, picture.pixels.data(), picture.width);
  }
  HardwareDevice* const drewOn = hardware;
  // A machine's state is a quarter of a megabyte, so the copy is made on the heap. The running
  // machine stops its threads before the new one starts its own.
  const auto state = std::make_unique<snaggletooth::SnesState>(machine->state());
  config.apuThread = asked.audio;
  config.ppuThread = asked.picture == PictureDrawing::OwnThread;
  config.gpuPpu =
      onHardware != nullptr ? static_cast<snaggletooth::gpu::Device*>(onHardware) : onGpu;
  machine.reset();
  try {
    machine = std::make_unique<snaggletooth::Snes>(config);
  } catch (const std::runtime_error&) {
    threads = MachineThreads{.audio = asked.audio, .picture = PictureDrawing::CallingThread};
    device = nullptr;
    hardware = nullptr;
    config.ppuThread = false;
    config.gpuPpu = nullptr;
    machine = std::make_unique<snaggletooth::Snes>(config);
    watch();
    machine->restore(*state);
    throw;
  }
  threads = asked;
  device = onGpu;
  hardware = onHardware;
  // The picture drawn so far goes into the texture when the drawing moves onto the frontend's
  // device, so a woven picture carries on from the other field.
  if (hardware != nullptr && hardware != drewOn) {
    (void)hardware->seed(picture.pixels.data(), picture.width, picture.height, picture.width);
  }
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
