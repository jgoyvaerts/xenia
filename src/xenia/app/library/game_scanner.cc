#include "xenia/app/library/game_scanner.h"
#include "xenia/app/library/scanner_utils.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

#include <deque>

namespace xe {
namespace app {

using filesystem::FileInfo;
using AsyncCallback = XGameScanner::AsyncCallback;

std::vector<std::filesystem::path> XGameScanner::FindGamesInPath(
    const std::filesystem::path& path) {
  // Path is a directory, scan recursively
  // TODO: Warn about recursively scanning paths with large hierarchies

  std::deque<std::filesystem::path> queue;
  queue.push_front(path);

  std::vector<std::filesystem::path> paths;
  int game_count = 0;

  while (!queue.empty()) {
    std::filesystem::path current_path = queue.front();
    FileInfo current_file;
    filesystem::GetInfo(current_path, &current_file);

    queue.pop_front();

    if (current_file.type == FileInfo::Type::kDirectory) {
      std::vector<FileInfo> directory_files =
          filesystem::ListFiles(current_path);
      for (FileInfo file : directory_files) {
        if (CompareCaseInsensitive(file.name, L"$SystemUpdate")) continue;

        auto next_path = current_path / file.name;
        // Skip searching directories with an extracted default.xex file
        if (std::filesystem::exists(next_path / "default.xex")) {
          queue.push_front(next_path / "default.xex");
          continue;
        }
        queue.push_front(next_path);
      }
    } else {
      // Exclusively scan iso, xex, or files without an extension.
      auto extension = GetFileExtension(current_path);
      if (!extension.empty() && extension != ".xex" && extension != ".iso") {
        continue;
      }

      // Do not attempt to scan SVOD data files
      auto filename = GetFileName(current_path);
      if (memcmp(filename.c_str(), L"Data", 4) == 0) continue;

      // Skip empty files
      if (current_file.total_size == 0) continue;

      paths.push_back(current_path);
      game_count++;
    }
  }
  return paths;
}

std::vector<XGameEntry> XGameScanner::ScanPath(
    const std::filesystem::path& path) {
  std::vector<XGameEntry> games;

  // Check if the given path exists
  if (!std::filesystem::exists(path)) {
    return games;
  }

  // Scan if the given path is a file
  if (!std::filesystem::is_directory(path)) {
    XGameEntry game_entry;
    if (XFAILED(ScanGame(path, &game_entry))) {
      //XELOGE("Failed to scan game at {}", xe::path_to_utf8(path));
    } else {
      games.emplace_back(std::move(game_entry));
    }
  } else {
    const std::vector<std::filesystem::path>& game_paths =
        FindGamesInPath(path);
    for (const std::filesystem::path& game_path : game_paths) {
      XGameEntry game_entry;
      if (XFAILED(ScanGame(game_path, &game_entry))) {
        continue;
      }
      games.emplace_back(std::move(game_entry));
    }
  }

  //XELOGI("Scanned {} files", games.size());
  return games;
}

int XGameScanner::ScanPathAsync(const std::filesystem::path& path,
                                const AsyncCallback& cb) {
  std::vector<std::filesystem::path> paths = {path};
  return ScanPathsAsync(paths, cb);
}

int XGameScanner::ScanPathsAsync(
    const std::vector<std::filesystem::path>& paths,
                                 const AsyncCallback& cb) {
  // start scanning in a new thread
  // TODO: switch to xe::threading::Thread instead of std::thread?
  std::thread scan_thread = std::thread(
      [](std::vector<std::filesystem::path> paths, AsyncCallback cb) {
        std::atomic<int> scanned = 0;

        auto scan_func = [&](const std::vector<std::filesystem::path>& paths,
                             size_t start,
                             size_t size) {
          for (auto it = paths.begin() + start;
               it != paths.begin() + start + size; ++it) {
            scanned++;
            XGameEntry game_info;
            auto status = ScanGame(*it, &game_info);
            if (cb && XSUCCEEDED(status)) {
              cb(game_info, scanned);
            } else {
              //XELOGE("Failed to scan game at {}", it);
            }
          }
        };

        uint32_t thread_count = xe::threading::logical_processor_count();

        std::vector<std::thread> threads;

        // scan games on this thread if the user has < 4 cores
        if (thread_count < 4) {
          scan_func(paths, 0, paths.size());
        } else {
          // split workload into even amounts based on core count
          size_t total_size = paths.size();
          size_t work_size = paths.size() / thread_count;
          size_t leftover = paths.size() % thread_count;

          if (work_size > 0) {
            for (uint32_t i = 0; i < thread_count; i++) {
              threads.emplace_back(std::move(
                  std::thread(scan_func, paths, i * work_size, work_size)));
            }
          }
          scan_func(paths, total_size - leftover, leftover);
        }

        for (auto& thread : threads) {
          thread.join();
        }
      },
      paths, cb);

  scan_thread.detach();

  return (int)paths.size();
}

X_STATUS XGameScanner::ScanGame(const std::filesystem::path& path,
                                XGameEntry* out_info) {
  if (!out_info) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!std::filesystem::exists(path)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  XGameFormat format = ResolveFormat(path);
  const char* format_str;

  switch (format) {
    case XGameFormat::kIso: {
      format_str = "ISO";
      break;
    }
    case XGameFormat::kStfs: {
      format_str = "STFS";
      break;
    }
    case XGameFormat::kXex: {
      format_str = "XEX";
      break;
    }
    default: {
      format_str = "Unknown";
      break;
    }
  }

  //XELOGD("Scanning {}", std::filesystem::absolute(path));

  auto device = CreateDevice(path);
  if (device == nullptr || !device->Initialize()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  XELOGI("Format is {}", format_str);

  GameInfo game_info;
  game_info.filename = GetFileName(path);
  game_info.path = path;
  game_info.format = format;

  // Read XEX
  auto xex_entry = device->ResolvePath("default.xex");
  if (xex_entry) {
    File* xex_file = nullptr;
    auto status = xex_entry->Open(vfs::FileAccess::kFileReadData, &xex_file);
    if (XSUCCEEDED(status)) {
      status = XexScanner::ScanXex(xex_file, &game_info);
      if (!XSUCCEEDED(status)) {
        XELOGE("Could not parse xex file: {}",
               xex_file->entry()->path().c_str());
        return status;
      }
    } else {
      XELOGE("Could not load default.xex from device: {}", status);
      return X_STATUS_UNSUCCESSFUL;
    }

    xex_file->Destroy();
  } else {
    XELOGE("Could not resolve default.xex");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Read NXE
  auto nxe_entry = device->ResolvePath("nxeart");
  if (nxe_entry) {
    File* nxe_file = nullptr;
    auto status = nxe_entry->Open(vfs::FileAccess::kFileReadData, &nxe_file);
    if (XSUCCEEDED(status)) {
      status = NxeScanner::ScanNxe(nxe_file, &game_info);
      if (!XSUCCEEDED(status)) {
        XELOGE("Could not parse nxeart file: {}",
               nxe_file->entry()->path());
        return status;
      }
    } else {
      XELOGE("Could not load nxeart from device: %x", status);
    }

    nxe_file->Destroy();
  } else {
    XELOGI("Game does not have an nxeart file");
  }

  out_info->apply_info(game_info);

  return X_STATUS_SUCCESS;
}

}  // namespace app
}  // namespace xe