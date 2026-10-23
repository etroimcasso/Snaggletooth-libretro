// A loaded cartridge and everything that runs it: the machine, the picture it draws into and
// where it draws it, the sound it is drained into, the save and work RAM the frontend keeps with the map that places
// them on the bus, the machine's saved states, and the picture's shape as the frontend was last
// told it.

#pragma once

#include "battery_save.h"
#include "gpu_device.h"
#include "hardware_device.h"
#include "memory_map.h"
#include "picture.h"
#include "save_state.h"
#include "work_ram.h"

#include "snaggletooth/snes/snes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace snaggletooth_libretro {

// Where the machine draws the console's picture: on the thread the frontend calls the core on,
// on a thread of its own beside it, or on the GPU.
enum class PictureDrawing : std::uint8_t { CallingThread, OwnThread, Gpu };

// Where the machine runs its sound unit's work, on the frontend's thread or on a thread of its
// own beside it, and where it draws its picture. The sound, the picture and the state are the
// same every way.
struct MachineThreads {
  bool audio = false;
  PictureDrawing picture = PictureDrawing::Gpu;

  [[nodiscard]] bool operator==(const MachineThreads&) const noexcept = default;
};

// The machine keeps a pointer to the observer and the session's buffers are handed to the
// frontend, so a session is built in place and never moved.
struct Session {
  // The machine is built on the threads asked for, drawing on `gpu` where the picture is drawn
  // on the GPU; `gpu` is ignored otherwise. A machine that cannot draw on the device throws
  // std::runtime_error from its constructor, and so does the session.
  Session(std::vector<std::uint8_t> image, snaggletooth::Region region, MachineThreads asked,
          GpuDevice* gpu);
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Runs the machine until the frame's vertical blank begins, where the machine reports the
  // finished picture, then copies the machine's save and work RAM into the frontend's buffers.
  void runFrame();

  // Hands the frontend the picture the last frame made, telling it the new geometry first when
  // the picture's shape or aspect has changed. Under the frontend's hardware context the picture
  // is handed over as a texture on the frontend's device: the one the machine drew into, or one
  // the picture drawn on the processor is copied into; with no context ready, the frontend is
  // told to show the last frame again.
  void showPicture();

  // Hands the frontend the sound the last frame made, every sample from the console's time up
  // to the frame's end.
  void playSound();

  // Moves the machine's work onto the threads asked for, drawing where the picture is drawn on
  // the GPU on `hardware`, the frontend's device, when there is one, and on `gpu`, the core's
  // own, otherwise. The machine takes its threads and its device when it is built, so a change
  // builds a new machine on the same cartridge and restores the running one's state into it: the
  // game carries on from the cycle it stopped at, with the same pads, save, work RAM and picture,
  // the picture carried into or out of the frontend's device's texture when the drawing moves
  // there or away. A machine that cannot draw on the device is built drawing on the frontend's
  // thread instead, the game carried on there, and the machine's std::runtime_error is thrown on.
  void runOn(MachineThreads asked, GpuDevice* gpu, HardwareDevice* hardware = nullptr);

  // Whether runOn() with these arguments builds a new machine.
  [[nodiscard]] bool rebuildsFor(MachineThreads asked, GpuDevice* gpu,
                                 HardwareDevice* hardware = nullptr) const;

  // Puts a saved state into the machine (SaveState::load), then copies the machine's save and
  // work RAM into the frontend's buffers, so they hold the state's and the next frame carries
  // no change of the frontend's into the machine. Answers false, with the machine and both
  // buffers as they were, when the state is refused.
  bool loadState(std::span<const std::uint8_t> bytes);

  // The cartridge image the machine was built from, the configuration it was built with, where
  // it runs its work, and the device it draws on, if it draws on the GPU: the core's own, or the
  // frontend's.
  std::vector<std::uint8_t> rom;
  MachineThreads threads;
  GpuDevice* device = nullptr;
  HardwareDevice* hardware = nullptr;
  snaggletooth::SnesConfig config;
  // Under the frontend's hardware context: true from the load, and the device frames are handed
  // over through while the context is ready.
  bool hardwareContext = false;
  HardwareDevice* presenter = nullptr;
  // The picture the frontend is handed, and the observer that fills it: Screen on the
  // processor, GpuScreen on the GPU.
  Picture picture;
  Screen screen{picture};
  GpuScreen gpuScreen{picture};
  std::unique_ptr<snaggletooth::Snes> machine;
  BatterySave save{*machine};
  WorkRam workRam{*machine};
  MemoryMap memoryMap{*machine, workRam.data(), save.data()};
  SaveState states{*machine};
  // Where the machine's sound is drained to each frame, in as many passes as it takes. A frame
  // makes about 533 samples at 60 Hz and 640 at 50 Hz, so one pass is the usual case.
  std::array<snaggletooth::StereoFrame, 1024> sound{};
  // The aspect the player chose, and the geometry the frontend was last told.
  Aspect aspect = Aspect::SquarePixels;
  unsigned shownWidth = kBaseWidth;
  unsigned shownHeight = kBaseHeight;
  unsigned shownLines = kBaseHeight;
  Aspect shownAspect = Aspect::SquarePixels;

 private:
  // The observer the machine's drawing calls for set on it.
  void watch();
};

}  // namespace snaggletooth_libretro
