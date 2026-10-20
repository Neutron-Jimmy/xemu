//
// xemu User Interface
//
// file-browser.cc -- see file-browser.hh.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "file-browser.hh"
#include "common.hh"
#include "font-manager.hh"
#include <SDL3/SDL.h>
#include "ui/xemu-uwp.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

FileBrowser g_file_browser;

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string HumanSize(uintmax_t n)
{
    char buf[32];
    if (n >= (1ull << 30)) {
        snprintf(buf, sizeof buf, "%.1f GB", n / (double)(1ull << 30));
    } else if (n >= (1ull << 20)) {
        snprintf(buf, sizeof buf, "%.1f MB", n / (double)(1ull << 20));
    } else if (n >= 1024) {
        snprintf(buf, sizeof buf, "%.0f KB", n / 1024.0);
    } else {
        snprintf(buf, sizeof buf, "%u B", (unsigned)n);
    }
    return buf;
}

// The folder everything lives under: the app's LocalState on the Xbox (the
// UWP build's SDL_GetPrefPath), the user's pref path elsewhere.
std::string FileBrowser::Root()
{
    char *p = SDL_GetPrefPath("xemu", "xemu");
    std::string root = p ? p : ".";
    SDL_free(p);
    while (root.size() > 3 && (root.back() == '\\' || root.back() == '/')) {
        root.pop_back();
    }
    return root;
}

