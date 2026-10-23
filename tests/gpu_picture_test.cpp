// The picture drawn on the GPU, the Picture Thread option's default, by a frontend that gives no
// Vulkan context: every frame the core hands over is the frame the machine draws on the
// processor, at every shape and under every interlace option; a state saved while drawing on the
// processor plays on under the GPU; where no GPU device opens, the core says so and draws on the
// processor; and on macOS the core stays loaded when a frontend closes it, so the methods SDL
// adds to Apple's classes stay where they point.
//
// What each frame should hold comes from a second machine run here on the processor, with an
// observer of the suite's own (reference_pictures.h). A case that needs a device skips, saying
// why, on a computer where none opens: the core's message names SDL's reason.

#include <libretro.h>

#include "loaded_core.h"
#include "reference_pictures.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

using snaggletooth_libretro_test::expectedFor;
using snaggletooth_libretro_test::expectPictures;
using snaggletooth_libretro_test::kColourBits;
using snaggletooth_libretro_test::kInterlaced;
using snaggletooth_libretro_test::kModeFive;
using snaggletooth_libretro_test::kModeZero;
using snaggletooth_libretro_test::kProgressive;
using snaggletooth_libretro_test::kTaller;
using snaggletooth_libretro_test::LoadedCore;
using snaggletooth_libretro_test::Mode;
using snaggletooth_libretro_test::Picture;
using snaggletooth_libretro_test::pictureCartridge;

constexpr const char* kInterlaceKey = "snaggletooth_interlace";
constexpr const char* kPictureThreadKey = "snaggletooth_ppu_thread";

// What the core's message begins with where it cannot draw on the GPU.
constexpr const char* kNoGpu = "Snaggletooth cannot draw on the GPU here";

// What the test frontend saw the core do, and the option values it answers the core with.
struct Frontend {
  std::map<std::string, std::string> values;  // the options the player set
  std::vector<std::string> messages;          // every SET_MESSAGE and SET_MESSAGE_EXT
  std::vector<Picture> pictures;

  // The core's message that it cannot draw on the GPU, or an empty string.
  [[nodiscard]] std::string noGpu() const {
    for (const std::string& message : messages) {
      if (message.rfind(kNoGpu, 0) == 0) return message;
    }
    return {};
  }
};

Frontend* frontend = nullptr;

bool environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* variable = static_cast<retro_variable*>(data);
      const auto value = frontend->values.find(variable->key);
      if (value == frontend->values.end()) return false;
      variable->value = value->second.c_str();
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = false;
      return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *static_cast<unsigned*>(data) = 1u;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
      frontend->messages.emplace_back(static_cast<const retro_message*>(data)->msg);
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      frontend->messages.emplace_back(static_cast<const retro_message_ext*>(data)->msg);
      return true;
    default:
      return false;
  }
}

void videoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
  Picture picture{.width = width, .height = height,
                  .pixels = std::vector<std::uint32_t>(std::size_t{width} * height)};
  if (data != nullptr) {
    for (unsigned row = 0; row < height; ++row) {
      std::memcpy(picture.pixels.data() + std::size_t{row} * width,
                  static_cast<const std::uint8_t*>(data) + row * pitch, std::size_t{width} * 4u);
    }
  }
  for (std::uint32_t& pixel : picture.pixels) pixel &= kColourBits;
  frontend->pictures.push_back(std::move(picture));
}

void audioSample(std::int16_t left, std::int16_t right) {
  (void)left;
  (void)right;
}

size_t audioSampleBatch(const std::int16_t* data, size_t frames) {
  (void)data;
  return frames;
}

void inputPoll() {}

std::int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
  (void)port;
  (void)device;
  (void)index;
  (void)id;
  return 0;
}

// The core loaded and set up as a frontend sets one up: the callbacks handed over, then
// retro_init. Deinitialized when the case ends.
class Core {
 public:
  explicit Core(Frontend& seen) {
    frontend = &seen;
    if (!module_.loaded()) return;
    module_.entry<void(retro_environment_t)>("retro_set_environment")(environment);
    module_.entry<void(retro_video_refresh_t)>("retro_set_video_refresh")(videoRefresh);
    module_.entry<void(retro_audio_sample_t)>("retro_set_audio_sample")(audioSample);
    module_.entry<void(retro_audio_sample_batch_t)>("retro_set_audio_sample_batch")(
        audioSampleBatch);
    module_.entry<void(retro_input_poll_t)>("retro_set_input_poll")(inputPoll);
    module_.entry<void(retro_input_state_t)>("retro_set_input_state")(inputState);
    module_.entry<void()>("retro_init")();
  }
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  ~Core() {
    if (module_.loaded()) module_.entry<void()>("retro_deinit")();
    frontend = nullptr;
  }

  [[nodiscard]] bool loaded() const noexcept { return module_.loaded(); }

  bool load(const std::vector<std::uint8_t>& image) {
    const retro_game_info game{.path = nullptr, .data = image.data(), .size = image.size(),
                               .meta = nullptr};
    return module_.entry<bool(const retro_game_info*)>("retro_load_game")(&game);
  }
  void run(std::size_t times = 1u) {
    for (std::size_t at = 0; at < times; ++at) module_.entry<void()>("retro_run")();
  }
  [[nodiscard]] std::vector<std::uint8_t> serialize() const {
    std::vector<std::uint8_t> state(module_.entry<size_t()>("retro_serialize_size")());
    if (!module_.entry<bool(void*, size_t)>("retro_serialize")(state.data(), state.size())) {
      state.clear();
    }
    return state;
  }
  bool unserialize(const std::vector<std::uint8_t>& state) {
    return module_.entry<bool(const void*, size_t)>("retro_unserialize")(state.data(),
                                                                         state.size());
  }

