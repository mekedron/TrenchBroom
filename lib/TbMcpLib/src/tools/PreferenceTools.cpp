/*
 Copyright (C) 2026 Nikita Rabykin

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mcp/tools/PreferenceTools.h"

#include "ToolUtils.h"
#include "base/PreferenceManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/Pagination.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/GameTools.h"
#include "mcp/tools/PreferenceCatalog.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace tb::mcp
{
namespace
{
using namespace schema;

// JSON forms

Json floatJson(const float value)
{
  // floats such as 0.4f are reported as 0.4, not as 0.4000000059604645
  return std::round(double(value) * 1e6) / 1e6;
}

int colorByte(const float value)
{
  return int(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

/** "#RRGGBB" for opaque colors, "#RRGGBBAA" otherwise. */
std::string colorHex(const Color& color)
{
  const auto v = color.to<RgbaF>().toVec();
  const auto r = colorByte(v[0]);
  const auto g = colorByte(v[1]);
  const auto b = colorByte(v[2]);
  const auto a = colorByte(v[3]);
  return a == 255 ? fmt::format("#{:02X}{:02X}{:02X}", r, g, b)
                  : fmt::format("#{:02X}{:02X}{:02X}{:02X}", r, g, b, a);
}

std::vector<std::string> shortcutTexts(const std::vector<KeySequence>& keySequences)
{
  auto result = std::vector<std::string>{};
  for (const auto& keySequence : keySequences)
  {
    if (!keySequence.value.empty())
    {
      result.push_back(keySequence.value);
    }
  }
  return result;
}

Json valueJson(const bool value)
{
  return value;
}

Json valueJson(const int value)
{
  return value;
}

Json valueJson(const float value)
{
  return floatJson(value);
}

Json valueJson(const std::string& value)
{
  return value;
}

Json valueJson(const std::filesystem::path& value)
{
  return value.generic_string();
}

Json valueJson(const Color& value)
{
  return colorHex(value);
}

Json valueJson(const std::vector<KeySequence>& value)
{
  return shortcutTexts(value);
}

Json currentValueJson(const AnyPreference& preference)
{
  return std::visit([](const auto* p) { return valueJson(pref(*p)); }, preference);
}

Json defaultValueJson(const AnyPreference& preference)
{
  return std::visit([](const auto* p) { return valueJson(p->defaultValue); }, preference);
}

std::string_view persistenceName(const PreferencePersistencePolicy policy)
{
  switch (policy)
  {
  case PreferencePersistencePolicy::Persistent:
    return "persistent";
  case PreferencePersistencePolicy::Transient:
    return "transient";
  case PreferencePersistencePolicy::ReadOnly:
    return "readOnly";
  }
  return "persistent";
}

std::string_view sourceName(const PreferenceSource source)
{
  switch (source)
  {
  case PreferenceSource::Editor:
    return "editor";
  case PreferenceSource::Game:
    return "game";
  case PreferenceSource::Host:
    return "host";
  }
  return "editor";
}

std::string pathString(const PreferenceInfo& info)
{
  return info.path().generic_string();
}

Json itemJson(const PreferenceInfo& info, const Detail detail)
{
  const auto value = currentValueJson(info.preference);
  const auto defaultValue = defaultValueJson(info.preference);

  auto result = Json{
    {"path", pathString(info)},
    {"type", preferenceTypeName(info.preference)},
    {"value", info.secret ? Json(nullptr) : value},
    {"modified", value != defaultValue},
    {"category", info.category},
    {"description", info.description},
  };
  if (!info.allowedValues.empty())
  {
    result["allowedValues"] = info.allowedValues;
  }
  if (info.minimum)
  {
    result["minimum"] = *info.minimum;
  }
  if (info.maximum)
  {
    result["maximum"] = *info.maximum;
  }
  if (info.secret)
  {
    result["secret"] = true;
  }

  if (detail == Detail::Full)
  {
    result["default"] = info.secret ? Json(nullptr) : defaultValue;
    result["persistence"] = persistenceName(preferencePersistence(info.preference));
    result["source"] = sourceName(info.source);
    if (!info.note.empty())
    {
      result["note"] = info.note;
    }
    if (!info.lockedReason.empty())
    {
      result["lockedReason"] = info.lockedReason;
    }
  }
  return result;
}

