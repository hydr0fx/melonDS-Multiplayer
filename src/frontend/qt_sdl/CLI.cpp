/*
    Copyright 2021-2023 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QStringList>

#include "CLI.h"
#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace CLI
{

CommandLineOptions* ManageArgs(QApplication& melon)
{
    QCommandLineParser parser;
    parser.addHelpOption();

    parser.addPositionalArgument("nds", "Nintendo DS ROM (or an archive file which contains it) to load into Slot-1");
    parser.addPositionalArgument("gba", "GBA ROM (or an archive file which contains it) to load into Slot-2");

    parser.addOption(QCommandLineOption({"b", "boot"}, "Whether to boot firmware on startup. Defaults to \"auto\" (boot if NDS rom given)", "auto/always/never", "auto"));
    parser.addOption(QCommandLineOption({"f", "fullscreen"}, "Start melonDS in fullscreen mode"));

    parser.addOption(QCommandLineOption("netsync", "Netplay mode (host/client)", "mode"));
    parser.addOption(QCommandLineOption("netsync-addr", "Netplay host address or Room-ID", "addr"));
    parser.addOption(QCommandLineOption("netsync-port", "Netplay UDP port", "port"));
    parser.addOption(QCommandLineOption("netsync-players", "Max players (2-4 or auto)", "players"));
    parser.addOption(QCommandLineOption("netsync-delay", "Input delay frames (0=auto, 1-30)", "delay"));
    parser.addOption(QCommandLineOption("netsync-name", "Player display name", "name"));
    parser.addOption(QCommandLineOption("netsync-room", "Lobby room name", "room"));
    parser.addOption(QCommandLineOption("netsync-roomid", "Target Room-ID for hole punching", "roomid"));
    parser.addOption(QCommandLineOption("netsync-pass", "Lobby password", "pass"));
    parser.addOption(QCommandLineOption("netsync-lobby", "Optional public lobby server URL", "url"));
    parser.addOption(QCommandLineOption("netsync-public", "Announce room to LAN/Lobby browser"));

#ifdef ARCHIVE_SUPPORT_ENABLED
    parser.addOption(QCommandLineOption({"a", "archive-file"}, "Specify file to load inside an archive given (NDS)", "rom"));
    parser.addOption(QCommandLineOption({"A", "archive-file-gba"}, "Specify file to load inside an archive given (GBA)", "rom"));
#endif

    parser.process(melon);

    CommandLineOptions* options = new CommandLineOptions;

    options->fullscreen = parser.isSet("fullscreen");
    options->netsyncPublic = parser.isSet("netsync-public");
    if (parser.isSet("netsync")) options->netsyncMode = parser.value("netsync");
    if (parser.isSet("netsync-addr")) options->netsyncAddr = parser.value("netsync-addr");
    if (parser.isSet("netsync-port")) options->netsyncPort = parser.value("netsync-port").toInt();
    if (parser.isSet("netsync-players")) options->netsyncPlayers = parser.value("netsync-players");
    if (parser.isSet("netsync-delay")) options->netsyncDelay = parser.value("netsync-delay").toInt();
    if (parser.isSet("netsync-name")) options->netsyncName = parser.value("netsync-name");
    if (parser.isSet("netsync-room")) options->netsyncRoom = parser.value("netsync-room");
    if (parser.isSet("netsync-roomid")) options->netsyncRoomId = parser.value("netsync-roomid");
    if (parser.isSet("netsync-pass")) options->netsyncPass = parser.value("netsync-pass");
    if (parser.isSet("netsync-lobby")) options->netsyncLobbyUrl = parser.value("netsync-lobby");

    QStringList posargs = parser.positionalArguments();
    switch (posargs.size())
    {
        default:
            Log(LogLevel::Warn, "Too many positional arguments; ignoring 3 onwards\n");
        case 2:
            options->gbaRomPath = posargs[1];
        case 1:
            options->dsRomPath = posargs[0];
        case 0:
            break;
    }

    QString bootMode = parser.value("boot");
    if (bootMode == "auto")
    {
        options->boot = !posargs.empty();
    }
    else if (bootMode == "always")
    {
        options->boot = true;
    }
    else if (bootMode == "never")
    {
        options->boot = false;
    }
    else
    {
        Log(LogLevel::Error, "ERROR: -b/--boot only accepts auto/always/never as arguments\n");
        exit(1);
    }

#ifdef ARCHIVE_SUPPORT_ENABLED
    if (parser.isSet("archive-file"))
    {
        if (options->dsRomPath.has_value())
        {
            options->dsRomArchivePath = parser.value("archive-file");
        }
        else
        {
            Log(LogLevel::Error, "Option -a/--archive-file given, but no archive specified!");
        }
    }

    if (parser.isSet("archive-file-gba"))
    {
        if (options->gbaRomPath.has_value())
        {
            options->gbaRomArchivePath = parser.value("archive-file-gba");
        }
        else
        {
            Log(LogLevel::Error, "Option -A/--archive-file-gba given, but no archive specified!");
        }
    }
#endif

    return options;
}

}
