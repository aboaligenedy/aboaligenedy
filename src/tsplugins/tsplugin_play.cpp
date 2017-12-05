//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2017, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  Transport stream processor shared library:
//  Play resulting TS in any supported media player, as found on the system.
//
//----------------------------------------------------------------------------

#include "tsPlugin.h"
#include "tsForkPipe.h"
#include "tsSysUtils.h"
#include "tsRegistry.h"
TSDUCK_SOURCE;

// Pipe buffer size is used on Windows only.
#define PIPE_BUFFER_SIZE 65536


//----------------------------------------------------------------------------
// Plugin definition
//----------------------------------------------------------------------------

namespace ts {
    class PlayPlugin: public OutputPlugin
    {
    public:
        // Implementation of plugin API
        PlayPlugin(TSP*);
        virtual bool start() override;
        virtual bool stop() override;
        virtual bool send(const TSPacket*, size_t) override;

    private:
        bool     _use_mplayer;
        bool     _use_xine;
        ForkPipe _pipe;

        // Search a file in a search path. Return true is found
        bool searchInPath(UString& result, const UStringVector& path, const UString& name);

        // Inaccessible operations
        PlayPlugin() = delete;
        PlayPlugin(const PlayPlugin&) = delete;
        PlayPlugin& operator=(const PlayPlugin&) = delete;
    };
}

TSPLUGIN_DECLARE_VERSION
TSPLUGIN_DECLARE_OUTPUT(ts::PlayPlugin)


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::PlayPlugin::PlayPlugin(TSP* tsp_) :
    OutputPlugin(tsp_, u"Play output TS on any supported media player in the system.", u"[options]"),
    _use_mplayer(false),
    _use_xine(false),
    _pipe()
{
    option(u"mplayer", 'm');
    option(u"xine",    'x');

    setHelp(u"Options:\n"
            u"\n"
            u"  --help\n"
            u"      Display this help text.\n"
#if !defined(TS_WINDOWS)
            u"\n"
            u"  -m\n"
            u"  --mplayer\n"
            u"      Use mplayer for rendering. The default is to look for vlc, mplayer and\n"
            u"      xine, in this order, and use the first available one.\n"
#endif
            u"\n"
            u"  --version\n"
            u"      Display the version number.\n"
#if !defined(TS_WINDOWS)
            u"\n"
            u"  -x\n"
            u"  --xine\n"
            u"      Use xine for rendering. The default is to look for vlc, mplayer and\n"
            u"      xine, in this order, and use the first available one.\n"
#endif
    );
}


//----------------------------------------------------------------------------
// Output stop method
//----------------------------------------------------------------------------

bool ts::PlayPlugin::stop()
{
    return _pipe.close(*tsp);
}


//----------------------------------------------------------------------------
// Output method
//---   -------------------------------------------------------------------------

bool ts::PlayPlugin::send(const TSPacket* buffer, size_t packet_count)
{
    return _pipe.write(buffer, PKT_SIZE * packet_count, *tsp);
}


//----------------------------------------------------------------------------
// Search a file in a search path. Return empty string if not found
//----------------------------------------------------------------------------

bool ts::PlayPlugin::searchInPath(UString& result, const UStringVector& path, const UString& name)
{
    for (UStringVector::const_iterator it = path.begin(); it != path.end(); ++it) {
        if (!it->empty()) {
            result = *it + PathSeparator + name;
            tsp->debug(u"looking for %s", {result});
            if (FileExists(result)) {
                return true;
            }
        }
    }
    return false;
}


//----------------------------------------------------------------------------
// Output start method
//----------------------------------------------------------------------------

bool ts::PlayPlugin::start()
{
    // Get option values
    _use_mplayer = present(u"mplayer");
    _use_xine = present(u"xine");
    if (_use_mplayer && _use_xine) {
        tsp->error(u"--mplayer (-m) and --xine (-x) are mutually exclusive");
        return false;
    }

    // Command to execute will be built here
    UString command;

#if defined (TS_WINDOWS)

    // On Windows, VLC is the only known media player that can read an MPEG
    // transport stream on its standard input. Try to locate vlc.exe using
    // various means.

    // Get environment path
    UStringVector search_path;
    GetEnvironmentPath(search_path, TS_COMMAND_PATH);

    // Look into some registry location
    UString ent = Registry::GetValue(u"HKLM\\SOFTWARE\\VideoLAN\\VLC", u"InstallDir");
    if (!ent.empty()) {
        search_path.push_back(ent);
    }
    ent = Registry::GetValue(u"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VLC media player", u"UninstallString");
    if (!ent.empty()) {
        search_path.push_back(DirectoryName(ent));
    }

    // Add default installation locations
    search_path.push_back(u"C:\\Program Files\\VideoLAN\\VLC");
    search_path.push_back(u"C:\\Program Files (x86)\\VideoLAN\\VLC");

    // Then search vlc.exe in these locations
    UString exec;
    if (searchInPath(exec, search_path, u"vlc.exe")) {
        // Enclose the executable path with quotes and use "-" as parameter (meaning standard input).
        command = u"\"" + exec + u"\" -";
    }
    else {
        tsp->error(u"VLC not found, install VideoLAN VLC media player, see http://www.videolan.org/vlc/");
        return false;
    }

#else // UNIX

    // Get environment path.
    UStringVector search_path;
    GetEnvironmentPath(search_path, TS_COMMAND_PATH);

    // Executable names for various players
    static const UChar vlc_exec[] = u"vlc";
    static const UChar mplayer_exec[] = u"mplayer";
    static const UChar xine_exec[] = u"xine";

    // Options to read TS on stdin for various players
    static const UChar vlc_opts[] = u"-";
    static const UChar mplayer_opts[] = u"-demuxer +mpegts -";
    static const UChar xine_opts[] = u"stdin:/#demux:mpeg-ts";

    // Search known media players
    UString exec;
    const UChar* opts = u"";

    if (_use_mplayer) {
        opts = mplayer_opts;
        if (!searchInPath(exec, search_path, mplayer_exec)) {
            tsp->error(u"mplayer not found in PATH");
            return false;
        }
    }
    else if (_use_xine) {
        opts = xine_opts;
        if (!searchInPath(exec, search_path, xine_exec)) {
            tsp->error(u"xine not found in PATH");
            return false;
        }
    }
    else if (searchInPath(exec, search_path, vlc_exec)) {
        opts = vlc_opts;
    }
    else if (searchInPath(exec, search_path, mplayer_exec)) {
        opts = mplayer_opts;
    }
    else if (searchInPath(exec, search_path, xine_exec)) {
        opts = xine_opts;
    }
    else {
        tsp->error(u"no supported media player was found");
        return false;
    }

    command = u"\"" + exec + u"\" " + opts;

#endif

    // Create pipe & process
    tsp->verbose(u"using media player command: %s", {command});
    _pipe.setIgnoreAbort(false);
    return _pipe.open(command, true, PIPE_BUFFER_SIZE, *tsp);
}
