#include "ctx_handlers.h"
#include "packaged_ctx_handlers.h"

#include "../common/path_utils.h"
#include "../ipc/ctx_menu_util.h"

#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <unordered_set>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

using luling::ipc::CleanMenuText;
using luling::ipc::IsBuiltinContextVerb;
using luling::ipc::IsDisabledHandler;
using luling::ipc::IsDroppedContextSubmenu;
using luling::ipc::KeepFlyoutParentWithoutLeaves;
using luling::ipc::kMaxSubmenuChildren;
using luling::ipc::ToLowerVerb;

namespace luling::shell {
namespace {

std::wstring ToParsingPath(std::wstring path) {
    return luling::path::StripExtendedPathPrefix(path);
}

std::wstring ClsidText(const CLSID& clsid) {
    wchar_t buf[64]{};
    if (StringFromGUID2(clsid, buf, ARRAYSIZE(buf)) <= 0) return {};
    return ToLowerVerb(buf);
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return {};
    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return {};
    struct LANGANDCODEPAGE { WORD language; WORD codepage; };
    LANGANDCODEPAGE* translate = nullptr;
    UINT translate_bytes = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translate), &translate_bytes) ||
        !translate || translate_bytes < sizeof(LANGANDCODEPAGE))
        return {};
    wchar_t query[64]{};
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
               translate[0].language, translate[0].codepage);
    wchar_t* desc = nullptr;
    UINT desc_bytes = 0;
    if (!VerQueryValueW(block.data(), query, reinterpret_cast<void**>(&desc), &desc_bytes) ||
        !desc || desc[0] == 0)
        return {};
    return desc;
}

std::wstring HandlerName(HKEY handlers, const wchar_t* subkey, const CLSID& clsid) {
    if (subkey && subkey[0] && subkey[0] != L'{') return subkey;
    wchar_t server[MAX_PATH]{};
    DWORD server_bytes = sizeof(server);
    std::wstring inproc = L"CLSID\\";
    inproc += ClsidText(clsid);
    inproc += L"\\InprocServer32";
    if (RegGetValueW(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, server,
                     &server_bytes) == ERROR_SUCCESS && server[0]) {
        wchar_t expanded[MAX_PATH]{};
        if (ExpandEnvironmentStringsW(server, expanded, ARRAYSIZE(expanded))) {
            auto desc = FileDescription(expanded);
            if (!desc.empty()) return desc;
        }
    }
    wchar_t named[256]{};
    DWORD named_bytes = sizeof(named);
    if (handlers && subkey &&
        RegGetValueW(handlers, subkey, nullptr, RRF_RT_REG_SZ, nullptr, named,
                     &named_bytes) == ERROR_SUCCESS && named[0] && named[0] != L'{')
        return named;
    return subkey && subkey[0] ? std::wstring(subkey) : ClsidText(clsid);
}

void AddHandlersFromKey(HKEY root, const wchar_t* relative,
                        std::vector<CtxHandlerDesc>& out,
                        std::unordered_set<std::wstring>& seen,
                        const std::vector<std::wstring>& disabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, relative, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD name_cch = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &name_cch, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
            break;
        wchar_t value[128]{};
        DWORD value_bytes = sizeof(value);
        const wchar_t* guid_text = name;
        if (RegGetValueW(key, name, nullptr, RRF_RT_REG_SZ, nullptr, value,
                         &value_bytes) == ERROR_SUCCESS && value[0] == L'{')
            guid_text = value;
        CLSID clsid{};
        if (FAILED(CLSIDFromString(guid_text, &clsid))) continue;
        const std::wstring text = ClsidText(clsid);
        if (text.empty() || !seen.insert(text).second) continue;
        if (IsDisabledHandler(text, disabled)) continue;
        CtxHandlerDesc desc;
        desc.clsid = clsid;
        desc.clsid_text = text;
        desc.name = HandlerName(key, name, clsid);
        out.push_back(std::move(desc));
    }
    RegCloseKey(key);
}