 private:
  LoadedCore module_;
};

// Runs a cartridge `runs` frames under the default Picture Thread, the GPU, with the interlace
// option set where `interlace` names a value, and holds every picture to the processor's.
// Skips, saying why, where the core reports that it cannot draw on the GPU.
void expectGpuPictures(const std::vector<std::uint8_t>& rom, std::size_t runs, Mode mode,
                       const char* interlace = nullptr) {
  Frontend seen;
  if (interlace != nullptr) seen.values[kInterlaceKey] = interlace;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  if (const std::string reason = seen.noGpu(); !reason.empty()) {
    GTEST_SKIP() << "no GPU device here: " << reason;
  }
  core.run(runs);
  ASSERT_TRUE(seen.noGpu().empty());
  expectPictures(seen.pictures, expectedFor(rom, runs, mode));
}

TEST(GpuPicture, GpuFramesMatchTheProcessorsFrames) {
  expectGpuPictures(pictureCartridge(kModeZero, kProgressive), 8u, Mode::Weave);
}

TEST(GpuPicture, GpuHalfPixelFramesMatch) {
  expectGpuPictures(pictureCartridge(kModeFive, kProgressive), 6u, Mode::Weave);
}

TEST(GpuPicture, GpuTallerPictureMatches) {
  expectGpuPictures(pictureCartridge(kModeZero, kTaller), 6u, Mode::Weave);
}

TEST(GpuPicture, GpuWeaveMatches) {
  expectGpuPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Weave, "weave");
}

TEST(GpuPicture, GpuBobMatches) {
  expectGpuPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Bob, "bob");
}

TEST(GpuPicture, GpuOffMatches) {
  expectGpuPictures(pictureCartridge(kModeFive, kInterlaced), 12u, Mode::Off, "off");
}

TEST(GpuPicture, AStateSavedOnTheProcessorLoadsUnderTheGpu) {
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kInterlaced);
  std::vector<std::uint8_t> state;
  std::vector<Picture> followed;
  {
    Frontend seen;
    seen.values[kPictureThreadKey] = "disabled";
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(rom));
    core.run(4u);
    state = core.serialize();
    ASSERT_FALSE(state.empty());
    seen.pictures.clear();
    core.run(8u);
    followed = seen.pictures;
  }
  Frontend seen;
  Core core(seen);
  ASSERT_TRUE(core.loaded());
  ASSERT_TRUE(core.load(rom));
  if (const std::string reason = seen.noGpu(); !reason.empty()) {
    GTEST_SKIP() << "no GPU device here: " << reason;
  }
  // A frame is shown before the state loads. The first picture after the load keeps that frame's
  // field in its other rows, as a woven picture does on the processor too; every later picture
  // is two fields drawn after the load.
  core.run();
  ASSERT_TRUE(core.unserialize(state));
  seen.pictures.clear();
  core.run(8u);
  ASSERT_EQ(seen.pictures.size(), followed.size());
  for (std::size_t at = 1; at < followed.size(); ++at) {
    EXPECT_TRUE(seen.pictures[at] == followed[at]) << "frame " << at << " after the load";
  }
}

// A frontend opens the core to read its system information and closes it before it opens the core
// to run a cartridge, and RetroArch's video setup then messages NSCursor. SDL's Cocoa code adds
// methods to NSCursor and three other Apple classes as soon as the module is mapped; the core
// stays mapped through the close, so a message to NSCursor still finds them. Its symbols stay
// its own: a frontend that finds retro_init across its whole process refuses to load a core.
TEST(GpuPicture, TheCoreStaysLoadedWhenAFrontendClosesIt) {
#ifdef __APPLE__
  const void* inModule = nullptr;
  {
    LoadedCore module;
    ASSERT_TRUE(module.loaded());
    retro_system_info info{};
    module.entry<void(retro_system_info*)>("retro_get_system_info")(&info);
    inModule = info.library_name;
  }
  Dl_info where{};
  EXPECT_NE(dladdr(inModule, &where), 0) << "the module was unloaded when the frontend closed it";
  EXPECT_EQ(dlsym(RTLD_DEFAULT, "retro_init"), nullptr)
      << "the core's entry points are visible across the process";
  Class cursor = objc_getClass("NSCursor");
  ASSERT_NE(cursor, nullptr);
  reinterpret_cast<void (*)(id, SEL)>(objc_msgSend)(reinterpret_cast<id>(cursor),
                                                    sel_registerName("unhide"));
#else
  GTEST_SKIP() << "the core keeps itself loaded on macOS alone, where SDL's Cocoa code adds "
                  "methods to Apple's classes";
#endif
}

TEST(GpuPicture, NoGpuDeviceDrawsOnTheProcessorAndSaysSo) {
  // SDL reads its hints from the environment, and no GPU driver is named "none", so no device
  // opens whatever the computer has.
#ifdef _WIN32
  _putenv_s("SDL_GPU_DRIVER", "none");
#else
  setenv("SDL_GPU_DRIVER", "none", 1);
#endif
  const std::vector<std::uint8_t> rom = pictureCartridge(kModeZero, kProgressive);
  {
    Frontend seen;
    Core core(seen);
    ASSERT_TRUE(core.loaded());
    ASSERT_TRUE(core.load(rom));
    core.run(8u);
    EXPECT_EQ(seen.messages.size(), 1u);
    EXPECT_FALSE(seen.noGpu().empty());
    expectPictures(seen.pictures, expectedFor(rom, 8u, Mode::Weave));
  }
#ifdef _WIN32
  _putenv_s("SDL_GPU_DRIVER", "");
#else
  unsetenv("SDL_GPU_DRIVER");
#endif
}

}  // namespace
