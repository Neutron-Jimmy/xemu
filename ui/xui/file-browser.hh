//
// xemu User Interface
//
// file-browser.hh -- an in-app file browser, drawn with ImGui, for platforms
// where the native file dialog cannot open: the Xbox (UWP) build, which has
// no Win32 windows at all. Its top is a Locations page: the app's LocalState,
// D:\DevelopmentFiles (the console's Dev Mode storage, readable with the
// package's broadFileSystemAccess) and any listable drive (USB: E:, F:, ...).
// It works with the controller like the rest of the HUD: A opens or picks,
// B goes up (to Locations, then cancels), Y shows all files.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <functional>
#include <string>
#include <vector>
#include <SDL3/SDL_dialog.h>

class FileBrowser
{
public:
    enum Mode { OpenFile, SaveFile, OpenFolder };
    using Callback = std::function<void(const char *path)>;

    void Open(Mode mode, const SDL_DialogFileFilter *filters, int nfilters,
              const char *default_location, Callback callback);
    void Draw(); // once per HUD frame, under the main loop lock

private:
    struct Entry {
        std::string name;
        bool dir;
        uintmax_t size;
    };

    void Refresh();
    void RefreshLocations();
    void GoUp();
    bool Matches(const std::string &name) const;
    void Finish(const std::string &path);
    void Cancel();
    std::string Root();

    bool m_open = false;
    bool m_open_requested = false;
    bool m_wait_b_release = false;
    bool m_show_all = false;
    bool m_focus_list = false;
    Mode m_mode = OpenFile;
    std::string m_title;
    std::string m_dir;                    // current folder, absolute; empty = Locations
    std::vector<std::pair<std::string, std::string>> m_locations; // label, path
    std::string m_save_name;              // SaveFile: the file name to create
    std::vector<std::string> m_exts;      // lower case, no dot; empty = all
    std::string m_filter_desc;
    std::vector<Entry> m_entries;
    std::string m_error;
    Callback m_callback;
};

extern FileBrowser g_file_browser;