// Packaged (MSIX / sparse) verbs whose manifest item types match the target.
void AddPackagedHandlers(bool background, const std::wstring& path,
                         std::vector<CtxHandlerDesc>& out,
                         std::unordered_set<std::wstring>& seen,
                         const std::vector<std::wstring>& disabled) {
    const std::vector<std::wstring> types = PackagedItemTypesFor(background, path);
    for (const auto& verb : PackagedContextMenuVerbs()) {
        if (!PackagedVerbMatches(verb, types)) continue;
        if (!seen.insert(verb.clsid_text).second) continue;
        if (IsDisabledHandler(verb.clsid_text, disabled)) continue;
        CtxHandlerDesc desc;
        desc.clsid = verb.clsid;
        desc.clsid_text = verb.clsid_text;
        desc.name = verb.name;
        desc.explorer_command = true;
        out.push_back(std::move(desc));
    }
}

std::wstring ExtensionOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return ToLowerVerb(name.substr(dot));
}

std::wstring ProgIdForExt(const std::wstring& ext) {
    if (ext.empty()) return {};
    wchar_t progid[256]{};
    DWORD bytes = sizeof(progid);
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                     progid, &bytes) != ERROR_SUCCESS || !progid[0])
        return {};
    return progid;
}

std::wstring PerceivedType(const std::wstring& ext) {
    if (ext.empty()) return {};
    wchar_t type[128]{};
    DWORD bytes = sizeof(type);
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), L"PerceivedType", RRF_RT_REG_SZ,
                     nullptr, type, &bytes) != ERROR_SUCCESS || !type[0])
        return {};
    return type;
}