void FileBrowser::Open(Mode mode, const SDL_DialogFileFilter *filters,
                       int nfilters, const char *default_location,
                       Callback callback)
{
    m_mode = mode;
    m_callback = std::move(callback);
    m_title = mode == OpenFolder ? "Select a folder" :
              mode == SaveFile   ? "Save as" :
                                   "Select a file";
    m_exts.clear();
    m_filter_desc.clear();
    // The first filter is the one that matters ("Disc Image Files", "Image
    // Files", ...); a "*" anywhere in it means everything.
    if (filters && nfilters > 0 && filters[0].pattern) {
        m_filter_desc = filters[0].name ? filters[0].name : "";
        std::string pat = filters[0].pattern;
        size_t start = 0;
        while (start <= pat.size()) {
            size_t end = pat.find(';', start);
            std::string ext = Lower(pat.substr(start, end - start));
            if (ext == "*") {
                m_exts.clear();
                break;
            }
            if (!ext.empty()) {
                m_exts.push_back(ext);
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    // Start where the current setting points, if that folder can be read;
    // otherwise in xemu's own folder.
    m_dir = Root();
    m_save_name.clear();
    if (default_location && *default_location) {
        std::error_code ec;
        fs::path p(default_location);
        if (!fs::is_directory(p, ec)) {
            if (mode == SaveFile) {
                m_save_name = p.filename().string();
            }
            p = p.parent_path();
        }
        fs::directory_iterator probe(p, ec);
        if (!ec && fs::is_directory(p, ec)) {
            m_dir = p.string();
        }
    }
    if (mode == SaveFile && m_save_name.empty()) {
        char name[64];
        time_t t = time(nullptr);
        strftime(name, sizeof name, "xemu-%Y%m%d-%H%M%S", localtime(&t));
        m_save_name = name;
        if (!m_exts.empty()) {
            m_save_name += "." + m_exts[0];
        }
    }

    m_show_all = false;
    m_error.clear();
    m_locations.clear();         // drives come and go (USB): look again
    Refresh();
    m_open_requested = true;
}

bool FileBrowser::Matches(const std::string &name) const
{
    if (m_show_all || m_exts.empty()) {
        return true;
    }
    std::string ext = Lower(fs::path(name).extension().string());
    if (!ext.empty() && ext[0] == '.') {
        ext.erase(0, 1);
    }
    return std::find(m_exts.begin(), m_exts.end(), ext) != m_exts.end();
}

// The Locations page (m_dir empty): xemu's own folder, the console's Dev Mode
// storage, and every drive whose root can be listed -- USB drives appear as
// E:, F:, ... With broadFileSystemAccess (the package manifest) the console
// lets the plain file calls read D:\DevelopmentFiles (measured: probe test I).
void FileBrowser::RefreshLocations()
{
    m_locations.clear();
    m_locations.push_back({"xemu's folder (LocalState)", Root()});
    std::error_code ec;
    if (fs::is_directory("D:\\DevelopmentFiles", ec)) {
        m_locations.push_back({"Development Files (D:\\DevelopmentFiles)",
                               "D:\\DevelopmentFiles"});
    }
    for (char d = 'C'; d <= 'Z'; d++) {
        std::string root = std::string(1, d) + ":\\";
        std::error_code ec2;
        fs::directory_iterator it(root, ec2);
        if (!ec2) {
            m_locations.push_back({"Drive " + root, root});
        }
    }
    m_focus_list = true;
}

// One level up; from the top of a drive or a Locations entry, the Locations
// page.
void FileBrowser::GoUp()
{
    std::string here = Lower(m_dir);
    for (auto &loc : m_locations) {
        if (Lower(loc.second) == here) {
            m_dir.clear();
            m_error.clear();
            RefreshLocations();
            return;
        }
    }
    fs::path p(m_dir);
    fs::path parent = p.parent_path();
    std::error_code ec;
    if (parent.empty() || parent == p || !fs::is_directory(parent, ec)) {
        m_dir.clear();
        m_error.clear();
        RefreshLocations();
        return;
    }
    fs::directory_iterator probe(parent, ec);
    if (ec) {           // a folder above that the console will not list
        m_dir.clear();
        m_error.clear();
        RefreshLocations();
        return;
    }
    m_dir = parent.string();
    m_error.clear();
    Refresh();
}

void FileBrowser::Refresh()
{
    m_entries.clear();
    if (m_locations.empty()) {
        RefreshLocations();     // GoUp() needs them to know where tops are
    }
    std::error_code ec;
    for (fs::directory_iterator it(m_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        Entry e;
        e.name = it->path().filename().string();
        std::error_code ec2;
        e.dir = it->is_directory(ec2);
        e.size = e.dir ? 0 : it->file_size(ec2);
        if (ec2) {
            e.size = 0;
        }
        if (e.dir || m_mode != OpenFolder) {
            m_entries.push_back(std::move(e));
        }
    }
    if (ec) {
        m_error = "Cannot read this folder: " + ec.message();
    }
    std::sort(m_entries.begin(), m_entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.dir != b.dir) {
                      return a.dir;
                  }
                  return Lower(a.name) < Lower(b.name);
              });
    m_focus_list = true;
}

void FileBrowser::Finish(const std::string &path)
{
    Callback cb = std::move(m_callback);
    m_callback = nullptr;
    m_open = false;
    ImGui::CloseCurrentPopup();
    if (cb) {
        cb(path.c_str());
    }
}

void FileBrowser::Cancel()
{
    m_callback = nullptr;
    m_open = false;
    // Keep B from reaching the menu underneath: it would close it too
    m_wait_b_release = true;
    ImGui::CloseCurrentPopup();
}

void FileBrowser::Draw()
{
    static const char *popup_id = "###xemu_file_browser";

    if (m_wait_b_release) {
        // swallow the B press until it is released
        if (ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight) ||
            ImGui::IsKeyDown(ImGuiKey_Escape)) {
            // owned and locked: IsKeyDown()/IsKeyPressed() elsewhere (the
            // menus ask for ImGuiKeyOwner_Any) see it as up until released
            ImGuiID owner = ImHashStr("xemu_file_browser_b_lock");
            ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, owner,
                               ImGuiInputFlags_LockUntilRelease);
            ImGui::SetKeyOwner(ImGuiKey_Escape, owner,
                               ImGuiInputFlags_LockUntilRelease);
        } else {
            m_wait_b_release = false;
        }
    }

    if (m_open_requested) {
        m_open_requested = false;
        m_open = true;
        ImGui::OpenPopup(popup_id);
    }
    if (!m_open) {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImVec2 size(io.DisplaySize.x * 0.85f, io.DisplaySize.y * 0.85f);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,
                                   io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    std::string title = m_title + popup_id;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr,
                                ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        m_open = false;
        return;
    }

    bool at_top = m_dir.empty();         // the Locations page

    // Where we are
    ImGui::TextUnformatted(at_top ? "Locations" : m_dir.c_str());
    if (!m_filter_desc.empty() && m_mode != OpenFolder) {
        ImGui::SameLine();
        ImGui::TextDisabled("   %s%s", m_show_all ? "All files" :
                                                    m_filter_desc.c_str(),
                            m_exts.empty() ? "" : "   (Y: toggle)");
    }
    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
    }
    ImGui::Separator();

    // The actions that are not a list entry
    if (m_mode == OpenFolder && !at_top) {
        if (ImGui::Button("Use this folder")) {
            Finish(m_dir);
            ImGui::EndPopup();
            return;
        }
        ImGui::SameLine();
    } else if (m_mode == SaveFile && !at_top) {
        ImGui::SetNextItemWidth(size.x * 0.5f);
        char buf[256];
        snprintf(buf, sizeof buf, "%s", m_save_name.c_str());
        if (ImGui::InputText("###save_name", buf, sizeof buf)) {
            m_save_name = buf;
        }
        ImGui::SameLine();
        if (ImGui::Button("Save here") && !m_save_name.empty()) {
            Finish((fs::path(m_dir) / m_save_name).string());
            ImGui::EndPopup();
            return;
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel")) {
        Cancel();
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  A: open   B: %s   Y: all files",
                        at_top ? "cancel" : "up");

    // The list
    ImGui::BeginChild("###entries", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NavFlattened);
    int shown = 0;
    std::string chosen;
    bool chosen_dir = false;
    auto focus_first = [this]() {
        if (m_focus_list) {
            ImGui::SetItemDefaultFocus();
            ImGui::SetKeyboardFocusHere(-1);
            m_focus_list = false;
        }
    };
    if (at_top) {
        for (auto &loc : m_locations) {
            // No icon glyphs: the popup draws in ImGui's default font, which
            // has none (they came out as "?"). Folders end in a backslash.
            std::string label = loc.first + "###" + loc.second;
            if (ImGui::Selectable(label.c_str())) {
                chosen = loc.second;
                chosen_dir = true;
            }
            focus_first();
            shown++;
        }
    } else {
        if (ImGui::Selectable("..\\  (up)")) {
            chosen = "..";
            chosen_dir = true;
        }
        focus_first();
        shown++;
        for (const Entry &e : m_entries) {
            if (!e.dir && !Matches(e.name)) {
                continue;
            }
            std::string label = e.name + (e.dir ? "\\" : "") + "###" + e.name;
            if (ImGui::Selectable(label.c_str())) {
                chosen = e.name;
                chosen_dir = e.dir;
            }
            focus_first();
            if (!e.dir) {
                std::string sz = HumanSize(e.size);
                ImGui::SameLine(ImGui::GetContentRegionAvail().x -
                                ImGui::CalcTextSize(sz.c_str()).x);
                ImGui::TextDisabled("%s", sz.c_str());
            }
            shown++;
        }
        if (shown == 1) {
            ImGui::TextDisabled(m_mode == OpenFolder ?
                                    "(no folders here)" :
                                    "(no matching files here -- press Y to "
                                    "show all files)");
        }
    }
    ImGui::EndChild();

    // Act on the choice, then the buttons that are not widgets
    if (!chosen.empty()) {
        if (chosen == "..") {
            GoUp();
        } else if (at_top) {
            m_dir = chosen;          // a location: an absolute path
            m_error.clear();
            Refresh();
        } else if (chosen_dir) {
            m_dir = (fs::path(m_dir) / chosen).string();
            m_error.clear();
            Refresh();
        } else if (m_mode == SaveFile) {
            m_save_name = chosen;
        } else {
            Finish((fs::path(m_dir) / chosen).string());
            ImGui::EndPopup();
            return;
        }
    }

    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && !m_exts.empty() &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
        m_show_all = !m_show_all;
        m_focus_list = true;
    }
    if (focused && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        if (at_top) {
            Cancel();
        } else {
            GoUp();
        }
    }

    ImGui::EndPopup();
}
