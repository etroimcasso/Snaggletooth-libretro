#include "controllers.h"

#include "frontend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace snaggletooth_libretro {

namespace {

// The device the frontend put in each port.
std::array<unsigned, kPortCount> portDevices{RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};

// Whether the frontend reports all of a pad's buttons in one call, as a bitmask.
bool inputBitmasks = false;

// The RetroPad button each of the console's twelve stands on. The RetroPad is laid out as the
// console's pad, so each button is the one of the same name in the same place.
struct PadButton {
  unsigned id;
  snaggletooth::Button button;
  const char* name;
};

constexpr std::array<PadButton, snaggletooth::kButtonCount> kPadButtons{{
    {.id = RETRO_DEVICE_ID_JOYPAD_B, .button = snaggletooth::Button::B, .name = "B"},
    {.id = RETRO_DEVICE_ID_JOYPAD_Y, .button = snaggletooth::Button::Y, .name = "Y"},
    {.id = RETRO_DEVICE_ID_JOYPAD_SELECT, .button = snaggletooth::Button::Select, .name = "Select"},
    {.id = RETRO_DEVICE_ID_JOYPAD_START, .button = snaggletooth::Button::Start, .name = "Start"},
    {.id = RETRO_DEVICE_ID_JOYPAD_UP, .button = snaggletooth::Button::Up, .name = "D-Pad Up"},
    {.id = RETRO_DEVICE_ID_JOYPAD_DOWN, .button = snaggletooth::Button::Down, .name = "D-Pad Down"},
    {.id = RETRO_DEVICE_ID_JOYPAD_LEFT, .button = snaggletooth::Button::Left, .name = "D-Pad Left"},
    {.id = RETRO_DEVICE_ID_JOYPAD_RIGHT, .button = snaggletooth::Button::Right,
     .name = "D-Pad Right"},
    {.id = RETRO_DEVICE_ID_JOYPAD_A, .button = snaggletooth::Button::A, .name = "A"},
    {.id = RETRO_DEVICE_ID_JOYPAD_X, .button = snaggletooth::Button::X, .name = "X"},
    {.id = RETRO_DEVICE_ID_JOYPAD_L, .button = snaggletooth::Button::L, .name = "L"},
    {.id = RETRO_DEVICE_ID_JOYPAD_R, .button = snaggletooth::Button::R, .name = "R"},
}};

// What each port offers in the frontend's controller menu: the console's pad, or nothing.
constexpr retro_controller_description kPortTypes[] = {
    {.desc = "SNES Joypad", .id = RETRO_DEVICE_JOYPAD},
    {.desc = "None", .id = RETRO_DEVICE_NONE},
};

constexpr retro_controller_info kControllerInfo[] = {
    {.types = kPortTypes, .num_types = std::size(kPortTypes)},
    {.types = kPortTypes, .num_types = std::size(kPortTypes)},
    {.types = nullptr, .num_types = 0u},
};

// The name of every button on both ports.
const std::array<retro_input_descriptor, kPortCount * snaggletooth::kButtonCount + 1u>
    kInputDescriptors = [] {
      std::array<retro_input_descriptor, kPortCount * snaggletooth::kButtonCount + 1u> list{};
      std::size_t at = 0;
      for (unsigned port = 0; port < kPortCount; ++port) {
        for (const PadButton& pad : kPadButtons) {
          list[at++] = retro_input_descriptor{.port = port,
                                              .device = RETRO_DEVICE_JOYPAD,
                                              .index = 0u,
                                              .id = pad.id,
                                              .description = pad.name};
        }
      }
      return list;
    }();

}  // namespace

void startControllers() {
  portDevices.fill(RETRO_DEVICE_JOYPAD);
  inputBitmasks =
      environment != nullptr && environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
}

void plugIn(unsigned port, unsigned device) {
  if (port < kPortCount) portDevices[port] = device;
}

void describeControllers() {
  if (environment == nullptr) return;
  environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,
              const_cast<retro_controller_info*>(kControllerInfo));
  environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
              const_cast<retro_input_descriptor*>(kInputDescriptors.data()));
}

std::optional<snaggletooth::Joypad> heldOn(unsigned port) {
  if ((portDevices[port] & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return std::nullopt;
  snaggletooth::Joypad pad;
  if (inputState == nullptr) return pad;
  if (inputBitmasks) {
    const auto held = static_cast<std::uint16_t>(
        inputState(port, RETRO_DEVICE_JOYPAD, 0u, RETRO_DEVICE_ID_JOYPAD_MASK));
    for (const PadButton& button : kPadButtons) {
      pad.hold(button.button, ((held >> button.id) & 1u) != 0u);
    }
  } else {
    for (const PadButton& button : kPadButtons) {
      pad.hold(button.button, inputState(port, RETRO_DEVICE_JOYPAD, 0u, button.id) != 0);
    }
  }
  return pad;
}

}  // namespace snaggletooth_libretro
