#include "options.h"

#include "frontend.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace snaggletooth_libretro {

namespace {

constexpr const char* kInterlaceKey = "snaggletooth_interlace";
constexpr const char* kAspectKey = "snaggletooth_aspect";
constexpr const char* kRegionKey = "snaggletooth_region";
constexpr const char* kAudioThreadKey = "snaggletooth_apu_thread";
constexpr const char* kPictureThreadKey = "snaggletooth_ppu_thread";

retro_core_option_v2_category optionCategories[] = {
    {.key = "video", .desc = "Video", .info = "How the console's picture is shown."},
    {.key = "system",
     .desc = "System",
     .info = "The console the cartridge runs in, and how the host runs it."},
    {},
};

retro_core_option_v2_definition optionDefinitions[] = {
    {.key = kInterlaceKey,
     .desc = "Interlaced Picture",
     .desc_categorized = "Interlaced Picture",
     .info = "How a game that draws an interlaced picture is shown. Weave interleaves its two "
             "fields into the full-height picture the console sent; Bob shows each field at full "
             "height on its own; Off shows each field as it is drawn, at half height.",
     .info_categorized = nullptr,
     .category_key = "video",
     .values = {{"weave", "Weave"}, {"bob", "Bob"}, {"off", "Off"}, {nullptr, nullptr}},
     .default_value = "weave"},
    {.key = kAspectKey,
     .desc = "Aspect Ratio",
     .desc_categorized = "Aspect Ratio",
     .info = "The shape the frontend scales the picture to. Square Pixels shows the console's "
             "pixels square: 8:7 for a 224-line picture, 256:239 for a 239-line one. 4:3 is the "
             "shape of the television the console was made for.",
     .info_categorized = nullptr,
     .category_key = "video",
     .values = {{"square", "Square Pixels"}, {"4:3", "4:3"}, {nullptr, nullptr}},
     .default_value = "square"},
    {.key = kRegionKey,
     .desc = "Region",
     .desc_categorized = "Region",
     .info = "The console the cartridge runs in. Auto follows the region the cartridge's header "
             "names; NTSC runs at 60 Hz and PAL at 50 Hz. Takes effect when a cartridge loads.",
     .info_categorized = nullptr,
     .category_key = "system",
     .values = {{"auto", "Auto"}, {"ntsc", "NTSC (60 Hz)"}, {"pal", "PAL (50 Hz)"},
                {nullptr, nullptr}},
     .default_value = "auto"},
    {.key = kAudioThreadKey,
     .desc = "Audio Thread",
     .desc_categorized = "Audio Thread",
     .info = "Runs the console's sound unit on a thread of its own beside the core's. The sound, "
             "the picture and the game are the same either way; a host whose cores are each too "
             "slow to run the whole console keeps up by sharing the work. Recommended only if "
             "the core cannot keep a consistent 50/60fps even with Picture Thread enabled.",
     .info_categorized = nullptr,
     .category_key = "system",
     .values = {{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}},
     .default_value = "disabled"},
    {.key = kPictureThreadKey,
     .desc = "Picture Thread",
     .desc_categorized = "Picture Thread",
     .info = "Draws the console's picture on a thread of its own beside the core's. The picture, "
             "the sound and the game are the same either way. Enable it if a game cannot hold "
             "50/60fps: a host whose cores are each too slow to run the whole console keeps up "
             "by sharing the work.",
     .info_categorized = nullptr,
     .category_key = "system",
     .values = {{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}},
     .default_value = "disabled"},
    {},
};

// The value the frontend holds for an option, or nullptr where it has none.
const char* valueOf(const char* key) {
  retro_variable variable{.key = key, .value = nullptr};
  if (environment == nullptr || !environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable)) {
    return nullptr;
  }
  return variable.value;
}

// Whether a switch is on: its value where the frontend holds one, otherwise its default.
bool switchedOn(const char* key, bool byDefault) {
  const char* value = valueOf(key);
  if (value == nullptr) return byDefault;
  return std::string_view(value) == "enabled";
}

retro_core_options_v2 coreOptions{.categories = optionCategories,
                                  .definitions = optionDefinitions};

}  // namespace

void registerOptions() {
  unsigned version = 0;
  if (!environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version)) version = 0;
  if (version >= 2u) {
    environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &coreOptions);
    return;
  }
  if (version >= 1u) {
    static std::vector<retro_core_option_definition> plain = [] {
      std::vector<retro_core_option_definition> definitions;
      for (const auto* option = optionDefinitions; option->key != nullptr; ++option) {
        retro_core_option_definition definition{.key = option->key,
                                                .desc = option->desc,
                                                .info = option->info,
                                                .values = {},
                                                .default_value = option->default_value};
        std::copy(std::begin(option->values), std::end(option->values),
                  std::begin(definition.values));
        definitions.push_back(definition);
      }
      definitions.push_back(retro_core_option_definition{});
      return definitions;
    }();
    environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, plain.data());
    return;
  }
  static const std::vector<std::string> lines = [] {
    std::vector<std::string> text;
    for (const auto* option = optionDefinitions; option->key != nullptr; ++option) {
      std::string line = std::string(option->desc) + "; " + option->default_value;
      for (const auto* value = option->values; value->value != nullptr; ++value) {
        if (std::string_view(value->value) == option->default_value) continue;
        line += std::string("|") + value->value;
      }
      text.push_back(std::move(line));
    }
    return text;
  }();
  static std::vector<retro_variable> variables = [] {
    std::vector<retro_variable> list;
    for (std::size_t at = 0; at < lines.size(); ++at) {
      list.push_back(retro_variable{.key = optionDefinitions[at].key, .value = lines[at].c_str()});
    }
    list.push_back(retro_variable{.key = nullptr, .value = nullptr});
    return list;
  }();
  environment(RETRO_ENVIRONMENT_SET_VARIABLES, variables.data());
}

bool optionsChanged() {
  bool changed = false;
  return environment != nullptr &&
         environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) && changed;
}

Interlace interlaceOption() {
  const char* value = valueOf(kInterlaceKey);
  if (value == nullptr) return Interlace::Weave;
  if (std::string_view(value) == "bob") return Interlace::Bob;
  if (std::string_view(value) == "off") return Interlace::Off;
  return Interlace::Weave;
}

Aspect aspectOption() {
  const char* value = valueOf(kAspectKey);
  if (value != nullptr && std::string_view(value) == "4:3") return Aspect::FourByThree;
  return Aspect::SquarePixels;
}

std::optional<snaggletooth::Region> regionOption() {
  const char* value = valueOf(kRegionKey);
  if (value == nullptr) return std::nullopt;
  if (std::string_view(value) == "ntsc") return snaggletooth::Region::Ntsc;
  if (std::string_view(value) == "pal") return snaggletooth::Region::Pal;
  return std::nullopt;
}

MachineThreads threadsOption() {
  return MachineThreads{.audio = switchedOn(kAudioThreadKey, false),
                        .picture = switchedOn(kPictureThreadKey, false)};
}

}  // namespace snaggletooth_libretro
