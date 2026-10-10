#include "shell/dock/dock_model.h"

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "core/log.h"
#include "shell/dock/pinned_apps.h"
#include "system/app_identity.h"
#include "util/string_utils.h"
#include "wayland/wayland_toplevels.h"

#include <unordered_map>

namespace {

  constexpr Logger kLog("dock");

  [[nodiscard]] std::string
  taskbarStyleWmClassLower(std::string_view startupWmClass, std::string_view fallbackIdLower) {
    if (!startupWmClass.empty()) {
      return StringUtils::toLower(std::string(startupWmClass));
    }
    return std::string(fallbackIdLower);
  }

} // namespace

namespace shell::dock {

  wl_output* dockFilterOutput(const DockConfig& cfg, wl_output* instanceOutput) {
    if (!cfg.activeMonitorOnly) {
      return nullptr;
    }
    return instanceOutput;
  }

  std::string currentActiveEntryIdLower(const CompositorPlatform& platform) {
    if (const auto active = platform.activeToplevel(); active.has_value()) {
      return StringUtils::toLower(app_identity::resolveRunningDesktopEntry(active->appId, desktopEntries()).id);
    }
    return {};
  }

  bool refreshPinnedAppsIfNeeded(
      const DockConfig& cfg, std::vector<std::string>& lastPinnedConfig, std::vector<DesktopEntry>& pinnedEntries,
      std::uint64_t& modelSerial, std::uint64_t& entriesVersion
  ) {
    if (desktopEntriesVersion() == entriesVersion && cfg.pinned == lastPinnedConfig) {
      return false;
    }

    lastPinnedConfig = cfg.pinned;
    entriesVersion = desktopEntriesVersion();
    pinnedEntries = pinned_apps::resolveEntries(cfg.pinned);

    ++modelSerial;
    kLog.debug("pinned app list: {} entries", pinnedEntries.size());
    return true;
  }

  std::string extractExecBinary(std::string_view exec) {
    if (exec.empty()) {
      return {};
    }
    std::size_t start = 0;
    while (start < exec.size() && std::isspace(static_cast<unsigned char>(exec[start])) != 0) {
      ++start;
    }
    while (start < exec.size()) {
      std::size_t end = start;
      while (end < exec.size() && std::isspace(static_cast<unsigned char>(exec[end])) == 0) {
        ++end;
      }
      std::string_view token = exec.substr(start, end - start);
      if (token.size() >= 2
          && ((token.front() == '"' && token.back() == '"') || (token.front() == '\'' && token.back() == '\''))) {
        token = token.substr(1, token.size() - 2);
      }
      if (token.contains('=')) {
        start = end;
        while (start < exec.size() && std::isspace(static_cast<unsigned char>(exec[start])) != 0) {
          ++start;
        }
        continue;
      }
      if (const auto slash = token.find_last_of('/'); slash != std::string_view::npos) {
        token = token.substr(slash + 1);
      }
      std::string lower = StringUtils::toLower(std::string(token));
      if (lower == "env" || lower == "prime-run" || lower == "gamemoderun" || lower == "systemd-run") {
        start = end;
        while (start < exec.size() && std::isspace(static_cast<unsigned char>(exec[start])) != 0) {
          ++start;
        }
        continue;
      }
      if (lower == "flatpak" || lower == "run") {
        start = end;
        while (start < exec.size() && std::isspace(static_cast<unsigned char>(exec[start])) != 0) {
          ++start;
        }
        continue;
      }
      return lower;
    }
    return {};
  }

  bool isEntryAlreadyRepresentedInDock(const std::vector<DesktopEntry>& entries, const DesktopEntry& candidate) {
    const std::string candidateIdLower = StringUtils::toLower(candidate.id);
    const std::string candidateWmClassLower = StringUtils::toLower(candidate.startupWmClass);
    const std::string candidateExecBinary = extractExecBinary(candidate.exec);

    for (const auto& entry : entries) {
      const std::string entryIdLower = StringUtils::toLower(entry.id);
      if (!candidateIdLower.empty() && entryIdLower == candidateIdLower) {
        return true;
      }
      if (!candidateWmClassLower.empty()) {
        if (!entry.startupWmClass.empty() && StringUtils::toLower(entry.startupWmClass) == candidateWmClassLower) {
          return true;
        }
        if (entryIdLower == candidateWmClassLower) {
          return true;
        }
      }
      if (!candidateExecBinary.empty()) {
        const std::string entryExecBinary = extractExecBinary(entry.exec);
        if (!entryExecBinary.empty() && entryExecBinary == candidateExecBinary) {
          return true;
        }
        if (entryIdLower == candidateExecBinary) {
          return true;
        }
      }
      if (!candidateIdLower.empty() && !entryIdLower.empty()) {
        if (candidateIdLower.starts_with(entryIdLower + "-")
            || candidateIdLower.starts_with(entryIdLower + "_")
            || entryIdLower.starts_with(candidateIdLower + "-")
            || entryIdLower.starts_with(candidateIdLower + "_")) {
          return true;
        }
      }
    }
    return false;
  }