bool SafeGetVerbW(IContextMenu* menu, UINT offset, wchar_t* buf, UINT cch) {
    __try {
        return SUCCEEDED(menu->GetCommandString(offset, GCS_VERBW, nullptr,
                                                reinterpret_cast<CHAR*>(buf), cch));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT SafeQueryContextMenu(IContextMenu* menu, HMENU hmenu, UINT index,
                             UINT id_first, UINT id_last, UINT flags) {
    __try {
        return menu->QueryContextMenu(hmenu, index, id_first, id_last, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

std::wstring CtxVerbOf(IContextMenu* menu, UINT id, UINT id_first) {
    if (id < id_first) return {};
    wchar_t buf[128]{};
    if (SafeGetVerbW(menu, id - id_first, buf, ARRAYSIZE(buf) - 1)) return buf;
    return {};
}

// The row's MIIM_BITMAP as premultiplied BGRA, the way Explorer paints it
// next to the label. Handlers hand out 32bpp ARGB bitmaps (premultiplied per
// the Vista menu guidance; straight alpha is premultiplied here), older ones
// opaque DDBs. HBMMENU_* stock ids and owner-draw callbacks carry no pixels.
common::MenuIconRef MenuItemIcon(HMENU menu, UINT pos) {
    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_BITMAP;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mii) || !mii.hbmpItem ||
        mii.hbmpItem == HBMMENU_CALLBACK ||
        reinterpret_cast<ULONG_PTR>(mii.hbmpItem) <=
            reinterpret_cast<ULONG_PTR>(HBMMENU_POPUP_MINIMIZE))
        return nullptr;
    BITMAP info{};
    if (!GetObjectW(mii.hbmpItem, sizeof(info), &info) || info.bmWidth <= 0 ||
        info.bmHeight <= 0 ||
        info.bmWidth > static_cast<LONG>(common::kMaxMenuIconEdge) ||
        info.bmHeight > static_cast<LONG>(common::kMaxMenuIconEdge))
        return nullptr;
    auto pixels = std::make_shared<common::MenuIconPixels>();
    pixels->width = static_cast<uint32_t>(info.bmWidth);
    pixels->height = static_cast<uint32_t>(info.bmHeight);
    pixels->bgra.resize(static_cast<size_t>(pixels->width) * pixels->height * 4u);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = info.bmWidth;
    bi.bmiHeader.biHeight = -info.bmHeight;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    const int lines = GetDIBits(dc, mii.hbmpItem, 0, static_cast<UINT>(info.bmHeight),
                                pixels->bgra.data(), &bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (lines != info.bmHeight) return nullptr;
    auto& bgra = pixels->bgra;
    bool any_alpha = false, straight = false;
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const uint8_t a = bgra[i + 3];
        if (a) any_alpha = true;
        if (bgra[i] > a || bgra[i + 1] > a || bgra[i + 2] > a) straight = true;
    }
    if (info.bmBitsPixel != 32 || !any_alpha) {
        for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    } else if (straight) {
        for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
            const uint32_t a = bgra[i + 3];
            for (size_t c = 0; c < 3; ++c)
                bgra[i + c] = static_cast<uint8_t>((bgra[i + c] * a + 127u) / 255u);
        }
    }
    return pixels;
}

// 16 DIP at the system DPI; the menu scales the few-percent mismatch on a
// mixed-DPI desk.
uint32_t MenuIconEdgePx() {
    int dpi = 96;
    if (HDC screen = GetDC(nullptr)) {
        dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }
    const int px = MulDiv(16, dpi > 0 ? dpi : 96, 96);
    return static_cast<uint32_t>(std::clamp(px, 16, static_cast<int>(common::kMaxMenuIconEdge)));
}

// HICON -> premultiplied BGRA (common/menu_icon.h). Icons with an alpha channel
// use it; classic ones take the AND mask.
common::MenuIconRef IconPixels(HICON icon) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return nullptr;
    common::MenuIconRef result;
    BITMAP color{};
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(color), &color) &&
        color.bmWidth > 0 && color.bmHeight > 0 &&
        color.bmWidth <= static_cast<LONG>(common::kMaxMenuIconEdge) &&
        color.bmHeight <= static_cast<LONG>(common::kMaxMenuIconEdge)) {
        auto pixels = std::make_shared<common::MenuIconPixels>();
        pixels->width = static_cast<uint32_t>(color.bmWidth);
        pixels->height = static_cast<uint32_t>(color.bmHeight);
        const size_t count = static_cast<size_t>(pixels->width) * pixels->height;
        pixels->bgra.resize(count * 4u);
        std::vector<uint8_t> mask(count * 4u);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = color.bmWidth;
        bi.bmiHeader.biHeight = -color.bmHeight;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (HDC dc = CreateCompatibleDC(nullptr)) {
            const UINT rows = static_cast<UINT>(color.bmHeight);
            const bool ok = GetDIBits(dc, info.hbmColor, 0, rows, pixels->bgra.data(), &bi,
                                      DIB_RGB_COLORS) == color.bmHeight;
            const bool has_mask = info.hbmMask &&
                GetDIBits(dc, info.hbmMask, 0, rows, mask.data(), &bi, DIB_RGB_COLORS) ==
                    color.bmHeight;
            DeleteDC(dc);
            if (ok) {
                auto& bgra = pixels->bgra;
                bool any_alpha = false;
                for (size_t i = 3; i < bgra.size(); i += 4) any_alpha = any_alpha || bgra[i] != 0;
                if (!any_alpha) {
                    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
                        const bool opaque = !has_mask || mask[i] == 0;
                        bgra[i + 3] = opaque ? 255 : 0;
                        if (!opaque) bgra[i] = bgra[i + 1] = bgra[i + 2] = 0;
                    }
                } else {
                    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
                        const uint32_t a = bgra[i + 3];
                        if (a == 255) continue;
                        for (size_t c = 0; c < 3; ++c)
                            bgra[i + c] = static_cast<uint8_t>((bgra[i + c] * a + 127u) / 255u);
                    }
                }
                result = std::move(pixels);
            }
        }
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return result;
}

