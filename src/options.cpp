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

retro_core_option_v2_category optionCategories[] = {
    {.key = "video", .desc = "Video", .info = "How the console's picture is shown."},
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
    {},
};

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
  retro_variable variable{.key = kInterlaceKey, .value = nullptr};
  if (environment == nullptr || !environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ||
      variable.value == nullptr) {
    return Interlace::Weave;
  }
  const std::string_view value = variable.value;
  if (value == "bob") return Interlace::Bob;
  if (value == "off") return Interlace::Off;
  return Interlace::Weave;
}

}  // namespace snaggletooth_libretro
