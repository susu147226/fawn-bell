#include "file_save_model.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace luling::ui {
namespace {

bool ValidText(std::wstring_view text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned>(text[i]);
        if (c < 32) return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) return false;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            return false;
        }
    }
    return true;
}

bool Extension(std::wstring_view text) {
    return !text.empty() && ValidText(text) && text.front() != L'.' && text.back() != L'.' &&
        text.find_first_of(L"<>:\"/\\|?*; ") == std::wstring_view::npos;
}

bool Reserved(std::wstring_view name) {
    std::wstring stem(name.substr(0, name.find(L'.')));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$") return true;
    if (stem.size() != 4 || (stem.substr(0, 3) != L"COM" && stem.substr(0, 3) != L"LPT")) return false;
    return (stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'¹' ||
        stem[3] == L'²' || stem[3] == L'³';
}

// "*.*" accepts any name; otherwise every "*.ext" segment must be a plain
// extension. The first one seeds the default extension.
bool FilterExtension(std::wstring_view pattern, std::wstring& extension) {
    if (pattern == L"*.*") return true;
    size_t start = 0;
    while (start < pattern.size()) {
        const auto end = pattern.find(L';', start);
        const auto part = pattern.substr(start, end == pattern.npos ? pattern.size() - start : end - start);
        if (part.size() < 3 || part.substr(0, 2) != L"*." || !Extension(part.substr(2))) return false;
        if (start == 0) extension = part.substr(2);
        if (end == pattern.npos) return true;
        start = end + 1;
    }
    return false;
}

std::wstring Native(const std::wstring& path) {
    return path.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}

bool Missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

} // namespace

SaveTargetResult ResolveSaveTarget(const SaveTargetRequest& request) {
    SaveTargetResult result;
    result.filter_index = request.filter_index;
    if ((!request.filters.empty() && request.filter_index >= request.filters.size()) ||
        (request.filters.empty() && request.filter_index != 0)) {
        result.state = SaveTargetState::InvalidFilter;
        return result;
    }
    const auto& name = request.filename;
    if (name.empty() || name.size() > 255 || !ValidText(name) || Reserved(name) ||
        name.find_first_of(L"<>:\"/\\|?*") != name.npos) {
        return result;
    }
    if (name.back() == L'.' || name.back() == L' ') {
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    std::wstring extension = request.default_extension;
    std::wstring selected_extension;
    if ((!extension.empty() && !Extension(extension)) ||
        (!request.filters.empty() &&
            !FilterExtension(request.filters[request.filter_index].pattern, selected_extension))) {
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    // A type change (or a filter with no caller default) supplies the suffix.
    if ((request.filter_changed || extension.empty()) && !selected_extension.empty())
        extension = selected_extension;
    std::wstring filename = name;
    if (filename.find(L'.') == filename.npos && !extension.empty()) filename += L'.' + extension;
    if (filename.size() > 255) return result;

    std::wstring directory = request.directory;
    std::replace(directory.begin(), directory.end(), L'/', L'\\');
    if (directory.rfind(L"\\\\?\\", 0) == 0) directory.erase(0, 4);
    if (directory.rfind(L"\\\\?\\UNC\\", 0) == 0) directory = L"\\\\" + directory.substr(8);
    while (directory.size() > 3 && directory.back() == L'\\') directory.pop_back();
    const bool drive = directory.size() >= 3 && std::iswalpha(directory[0]) &&
        directory[1] == L':' && directory[2] == L'\\';
    const auto server_end = directory.find(L'\\', 2);
    const bool unc = directory.rfind(L"\\\\", 0) == 0 && server_end != directory.npos &&
        server_end > 2 && server_end + 1 < directory.size();
    if ((!drive && !unc) || !ValidText(directory) ||
        directory.find_first_of(L"<>\"|?*") != directory.npos ||
        directory.find(L':', drive ? 2 : 0) != directory.npos) {
        result.state = SaveTargetState::InvalidDirectory;
        return result;
    }
    const auto parent = std::filesystem::path(directory).lexically_normal();
    result.path = (parent / filename).wstring();
    if (result.path.size() >= 32760) {
        result.path.clear();
        result.state = SaveTargetState::Unsupported;
        return result;
    }
    result.state = SaveTargetState::Resolved;
    return result;
}

SaveTargetResult ValidateSaveTarget(const SaveTargetRequest& request) {
    auto result = ResolveSaveTarget(request);
    if (result.state != SaveTargetState::Resolved) return result;
    const auto parent = std::filesystem::path(result.path).parent_path().wstring();
    const DWORD parent_attributes = GetFileAttributesW(Native(parent).c_str());
    if (parent_attributes == INVALID_FILE_ATTRIBUTES) {
        result.error = GetLastError();
        result.state = Missing(result.error) ? SaveTargetState::ParentMissing
                                             : SaveTargetState::IoError;
        return result;
    }
    if (!(parent_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.state = SaveTargetState::ParentNotDirectory;
        return result;
    }
    const DWORD attributes = GetFileAttributesW(Native(result.path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.error = GetLastError();
        result.state = result.error == ERROR_FILE_NOT_FOUND ? SaveTargetState::Ready
                                                           : SaveTargetState::IoError;
        if (result.state == SaveTargetState::Ready) result.error = ERROR_SUCCESS;
        return result;
    }
    result.target_exists = true;
    result.read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    result.state = attributes & FILE_ATTRIBUTE_DIRECTORY ? SaveTargetState::TargetDirectory
                                                         : SaveTargetState::ConfirmOverwrite;
    return result;
}

} // namespace luling::ui