// A packaged command's GetIcon reference ("C:\...\app.exe,-101" or
// "%ProgramFiles%\app.exe") as pixels. ms-resource references cannot be
// resolved here and stay without an icon.
common::MenuIconRef MenuIconFromReference(const std::wstring& reference) {
    if (reference.empty() || reference[0] == L'@') return nullptr;
    wchar_t buffer[MAX_PATH * 2]{};
    wcsncpy_s(buffer, reference.c_str(), _TRUNCATE);
    int index = PathParseIconLocationW(buffer);
    PathUnquoteSpacesW(buffer);
    wchar_t expanded[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(buffer, expanded, ARRAYSIZE(expanded));
    const std::wstring file = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : buffer;
    if (file.empty()) return nullptr;
    HICON icon = nullptr;
    const uint32_t edge = MenuIconEdgePx();
    if (FAILED(SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr,
                                 MAKELONG(edge, edge))) || !icon)
        return nullptr;
    common::MenuIconRef pixels = IconPixels(icon);
    DestroyIcon(icon);
    return pixels;
}

std::wstring MenuItemText(HMENU menu, UINT pos) {
    wchar_t buf[512]{};
    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = buf;
    mii.cch = ARRAYSIZE(buf) - 1;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mii)) return {};
    return CleanMenuText(buf);
}

constexpr ULONGLONG kNestedFlyoutBudgetMs = 80;
constexpr CLSID kSendToHandler = {0x7BA4C740, 0x9E81, 0x11CF,
                                 {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};

void InitMenuPopup(IContextMenu2* menu2, IContextMenu3* menu3, HMENU submenu, UINT pos) {
    if (!submenu) return;
    __try {
        if (menu3) {
            LRESULT ignored = 0;
            menu3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
                                  MAKELPARAM(pos, FALSE), &ignored);
        } else if (menu2) {
            menu2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
                                 MAKELPARAM(pos, FALSE));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void CollectSubmenuLeaves(IContextMenu* menu, IContextMenu2* menu2, IContextMenu3* menu3,
                          HMENU submenu, UINT pos, bool background, UINT id_first,
                          bool parent_enabled, int depth, ULONGLONG deadline,
                          std::vector<CtxItemOut>& kids) {
    if (GetTickCount64() < deadline) InitMenuPopup(menu2, menu3, submenu, pos);
    const int sub_count = GetMenuItemCount(submenu);
    for (int j = 0; j < sub_count && static_cast<int>(kids.size()) < kMaxSubmenuChildren; ++j) {
        MENUITEMINFOW sub{ sizeof(sub) };
        sub.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(submenu, j, TRUE, &sub)) continue;
        if (sub.fType & MFT_SEPARATOR) continue;
        if (sub.hSubMenu) {
            if (depth >= 1) continue;
            const bool nested_enabled =
                parent_enabled && !(sub.fState & (MFS_DISABLED | MFS_GRAYED));
            CollectSubmenuLeaves(menu, menu2, menu3, sub.hSubMenu, static_cast<UINT>(j),
                                 background, id_first, nested_enabled, depth + 1, deadline,
                                 kids);
            continue;
        }
        const std::wstring child_text = MenuItemText(submenu, static_cast<UINT>(j));
        if (child_text.empty()) continue;
        CtxItemOut item;
        item.id = sub.wID;
        item.enabled = parent_enabled && !(sub.fState & (MFS_DISABLED | MFS_GRAYED));
        item.child = true;
        item.verb = CtxVerbOf(menu, sub.wID, id_first);
        if (IsBuiltinContextVerb(item.verb, background)) continue;
        item.text = child_text;
        item.icon = MenuItemIcon(submenu, static_cast<UINT>(j));
        kids.push_back(std::move(item));
    }
}

} // namespace