  namespace {

    bool
    alreadyListsResolvedEntry(const std::vector<DesktopEntry>& entries, const app_identity::ResolvedRunningApp& run) {
      const std::string resolvedIdLower = StringUtils::toLower(run.entry.id);
      for (const auto& entry : entries) {
        if (StringUtils::toLower(entry.id) == resolvedIdLower) {
          return true;
        }
      }
      return false;
    }

  } // namespace

  std::vector<ToplevelInfo>
  windowsForDockItem(CompositorPlatform& platform, const DockItemModel& item, wl_output* outputFilter) {
    return windowsForDockItem(platform, item.windowLookupIdLower, item.windowLookupWmClassLower, outputFilter);
  }

  std::vector<ToplevelInfo> windowsForDockItem(
      CompositorPlatform& platform, std::string_view idLower, std::string_view wmClassLower, wl_output* outputFilter
  ) {
    return platform.enrichedWindowsForApp(std::string(idLower), std::string(wmClassLower), outputFilter);
  }

  DockSnapshot buildDockSnapshot(DockModelDependencies deps) {
    DockSnapshot snapshot;
    snapshot.output = deps.output;
    snapshot.filterOutput = dockFilterOutput(deps.config, deps.output);
    snapshot.sourceSerial = deps.sourceSerial;

    wl_output* const activeOutput = deps.platform.activeToplevelOutput();
    snapshot.activeAppIdLower =
        (deps.config.activeMonitorOnly && activeOutput != deps.output) ? std::string{} : deps.globalActiveIdLower;

    const auto runningIds =
        deps.config.showRunning ? deps.platform.runningAppIds(snapshot.filterOutput) : std::vector<std::string>{};
    const auto resolvedRunning = app_identity::resolveRunningApps(runningIds, desktopEntries(), deps.pinnedEntries);

    std::unordered_map<std::string, std::string> compositorIdByEntryId;
    compositorIdByEntryId.reserve(resolvedRunning.size());
    for (const auto& run : resolvedRunning) {
      compositorIdByEntryId.emplace(StringUtils::toLower(run.entry.id), run.runningLower);
    }

    std::vector<DesktopEntry> itemEntries = deps.pinnedEntries;
    if (deps.config.showRunning) {
      for (const auto& run : resolvedRunning) {
        if (!alreadyListsResolvedEntry(itemEntries, run)) {
          itemEntries.push_back(run.entry);
        }
      }
    }

    for (const auto& launching : deps.launchingEntries) {
      if (!isEntryAlreadyRepresentedInDock(itemEntries, launching)) {
        itemEntries.push_back(launching);
      }
    }

    snapshot.items.reserve(itemEntries.size());
    for (const auto& entry : itemEntries) {
      DockItemModel dockItem;
      dockItem.entry = entry;
      dockItem.idLower = StringUtils::toLower(entry.id);
      dockItem.startupWmClassLower = StringUtils::toLower(entry.startupWmClass);

      if (const auto it = compositorIdByEntryId.find(dockItem.idLower); it != compositorIdByEntryId.end()) {
        dockItem.running = true;
        dockItem.windowLookupIdLower = it->second;
        dockItem.windowLookupWmClassLower = taskbarStyleWmClassLower(entry.startupWmClass, it->second);
      } else {
        dockItem.running = false;
        dockItem.windowLookupIdLower = dockItem.idLower;
        dockItem.windowLookupWmClassLower = taskbarStyleWmClassLower(entry.startupWmClass, dockItem.idLower);
      }

      dockItem.active = !snapshot.activeAppIdLower.empty() && snapshot.activeAppIdLower == dockItem.idLower;
      if (deps.config.showDots || deps.config.showInstanceCount) {
        dockItem.instanceCount = windowsForDockItem(deps.platform, dockItem, snapshot.filterOutput).size();
      }
      snapshot.items.push_back(std::move(dockItem));
    }

    return snapshot;
  }

  bool sameDockItemSet(const DockSnapshot& a, const DockSnapshot& b) {
    if (a.items.size() != b.items.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.items.size(); ++i) {
      if (a.items[i].entry.id != b.items[i].entry.id) {
        return false;
      }
      if (a.items[i].idLower != b.items[i].idLower) {
        return false;
      }
      if (a.items[i].startupWmClassLower != b.items[i].startupWmClassLower) {
        return false;
      }
    }
    return true;
  }

} // namespace shell::dock