// Lookup

/** The Levenshtein distance of two strings. */
size_t editDistance(const std::string_view a, const std::string_view b)
{
  auto previous = std::vector<size_t>(b.size() + 1);
  auto current = std::vector<size_t>(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
  {
    previous[j] = j;
  }
  for (size_t i = 1; i <= a.size(); ++i)
  {
    current[0] = i;
    for (size_t j = 1; j <= b.size(); ++j)
    {
      const auto substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }
    std::swap(previous, current);
  }
  return previous[b.size()];
}

/** Up to three known paths that resemble the given one. */
std::vector<std::string> closestPaths(
  const std::vector<PreferenceInfo>& preferences, const std::string_view path)
{
  const auto lowerPath = kdl::str_to_lower(path);
  const auto lastSlash = lowerPath.rfind('/');
  const auto lowerName =
    lastSlash == std::string::npos ? lowerPath : lowerPath.substr(lastSlash + 1);

  auto scored = std::vector<std::pair<size_t, std::string>>{};
  for (const auto& info : preferences)
  {
    const auto candidate = pathString(info);
    const auto lowerCandidate = kdl::str_to_lower(candidate);
    auto score = editDistance(lowerPath, lowerCandidate);
    if (!lowerName.empty() && lowerCandidate.find(lowerName) != std::string::npos)
    {
      score = std::min(score, lowerCandidate.size() - lowerName.size()) / 4;
    }
    scored.emplace_back(score, candidate);
  }
  std::ranges::stable_sort(
    scored, [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  auto result = std::vector<std::string>{};
  for (size_t i = 0; i < scored.size() && i < 3; ++i)
  {
    result.push_back(scored[i].second);
  }
  return result;
}

/** Finds a preference by path; an exact match wins over a case-insensitive one. */
const PreferenceInfo* findPreference(
  const std::vector<PreferenceInfo>& preferences, const std::string_view path)
{
  const PreferenceInfo* caseInsensitiveMatch = nullptr;
  for (const auto& info : preferences)
  {
    const auto candidate = pathString(info);
    if (candidate == path)
    {
      return &info;
    }
    if (!caseInsensitiveMatch && kdl::ci::str_is_equal(candidate, path))
    {
      caseInsensitiveMatch = &info;
    }
  }
  return caseInsensitiveMatch;
}

ToolError unknownPathError(
  const std::vector<PreferenceInfo>& preferences, const std::vector<std::string>& paths)
{
  auto suggestions = std::vector<std::string>{};
  for (const auto& path : paths)
  {
    for (auto& suggestion : closestPaths(preferences, path))
    {
      if (std::ranges::find(suggestions, suggestion) == suggestions.end())
      {
        suggestions.push_back(std::move(suggestion));
      }
    }
  }
  return makeError(
    ErrorCode::ObjectNotFound,
    fmt::format(
      "Unknown preference path{}: {}.",
      paths.size() == 1 ? "" : "s",
      fmt::join(paths, ", ")),
    fmt::format(
      "Did you mean {}? preferences_get with 'query' or 'prefix' lists the preferences.",
      fmt::join(suggestions, ", ")));
}

std::vector<std::string> stringOrStrings(const Args& args, const std::string_view key)
{
  const auto value = args.getOptional<Json>(key);
  if (!value)
  {
    return {};
  }
  if (value->is_string())
  {
    return {value->get<std::string>()};
  }
  return value->get<std::vector<std::string>>();
}

// preferences_get

ToolResult preferencesGet(CallContext& context, const Args& args)
{
  auto request = pageRequest(args, 0);
  if (request.is_error())
  {
    return errorOf(request);
  }

  const auto preferences =
    allPreferences(context.host(), context.hasDocument() ? &context.document() : nullptr);

  const auto paths = args.getOr<std::vector<std::string>>("paths", {});
  const auto prefixes = stringOrStrings(args, "prefix");
  const auto categories = stringOrStrings(args, "category");
  const auto words =
    kdl::str_split(kdl::str_to_lower(args.getOr<std::string>("query", "")), " ");
  const auto modifiedOnly = args.getOr("modifiedOnly", false);

  auto selected = std::vector<const PreferenceInfo*>{};
  if (!paths.empty())
  {
    auto unknown = std::vector<std::string>{};
    for (const auto& path : paths)
    {
      if (const auto* info = findPreference(preferences, path))
      {
        selected.push_back(info);
      }
      else
      {
        unknown.push_back(path);
      }
    }
    if (!unknown.empty())
    {
      return unknownPathError(preferences, unknown);
    }
  }
  else
  {
    for (const auto& info : preferences)
    {
      selected.push_back(&info);
    }
  }

  auto counts = std::map<std::string, size_t>{};
  for (const auto& info : preferences)
  {
    ++counts[info.category];
  }

  auto items = std::vector<Json>{};
  for (const auto* info : selected)
  {
    const auto path = pathString(*info);
    if (!prefixes.empty() && std::ranges::none_of(prefixes, [&](const auto& prefix) {
          return kdl::ci::str_is_prefix(path, prefix);
        }))
    {
      continue;
    }
    if (
      !categories.empty() && std::ranges::none_of(categories, [&](const auto& category) {
        return kdl::ci::str_is_equal(info->category, category);
      }))
    {
      continue;
    }
    if (!words.empty())
    {
      const auto haystack =
        kdl::str_to_lower(path + " " + info->category + " " + info->description);
      if (std::ranges::any_of(words, [&](const auto& word) {
            return haystack.find(word) == std::string::npos;
          }))
      {
        continue;
      }
    }

    auto item = itemJson(*info, request.value().detail);
    if (modifiedOnly && !item["modified"].get<bool>())
    {
      continue;
    }
    items.push_back(std::move(item));
  }

  auto result = makePage(items, request.value(), 0);
  result["categories"] = counts;
  return result;
}

// preferences_set

/** A validated change of one preference. */
struct PlannedChange
{
  const PreferenceInfo* info = nullptr;
  Json previous;
  Json value;
  /** The new shortcuts, for conflict detection. */
  std::optional<std::vector<std::string>> shortcuts;
  std::function<void()> apply;
};

std::string describeAllowed(const std::vector<Json>& values)
{
  auto strings = std::vector<std::string>{};
  for (const auto& value : values)
  {
    strings.push_back(value.dump());
  }
  return kdl::str_join(strings, ", ");
}

std::optional<std::string> checkRange(const PreferenceInfo& info, const double value)
{
  if ((info.minimum && value < *info.minimum) || (info.maximum && value > *info.maximum))
  {
    return fmt::format(
      "must be between {} and {}",
      info.minimum ? fmt::format("{}", *info.minimum) : "-inf",
      info.maximum ? fmt::format("{}", *info.maximum) : "inf");
  }
  return std::nullopt;
}

std::optional<std::string> checkAllowed(const PreferenceInfo& info, const Json& value)
{
  if (
    !info.allowedValues.empty()
    && std::ranges::find(info.allowedValues, value) == info.allowedValues.end())
  {
    return fmt::format("must be one of {}", describeAllowed(info.allowedValues));
  }
  return std::nullopt;
}

std::optional<uint8_t> hexByte(const std::string_view str)
{
  auto value = 0;
  for (const auto c : str)
  {
    value *= 16;
    if (c >= '0' && c <= '9')
    {
      value += c - '0';
    }
    else if (c >= 'a' && c <= 'f')
    {
      value += c - 'a' + 10;
    }
    else if (c >= 'A' && c <= 'F')
    {
      value += c - 'A' + 10;
    }
    else
    {
      return std::nullopt;
    }
  }
  return uint8_t(value);
}

template <typename T>
struct Parsed
{
  std::optional<T> value;
  std::string error;
};

Parsed<bool> parseValue(const PreferenceInfo&, const Json& json, bool*)
{
  if (!json.is_boolean())
  {
    return {std::nullopt, "expects true or false"};
  }
  return {json.get<bool>(), {}};
}

Parsed<int> parseValue(const PreferenceInfo& info, const Json& json, int*)
{
  if (!json.is_number() || std::floor(json.get<double>()) != json.get<double>())
  {
    return {std::nullopt, "expects an integer"};
  }
  const auto value = json.get<double>();
  if (
    value < double(std::numeric_limits<int>::min())
    || value > double(std::numeric_limits<int>::max()))
  {
    return {std::nullopt, "is out of range"};
  }
  if (auto error = checkRange(info, value))
  {
    return {std::nullopt, std::move(*error)};
  }
  if (auto error = checkAllowed(info, Json(int(value))))
  {
    return {std::nullopt, std::move(*error)};
  }
  return {int(value), {}};
}

Parsed<float> parseValue(const PreferenceInfo& info, const Json& json, float*)
{
  if (!json.is_number())
  {
    return {std::nullopt, "expects a number"};
  }
  const auto value = json.get<double>();
  if (auto error = checkRange(info, value))
  {
    return {std::nullopt, std::move(*error)};
  }
  return {float(value), {}};
}

Parsed<std::string> parseValue(const PreferenceInfo& info, const Json& json, std::string*)
{
  if (!json.is_string())
  {
    return {std::nullopt, "expects a string"};
  }
  auto value = json.get<std::string>();
  if (!info.allowedValues.empty())
  {
    // accept a case-insensitive match and use the allowed spelling
    for (const auto& allowed : info.allowedValues)
    {
      if (allowed.is_string() && kdl::ci::str_is_equal(allowed.get<std::string>(), value))
      {
        return {allowed.get<std::string>(), {}};
      }
    }
    return {
      std::nullopt,
      fmt::format("must be one of {}", describeAllowed(info.allowedValues))};
  }
  return {std::move(value), {}};
}

Parsed<std::filesystem::path> parseValue(
  const PreferenceInfo&, const Json& json, std::filesystem::path*)
{
  if (!json.is_string())
  {
    return {std::nullopt, "expects a path string ('' clears it)"};
  }
  return {std::filesystem::path{json.get<std::string>()}.lexically_normal(), {}};
}

Parsed<Color> parseValue(const PreferenceInfo&, const Json& json, Color*)
{
  static const auto expected = std::string{
    "expects \"#RRGGBB\", \"#RRGGBBAA\", [r, g, b] or [r, g, b, a] with components "
    "from 0 to 1"};

  if (json.is_string())
  {
    const auto str = kdl::str_trim(json.get<std::string>());
    if (str.starts_with('#'))
    {
      const auto hex = std::string_view{str}.substr(1);
      if (hex.size() != 6 && hex.size() != 8)
      {
        return {std::nullopt, expected};
      }
      auto bytes = std::vector<uint8_t>{};
      for (size_t i = 0; i < hex.size(); i += 2)
      {
        const auto byte = hexByte(hex.substr(i, 2));
        if (!byte)
        {
          return {std::nullopt, expected};
        }
        bytes.push_back(*byte);
      }
      return bytes.size() == 3
               ? Parsed<Color>{Color{RgbB{bytes[0], bytes[1], bytes[2]}}, {}}
               : Parsed<Color>{Color{RgbaB{bytes[0], bytes[1], bytes[2], bytes[3]}}, {}};
    }

    // TrenchBroom's own form, e.g. "1 0.5 0" or "255 128 0"
    if (auto color = Color::parse(str); color.is_success())
    {
      return {color.value(), {}};
    }
    return {std::nullopt, expected};
  }

  if (json.is_array() && (json.size() == 3 || json.size() == 4))
  {
    auto components = std::vector<float>{};
    for (const auto& component : json)
    {
      if (!component.is_number())
      {
        return {std::nullopt, expected};
      }
      const auto value = component.get<double>();
      if (value < 0.0 || value > 1.0)
      {
        return {std::nullopt, expected};
      }
      components.push_back(float(value));
    }
    return components.size() == 3
             ? Parsed<Color>{Color{RgbF{components[0], components[1], components[2]}}, {}}
             : Parsed<Color>{
                 Color{RgbaF{components[0], components[1], components[2], components[3]}},
                 {}};
  }

  return {std::nullopt, expected};
}

Parsed<std::vector<KeySequence>> parseValue(
  const PreferenceInfo&, const Json& json, std::vector<KeySequence>*)
{
  static const auto expected = std::string{
    "expects an array of shortcuts in portable text, e.g. [\"Ctrl+Shift+K\"], or [] "
    "to remove the shortcut"};

  auto texts = std::vector<std::string>{};
  if (json.is_string())
  {
    texts.push_back(json.get<std::string>());
  }
  else if (json.is_array())
  {
    for (const auto& element : json)
    {
      if (!element.is_string())
      {
        return {std::nullopt, expected};
      }
      texts.push_back(element.get<std::string>());
    }
  }
  else
  {
    return {std::nullopt, expected};
  }

  auto result = std::vector<KeySequence>{};
  for (const auto& text : texts)
  {
    const auto trimmed = kdl::str_trim(text);
    if (!trimmed.empty())
    {
      result.push_back(KeySequence{trimmed});
    }
  }
  return {std::move(result), {}};
}

struct PlanError
{
  ErrorCode code;
  std::string message;
};

/** Plans setting the given preference to the given JSON value. */
std::variant<PlannedChange, PlanError> planSet(
  const PreferenceInfo& info, const Json& json)
{
  const auto path = pathString(info);
  return std::visit(
    [&](auto* preference) -> std::variant<PlannedChange, PlanError> {
      using T = std::decay_t<decltype(preference->defaultValue)>;
      auto parsed = parseValue(info, json, static_cast<T*>(nullptr));
      if (!parsed.value)
      {
        return PlanError{
          ErrorCode::InvalidArgument,
          fmt::format(
            "'{}' ({}) {}, got {}",
            path,
            preferenceTypeName(info.preference),
            parsed.error,
            json.dump())};
      }

      auto change = PlannedChange{
        .info = &info,
        .previous = currentValueJson(info.preference),
        .value = valueJson(*parsed.value),
      };
      if constexpr (std::is_same_v<T, std::vector<KeySequence>>)
      {
        change.shortcuts = shortcutTexts(*parsed.value);
      }
      change.apply = [preference, value = std::move(*parsed.value)]() {
        setPref(*preference, value);
      };
      return change;
    },
    info.preference);
}

/** Plans resetting the given preference to its default. */
PlannedChange planReset(const PreferenceInfo& info)
{
  return std::visit(
    [&](auto* preference) {
      using T = std::decay_t<decltype(preference->defaultValue)>;
      auto change = PlannedChange{
        .info = &info,
        .previous = currentValueJson(info.preference),
        .value = valueJson(preference->defaultValue),
      };
      if constexpr (std::is_same_v<T, std::vector<KeySequence>>)
      {
        change.shortcuts = shortcutTexts(preference->defaultValue);
      }
      change.apply = [preference]() { setPref(*preference, preference->defaultValue); };
      return change;
    },
    info.preference);
}

std::optional<PlanError> checkWritable(const PreferenceInfo& info)
{
  const auto path = pathString(info);
  if (preferencePersistence(info.preference) == PreferencePersistencePolicy::ReadOnly)
  {
    return PlanError{
      ErrorCode::ObjectNotEditable, fmt::format("'{}' is read-only.", path)};
  }
  if (!info.lockedReason.empty())
  {
    return PlanError{
      ErrorCode::ObjectNotEditable,
      fmt::format("'{}' cannot be changed by agents: {}", path, info.lockedReason)};
  }
  return std::nullopt;
}

/** The other shortcut preferences that use one of the given change's shortcuts. */
Json shortcutConflicts(
  const PlannedChange& change,
  const std::vector<PreferenceInfo>& preferences,
  const std::vector<PlannedChange>& changes)
{
  auto conflicts = Json::array();
  if (!change.shortcuts)
  {
    return conflicts;
  }

  for (const auto& other : preferences)
  {
    if (&other == change.info)
    {
      continue;
    }

    auto otherShortcuts = std::optional<std::vector<std::string>>{};
    if (const auto it =
          std::ranges::find_if(changes, [&](const auto& c) { return c.info == &other; });
        it != changes.end())
    {
      otherShortcuts = it->shortcuts;
    }
    else if (
      const auto* keyPreference =
        std::get_if<Preference<std::vector<KeySequence>>*>(&other.preference))
    {
      otherShortcuts = shortcutTexts(pref(**keyPreference));
    }

    if (otherShortcuts)
    {
      for (const auto& shortcut : *change.shortcuts)
      {
        if (std::ranges::any_of(*otherShortcuts, [&](const auto& otherShortcut) {
              return kdl::ci::str_is_equal(shortcut, otherShortcut);
            }))
        {
          conflicts.push_back(Json{
            {"shortcut", shortcut},
            {"path", pathString(other)},
            {"description", other.description},
          });
        }
      }
    }
  }
  return conflicts;
}

std::optional<std::string> gameName(const PreferenceInfo& info)
{
  if (info.source != PreferenceSource::Game)
  {
    return std::nullopt;
  }
  // "Games/<game>/..."
  auto it = info.path().begin();
  if (it == info.path().end() || ++it == info.path().end())
  {
    return std::nullopt;
  }
  return it->string();
}

ToolResult preferencesSet(CallContext& context, const Args& args)
{
  const auto values = args.getOr<Json>("values", Json::object());
  const auto reset = args.getOr<std::vector<std::string>>("reset", {});
  if (values.empty() && reset.empty())
  {
    return makeError(
      ErrorCode::InvalidArgument,
      "Nothing to change.",
      "Pass 'values', e.g. {\"values\": {\"Map view/Show edges\": false}}, or 'reset', "
      "e.g. {\"reset\": [\"Views/Map view layout\"]}.");
  }

  const auto preferences =
    allPreferences(context.host(), context.hasDocument() ? &context.document() : nullptr);

  // resolve the paths
  auto requests = std::vector<std::pair<const PreferenceInfo*, std::optional<Json>>>{};
  auto unknown = std::vector<std::string>{};
  const auto resolve = [&](const std::string& path, std::optional<Json> value) {
    if (const auto* info = findPreference(preferences, path))
    {
      requests.emplace_back(info, std::move(value));
    }
    else
    {
      unknown.push_back(path);
    }
  };
  for (const auto& item : values.items())
  {
    resolve(item.key(), item.value());
  }
  for (const auto& path : reset)
  {
    resolve(path, std::nullopt);
  }
  if (!unknown.empty())
  {
    return unknownPathError(preferences, unknown);
  }

  // validate everything before changing anything
  auto changes = std::vector<PlannedChange>{};
  auto errors = std::vector<PlanError>{};
  auto seen = std::set<const PreferenceInfo*>{};
  for (const auto& [info, value] : requests)
  {
    if (!seen.insert(info).second)
    {
      errors.push_back(PlanError{
        ErrorCode::InvalidArgument,
        fmt::format("'{}' is given more than once.", pathString(*info))});
      continue;
    }
    if (auto error = checkWritable(*info))
    {
      errors.push_back(std::move(*error));
      continue;
    }
    if (value)
    {
      auto planned = planSet(*info, *value);
      if (auto* error = std::get_if<PlanError>(&planned))
      {
        errors.push_back(std::move(*error));
        continue;
      }
      changes.push_back(std::move(std::get<PlannedChange>(planned)));
    }
    else
    {
      changes.push_back(planReset(*info));
    }
  }

  if (!errors.empty())
  {
    auto messages = std::vector<std::string>{};
    auto details = Json::array();
    for (const auto& error : errors)
    {
      messages.push_back(error.message);
      details.push_back(Json{{"code", toString(error.code)}, {"message", error.message}});
    }
    auto result = makeError(
      errors.front().code,
      fmt::format("Nothing was changed: {}", kdl::str_join(messages, "; ")),
      "preferences_get with 'paths' shows each preference's type, allowed values and "
      "range.");
    result.details = Json{{"errors", std::move(details)}};
    return result;
  }

  // report the conflicts before applying (the values are taken from the plan)
  auto conflicts = std::vector<Json>{};
  for (const auto& change : changes)
  {
    conflicts.push_back(shortcutConflicts(change, preferences, changes));
  }

  if (!context.dryRun())
  {
    for (const auto& change : changes)
    {
      change.apply();
    }
  }

  auto items = Json::array();
  auto changedGames = std::set<std::string>{};
  for (size_t i = 0; i < changes.size(); ++i)
  {
    const auto& change = changes[i];
    const auto& info = *change.info;
    const auto changed = change.previous != change.value;

    auto item = Json{
      {"path", pathString(info)},
      {"type", preferenceTypeName(info.preference)},
      {"value", info.secret ? Json(nullptr) : change.value},
      {"previous", info.secret ? Json(nullptr) : change.previous},
      {"changed", changed},
    };
    if (changed && !info.note.empty())
    {
      item["note"] = info.note;
    }
    if (!conflicts[i].empty())
    {
      item["conflicts"] = conflicts[i];
      for (const auto& conflict : conflicts[i])
      {
        context.warn(
          "SHORTCUT_CONFLICT",
          fmt::format(
            "{} is also assigned to '{}'; both shortcuts only work if they apply in "
            "different contexts (e.g. 2D and 3D views).",
            conflict["shortcut"].get<std::string>(),
            conflict["path"].get<std::string>()));
      }
    }
    if (info.source == PreferenceSource::Game && change.value.is_string())
    {
      const auto path = std::filesystem::path{change.value.get<std::string>()};
      if (!path.empty() && !pathExists(path))
      {
        context.warn(
          "PATH_NOT_FOUND",
          fmt::format("'{}': {} does not exist.", pathString(info), path.string()));
      }
      if (changed)
      {
        if (const auto name = gameName(info))
        {
          changedGames.insert(*name);
        }
      }
    }
    items.push_back(std::move(item));
  }

  if (!context.dryRun())
  {
    for (const auto& name : changedGames)
    {
      context.server().notifyResourceUpdated(gameConfigUri(name));
    }
  }

  auto result = Json{{"items", std::move(items)}};
  if (context.dryRun())
  {
    result["wouldDo"] = fmt::format(
      "change {} preference{}", changes.size(), changes.size() == 1 ? "" : "s");
  }
  return result;
}

Schema stringOrStringsSchema(std::string description)
{
  return oneOf({string(), array(string())}).describe(std::move(description));
}

Schema itemSchema()
{
  return object({
    field("path", string()).required(),
    field("type", string()),
    field("value", any()),
    field("modified", boolean()).describe("Whether the value differs from the default"),
    field("category", string()),
    field("description", string()),
    field("allowedValues", array(any())),
    field("minimum", number()),
    field("maximum", number()),
    field("secret", boolean()).describe("The value is hidden (e.g. the access token)"),
    field("default", any()),
    field("persistence", string()).describe("full: persistent, transient or readOnly"),
    field("source", string())
      .describe("full: editor, game (game configuration) or host (editor-only settings)"),
    field("note", string()).describe("E.g. that a change needs a restart"),
    field("lockedReason", string()).describe("Why agents cannot change it, if so"),
  });
}

} // namespace

void registerPreferenceTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"preferences_get"}
      .title("Get Preferences")
      .description(
        "Lists the editor's preferences with their current values: view options, "
        "colors, camera and mouse settings, keyboard shortcuts (category 'keyboard', "
        "the paths of actions_list; with a document also its tag and entity shortcuts), "
        "game paths and compilation tool paths (category 'games'), the MCP server "
        "(category 'mcp') and more. Filter with 'paths', 'prefix', 'category', 'query' "
        "and 'modifiedOnly'; 'categories' counts all preferences per category. Types and "
        "value forms: bool, int, float, string, path (string), color (\"#RRGGBB\" or "
        "\"#RRGGBBAA\"), shortcuts (array of portable key sequences such as "
        "\"Ctrl+Shift+K\"). detail 'full' adds the default, persistence, source and "
        "notes. Read-only; change values with preferences_set. Examples: "
        "{\"category\": \"view\"}; {\"query\": \"grid\"}; {\"prefix\": "
        "\"Games/Quake/\", \"modifiedOnly\": true, \"detail\": \"full\"}")
      .input(object({
        field("paths", array(string()))
          .describe("Exact preference paths, e.g. [\"Map view/Show edges\"]"),
        field(
          "prefix",
          stringOrStringsSchema(
            "Path prefix or prefixes, e.g. \"Games/Quake/\" or \"Menu/View/\"")),
        field(
          "category",
          stringOrStringsSchema(
            "Category or categories: view, renderer, colors, camera, controls, editor, "
            "browser, updater, keyboard, games, mcp")),
        field("query", string())
          .describe("Words that must all appear in the path, category or description"),
        field("modifiedOnly", boolean().defaultsTo(false))
          .describe("Only preferences whose value differs from the default"),
      }))
      .output(object({
        field("items", array(itemSchema())).required(),
        field("total", integer()).required(),
        field("nextCursor", any()),
        field("stale", boolean())
          .describe("Whether the list may have changed since the cursor was issued"),
        field("categories", any())
          .describe("Number of preferences per category (unfiltered)"),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .paginated()
      .idempotent()
      .handler(preferencesGet));

  registry.add(
    ToolDef{"preferences_set"}
      .title("Set Preferences")
      .description(
        "Changes editor preferences (saved immediately, not undoable): 'values' maps "
        "preference paths to new values, 'reset' lists paths to restore to their "
        "defaults. All changes are validated first (type, allowed values, range, "
        "read-only); if one is invalid, nothing changes. Values use the forms of "
        "preferences_get; colors also accept [r, g, b(, a)] with components from 0 to 1, "
        "shortcuts also a single string, [] removes a shortcut. Returns each previous "
        "and new value, notes (e.g. restart needed) and shortcut conflicts. The MCP "
        "server's connection preferences (enabled, port, bind address, access token) "
        "cannot be changed by agents. Example: {\"values\": {\"Views/Map view layout\": "
        "3, \"render/Colors/Background\": \"#202020\", \"Menu/View/Maximize Current "
        "View\": [\"Ctrl+M\"]}, \"reset\": [\"Controls/Camera/Field of vision\"]}")
      .input(object({
        field("values", object({}).allowAdditionalProperties())
          .describe(
            "Preference path (as preferences_get lists it) -> new value in the forms of "
            "preferences_get"),
        field("reset", array(string()))
          .describe("Preference paths to restore to their defaults"),
      }))
      .output(object({
        field(
          "items",
          array(object({
            field("path", string()).required(),
            field("type", string()),
            field("value", any()),
            field("previous", any()).describe("The value before the call"),
            field("changed", boolean()),
            field("note", string()).describe("E.g. that the change needs a restart"),
            field("conflicts", array(any()))
              .describe("Other actions that use the same shortcut"),
          })))
          .required(),
        field("wouldDo", string().describe("Dry run only: what the call would do")),
      }))
      .mutation(Mutation::External)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(preferencesSet));
}

} // namespace tb::mcp