CtxBind::~CtxBind() {
    if (data) data->Release();
    if (folder) CoTaskMemFree(folder);
    if (assoc) RegCloseKey(assoc);
}

void ReleaseHandlerSlot(CtxHandlerSlot& slot) noexcept {
    if (slot.hmenu) DestroyMenu(slot.hmenu);
    if (slot.menu3) slot.menu3->Release();
    if (slot.menu2) slot.menu2->Release();
    if (slot.menu) slot.menu->Release();
    slot.hmenu = nullptr;
    slot.menu3 = nullptr;
    slot.menu2 = nullptr;
    slot.menu = nullptr;
}

std::vector<CtxHandlerDesc> EnumerateCtxHandlers(
    bool background, const std::wstring& path,
    const std::vector<std::wstring>& disabled_clsids) {
    std::vector<CtxHandlerDesc> out;
    std::unordered_set<std::wstring> seen;
    auto add = [&](const wchar_t* relative) {
        AddHandlersFromKey(HKEY_CLASSES_ROOT, relative, out, seen, disabled_clsids);
    };
    if (background) {
        add(L"Directory\\Background\\shellex\\ContextMenuHandlers");
        add(L"Directory\\shellex\\ContextMenuHandlers");
        add(L"Folder\\shellex\\ContextMenuHandlers");
        add(L"LibraryFolder\\Background\\shellex\\ContextMenuHandlers");
        add(L"Drive\\shellex\\ContextMenuHandlers");
        add(L"*\\shellex\\ContextMenuHandlers");
        AddPackagedHandlers(true, path, out, seen, disabled_clsids);
        return out;
    }
    add(L"*\\shellex\\ContextMenuHandlers");
    add(L"AllFilesystemObjects\\shellex\\ContextMenuHandlers");
    add(L"Folder\\shellex\\ContextMenuHandlers");
    add(L"Directory\\shellex\\ContextMenuHandlers");
    add(L"Drive\\shellex\\ContextMenuHandlers");
    const std::wstring ext = ExtensionOf(path);
    if (!ext.empty()) {
        const std::wstring assoc = L"SystemFileAssociations\\" + ext +
            L"\\shellex\\ContextMenuHandlers";
        add(assoc.c_str());
        const std::wstring progid = ProgIdForExt(ext);
        if (!progid.empty()) {
            const std::wstring key = progid + L"\\shellex\\ContextMenuHandlers";
            add(key.c_str());
        }
        const std::wstring perceived = PerceivedType(ext);
        if (!perceived.empty()) {
            const std::wstring key = L"SystemFileAssociations\\" + perceived +
                L"\\shellex\\ContextMenuHandlers";
            add(key.c_str());
        }
    }
    AddPackagedHandlers(false, path, out, seen, disabled_clsids);
    return out;
}

bool BindCtxSelection(const std::vector<std::wstring>& paths, bool background,
                      CtxBind& out) {
    if (paths.empty()) return false;
    if (background) {
        if (FAILED(SHParseDisplayName(ToParsingPath(paths.front()).c_str(), nullptr,
                                      &out.folder, 0, nullptr)) || !out.folder)
            return false;
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromIDList(out.folder, IID_PPV_ARGS(&folder))) && folder) {
            folder->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&out.data));
            folder->Release();
        }
        RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Directory\\Background", 0, KEY_READ, &out.assoc);
        return true;
    }

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(ToParsingPath(p).c_str(), nullptr, &pidl, 0,
                                         nullptr)) && pidl)
            pidls.push_back(pidl);
    }
    if (pidls.empty()) return false;
    IShellItemArray* array = nullptr;
    HRESULT hr = SHCreateShellItemArrayFromIDLists(
        static_cast<UINT>(pidls.size()),
        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array);
    if (SUCCEEDED(hr) && array) {
        array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&out.data));
        IShellItem* first = nullptr;
        if (SUCCEEDED(array->GetItemAt(0, &first)) && first) {
            IShellItem* parent = nullptr;
            if (SUCCEEDED(first->GetParent(&parent)) && parent) {
                SHGetIDListFromObject(parent, &out.folder);
                parent->Release();
            }
            first->Release();
        }
        array->Release();
    }
    for (auto pidl : pidls) CoTaskMemFree(pidl);
    const std::wstring ext = ExtensionOf(paths.front());
    const std::wstring progid = ProgIdForExt(ext);
    if (!progid.empty())
        RegOpenKeyExW(HKEY_CLASSES_ROOT, progid.c_str(), 0, KEY_READ, &out.assoc);
    else if (!ext.empty())
        RegOpenKeyExW(HKEY_CLASSES_ROOT, ext.c_str(), 0, KEY_READ, &out.assoc);
    else
        RegOpenKeyExW(HKEY_CLASSES_ROOT, L"*", 0, KEY_READ, &out.assoc);
    return out.data != nullptr;
}

HRESULT QueryOneHandler(const CtxHandlerDesc& handler, const CtxBind& bind,
                        HWND owner, UINT id_first, UINT id_last, UINT qcm_flags,
                        CtxHandlerSlot& slot) {
    slot = {};
    slot.id_first = id_first;
    slot.id_last = id_last;
    slot.clsid = handler.clsid_text;
    slot.name = handler.name;

    HRESULT hr = E_FAIL;
    if (handler.explorer_command) {
        // Packaged verbs are IExplorerCommand servers, usually out of process
        // (dllhost surrogate); the selection is the bound data object, which is
        // the folder itself for a background click.
        IExplorerCommand* command = nullptr;
        hr = CoCreateInstance(handler.clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                              IID_PPV_ARGS(&command));
        if (FAILED(hr) || !command) return FAILED(hr) ? hr : E_FAIL;
        IShellItemArray* items = nullptr;
        if (bind.data)
            SHCreateShellItemArrayFromDataObject(bind.data, IID_PPV_ARGS(&items));
        hr = items ? CreateExplorerCommandMenu(command, items, &slot.menu) : E_FAIL;
        if (SUCCEEDED(hr) && items) {
            // A packaged verb reports its icon through GetIcon (a file,index
            // reference); its menu rows carry no MIIM_BITMAP of their own.
            LPWSTR icon_reference = nullptr;
            if (SUCCEEDED(command->GetIcon(items, &icon_reference)) && icon_reference) {
                slot.command_icon = MenuIconFromReference(icon_reference);
                CoTaskMemFree(icon_reference);
            }
        }
        if (items) items->Release();
        command->Release();
    } else {
        IUnknown* unk = nullptr;
        hr = CoCreateInstance(handler.clsid, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IUnknown, reinterpret_cast<void**>(&unk));
        if (FAILED(hr) || !unk) return FAILED(hr) ? hr : E_FAIL;

        IShellExtInit* init = nullptr;
        if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&init))) && init) {
            init->Initialize(bind.folder, bind.data, bind.assoc);
            init->Release();
        }
        hr = unk->QueryInterface(IID_PPV_ARGS(&slot.menu));
        unk->Release();
    }
    if (FAILED(hr) || !slot.menu) {
        slot.menu = nullptr;
        return FAILED(hr) ? hr : E_FAIL;
    }

    slot.hmenu = CreatePopupMenu();
    if (!slot.hmenu) {
        slot.menu->Release();
        slot.menu = nullptr;
        return E_OUTOFMEMORY;
    }
    // QueryContextMenu takes no owner window; the owner only matters when the
    // verb is invoked, which CtxInvoke does with our own message window.
    (void)owner;
    hr = SafeQueryContextMenu(slot.menu, slot.hmenu, 0, id_first, id_last, qcm_flags);
    if (FAILED(hr)) {
        ReleaseHandlerSlot(slot);
        return hr;
    }
    if (handler.clsid == kSendToHandler && bind.data) {
        // The standalone SendTo extension exposes only a placeholder on modern
        // Windows. The default Shell menu supplies the services that populate
        // and invoke its destinations. Keep the CLSID-provided localized title
        // to isolate that submenu without hardcoding any display language.
        // Prefer the placeholder flyout. Some Windows builds add the
        // placeholder as a plain row instead; it used to slip through as an
        // inert "Send to" that did nothing when clicked (#77).
        int plain_row = -1;
        for (int i = 0; i < GetMenuItemCount(slot.hmenu); ++i) {
            if (GetSubMenu(slot.hmenu, i)) {
                slot.send_to_title = MenuItemText(slot.hmenu, static_cast<UINT>(i));
                break;
            }
            MENUITEMINFOW row{ sizeof(row) };
            row.fMask = MIIM_FTYPE;
            if (plain_row < 0 && GetMenuItemInfoW(slot.hmenu, i, TRUE, &row) &&
                !(row.fType & MFT_SEPARATOR) &&
                !MenuItemText(slot.hmenu, static_cast<UINT>(i)).empty())
                plain_row = i;
        }
        if (slot.send_to_title.empty() && plain_row >= 0)
            slot.send_to_title = MenuItemText(slot.hmenu, static_cast<UINT>(plain_row));
        if (!slot.send_to_title.empty()) {
            IShellItemArray* selection = nullptr;
            IContextMenu* native = nullptr;
            HRESULT native_hr = SHCreateShellItemArrayFromDataObject(bind.data, IID_PPV_ARGS(&selection));
            if (SUCCEEDED(native_hr)) {
                native_hr = selection->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&native));
                selection->Release();
            }
            HMENU native_popup = native ? CreatePopupMenu() : nullptr;
            if (native_popup) {
                // No CMF_SYNCCASCADEMENU: synchronously initializing every
                // cascade turns this worker into a serial shadow query of the
                // whole default menu, making it structurally the slowest
                // worker on machines with legacy extensions (#77 regression).
                // CollectHandlerItems fills the Send to flyout through
                // InitMenuPopup with its own budget instead.
                native_hr = SafeQueryContextMenu(native, native_popup, 0, id_first, id_last,
                                                 qcm_flags);
                if (SUCCEEDED(native_hr)) {
                    ReleaseHandlerSlot(slot);
                    slot.menu = native;
                    slot.hmenu = native_popup;
                    native = nullptr;
                    native_popup = nullptr;
                }
            }
            if (native_popup) DestroyMenu(native_popup);
            if (native) native->Release();
        }
    }
    slot.menu->QueryInterface(IID_PPV_ARGS(&slot.menu3));
    slot.menu->QueryInterface(IID_PPV_ARGS(&slot.menu2));
    return S_OK;
}

void CollectHandlerItems(const CtxHandlerSlot& slot, bool background,
                         std::vector<CtxItemOut>& out) {
    if (!slot.menu || !slot.hmenu) return;
    // WM_INITMENUPOPUP is where most handlers add their row bitmaps and fill
    // placeholder rows; Explorer sends it before showing the menu, and a row
    // without it comes back empty or plain.
    InitMenuPopup(slot.menu2, slot.menu3, slot.hmenu, 0);
    const int count = GetMenuItemCount(slot.hmenu);
    bool pending_separator = false;
    auto push = [&](CtxItemOut item) {
        if (item.text.empty()) return;
        item.clsid = slot.clsid;
        item.handler = slot.name;
        // A packaged verb's rows carry no bitmap; its GetIcon result stands in.
        if (!item.icon) item.icon = slot.command_icon;
        if (pending_separator && !out.empty()) {
            out.back().separator_after = true;
            pending_separator = false;
        }
        out.push_back(std::move(item));
    };
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{ sizeof(mii) };
        mii.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(slot.hmenu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) {
            if (!slot.send_to_title.empty()) continue;
            if (!out.empty()) pending_separator = true;
            continue;
        }
        const bool enabled = !(mii.fState & (MFS_DISABLED | MFS_GRAYED));
        const std::wstring title = MenuItemText(slot.hmenu, static_cast<UINT>(i));
        if (!slot.send_to_title.empty() && title != slot.send_to_title &&
            !(mii.hSubMenu &&
              luling::ipc::ToLowerVerb(CtxVerbOf(slot.menu, mii.wID, slot.id_first)) == L"sendto"))
            continue;
        if (mii.hSubMenu) {
            const std::wstring parent_verb = slot.send_to_title.empty()
                ? CtxVerbOf(slot.menu, mii.wID, slot.id_first) : L"sendto";
            if (IsBuiltinContextVerb(parent_verb, background) ||
                IsDroppedContextSubmenu(parent_verb)) continue;
            const std::wstring& parent_text = title;
            if (parent_text.empty()) continue;
            const bool send_to = !slot.send_to_title.empty() ||
                                 luling::ipc::ToLowerVerb(parent_verb) == L"sendto";
            std::vector<CtxItemOut> kids;
            // Each flyout gets its own budget. A shared one let a slow cascade
            // above exhaust it, so flyouts that fill themselves on
            // WM_INITMENUPOPUP (Send to, Bandizip's 压缩为…) came back empty and
            // were dropped whole (#77).
            CollectSubmenuLeaves(slot.menu, slot.menu2, slot.menu3, mii.hSubMenu,
                                 static_cast<UINT>(i), background, slot.id_first, enabled, 0,
                                 GetTickCount64() + kNestedFlyoutBudgetMs, kids);
            if (kids.empty()) {
                if (!KeepFlyoutParentWithoutLeaves(mii.wID, slot.id_first, slot.id_last))
                    continue;
                CtxItemOut item;
                item.id = mii.wID;
                // An empty Send to flyout is not an invokable destination.
                item.enabled = enabled && !send_to;
                item.verb = parent_verb;
                item.text = parent_text;
                item.icon = MenuItemIcon(slot.hmenu, static_cast<UINT>(i));
                push(std::move(item));
                continue;
            }
            CtxItemOut header;
            header.id = 0;
            header.enabled = enabled;
            header.has_children = true;
            header.verb = parent_verb;
            header.text = parent_text;
            header.icon = MenuItemIcon(slot.hmenu, static_cast<UINT>(i));
            push(std::move(header));
            for (auto& k : kids) {
                k.clsid = slot.clsid;
                k.handler = slot.name;
                if (!k.icon) k.icon = slot.command_icon;
                out.push_back(std::move(k));
            }
            continue;
        }
        if (mii.wID < slot.id_first || mii.wID > slot.id_last) continue;
        const std::wstring verb = CtxVerbOf(slot.menu, mii.wID, slot.id_first);
        if (IsBuiltinContextVerb(verb, background)) continue;
        CtxItemOut item;
        item.id = mii.wID;
        // A Send to row without its flyout cannot send anywhere (#77).
        item.enabled = enabled && slot.send_to_title.empty();
        item.verb = verb;
        item.text = MenuItemText(slot.hmenu, static_cast<UINT>(i));
        item.icon = MenuItemIcon(slot.hmenu, static_cast<UINT>(i));
        push(std::move(item));
    }
}

uint32_t FindItemId(const std::vector<CtxItemOut>& items, uint32_t id,
                    const std::wstring& verb, const std::wstring& text) {
    if (id != 0) {
        for (const auto& item : items)
            if (item.id == id) return item.id;
    }
    if (!verb.empty()) {
        for (const auto& item : items)
            if (!item.has_children && item.id != 0 && item.verb == verb) return item.id;
    }
    if (!text.empty()) {
        for (const auto& item : items)
            if (!item.has_children && item.id != 0 && item.text == text) return item.id;
    }
    return 0;
}

} // namespace luling::shell
