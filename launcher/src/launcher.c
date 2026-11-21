/*
 * launcher.c -- "Namco 22 Arcade LAN.exe": the package's front end (Windows).
 *
 * Full screen, like a cabinet's own menu: the games down the left, the chosen one's picture and how to play it on the right.
 * Keyboard (arrows, Enter, Esc), a game controller (D-pad / stick, A, B) or the mouse. The package beside it:
 *
 *   Namco 22 Arcade LAN.exe       this program, and its settings: launcher.cfg
 *   games\<Game>\                  each game's program (<Name>-ArcadeLAN.exe), its own roms\ folder and settings (link.cfg)
 *   advanced\                      one .bat per way to play (written by --write-advanced): the same as this menu's buttons
 *
 * SET UP AND FORGET: Settings > "Start a game when Windows starts" picks a game and a way to play it. Windows then starts this
 * program with --auto at sign-in (HKCU ...\Run), it counts down (any key or button gives the menu instead) and starts the
 * game; if the game ever closes, it is started again after a few seconds (Settings > "If the game closes"). A game that
 * restarts itself (the link's safety net) is noticed by its program path, so it is never started twice.
 *
 *   --auto                 the start-up countdown, as Windows runs it
 *   --window               in a window instead of full screen (also Settings > Launcher screen)
 *   --write-advanced DIR   write the .bat launchers into DIR and exit
 *   --list                 print every game and way to play, and exit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_syswm.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_GL2_IMPLEMENTATION
#include "../../third_party/nuklear.h"
#include "../../third_party/nuklear_sdl_gl2.h"

#define APP_NAME   "Namco 22 Arcade LAN"
#define RUN_VALUE  L"Namco 22 Arcade LAN"         /* the HKCU Run value's name */

/* ---- THE GAMES AND THE WAYS TO PLAY THEM ------------------------------------------------------------------------------
 * A way to play starts one or more copies of the game (copies on one PC: "try" and party modes), in the game's folder.
 * Per copy: the command line after the program, and environment settings ("NAME=value;NAME=value"). */
#define MAXCOPY 8
typedef struct { const char *args, *env; } copy_t;
typedef struct {
    const char *label, *hint;
    bool auto_ok;                       /* offered for the automatic start (a cabinet's normal way to run) */
    int n; copy_t c[MAXCOPY];
} way_t;
typedef struct {
    const char *id, *name, *year, *folder, *exe, *line1, *line2, *cfg;   /* cfg: its link settings file (NULL = none) */
    int nmodes; way_t m[12];
} game_t;

/* THE WAYS TO PLAY. One wording for every game: a linked cabinet (full screen / window), the tests that run linked copies on this PC,
 * and the split-screen Party modes. Index 0 (and Time Crisis II's first three) are the ones offered for the automatic start: the
 * automatic start is saved as game id + way number, so a new way goes after the others or keeps the numbers of those before it. */
#define M_NET_FULL(roms) { "Full screen", "The normal choice for a cabinet. With Link play on (Settings) it joins the other cabinets.", true, 1, \
        { { roms " --fullscreen", NULL } } }
#define M_NET_WIN(roms)  { "Window", "The same in a window, for setting up and testing.", false, 1, \
        { { roms " --window", NULL } } }
#define M_TRY2(roms)     { "Test: 2 cabinets", "Opens 2 linked windows side by side. Click a window to play in it.", false, 2, \
        { { roms " --window 1 --link 1 --link-cabinets 2", NULL }, { roms " --window 1 --link 2 --link-cabinets 2 --mute", NULL } } }
#define M_TRY4(roms)     { "Test: 4 cabinets", "Opens 4 linked windows in a grid. Only cabinet 1 has sound.", false, 4, \
        { { roms " --window 1 --link 1 --link-cabinets 4", NULL }, { roms " --window 1 --link 2 --link-cabinets 4 --mute", NULL }, \
          { roms " --window 1 --link 3 --link-cabinets 4 --mute", NULL }, { roms " --window 1 --link 4 --link-cabinets 4 --mute", NULL } } }
#define M_PARTY2K(roms)  { "Party: 2 players, keyboard and controller", \
        "Split screen on this PC: player 1 on the keyboard, player 2 on a controller. Connect the controller first.", false, 2, \
        { { roms " --window 1 --link 2 --link-cabinets 2 --party --pad 1 --mute", NULL }, { roms " --window 1 --link 1 --link-cabinets 2 --party --pad 0", NULL } } }
#define M_PARTY2P(roms)  { "Party: 2 players, 2 controllers", "Split screen on this PC, one controller each. Connect the controllers first.", false, 2, \
        { { roms " --window 1 --link 2 --link-cabinets 2 --party --pad 2 --mute", NULL }, { roms " --window 1 --link 1 --link-cabinets 2 --party --pad 1", NULL } } }
#define M_PARTY4K(roms)  { "Party: 4 players, keyboard and 3 controllers", \
        "Split screen on this PC: player 1 on the keyboard, players 2 to 4 on controllers. Connect the controllers first.", false, 4, \
        { { roms " --window 1 --link 2 --link-cabinets 4 --party --pad 1 --mute", NULL }, { roms " --window 1 --link 3 --link-cabinets 4 --party --pad 2 --mute", NULL }, \
          { roms " --window 1 --link 4 --link-cabinets 4 --party --pad 3 --mute", NULL }, { roms " --window 1 --link 1 --link-cabinets 4 --party --pad 0", NULL } } }
#define M_PARTY4P(roms)  { "Party: 4 players, 4 controllers", "Split screen on this PC, one controller each. Connect the controllers first.", false, 4, \
        { { roms " --window 1 --link 2 --link-cabinets 4 --party --pad 2 --mute", NULL }, { roms " --window 1 --link 3 --link-cabinets 4 --party --pad 3 --mute", NULL }, \
          { roms " --window 1 --link 4 --link-cabinets 4 --party --pad 4 --mute", NULL }, { roms " --window 1 --link 1 --link-cabinets 4 --party --pad 1", NULL } } }
#define M_SOLO_FULL(roms) { "Full screen", "The normal choice for a cabinet.", true, 1, { { roms " --fullscreen", NULL } } }
#define M_SOLO_WIN(roms, w) { "Window", "The same in a window.", false, 1, { { roms " --window" w, NULL } } }
#define NET_MODES(roms) M_NET_FULL(roms), M_NET_WIN(roms), M_TRY2(roms), M_TRY4(roms), M_PARTY2K(roms), M_PARTY2P(roms), M_PARTY4K(roms), M_PARTY4P(roms)

static const game_t games[] = {
    { "acedrive", "Ace Driver", "1994", "Ace Driver", "AceDriver-ArcadeLAN.exe",
      "Formula racing for up to 8 linked cabinets. Linked play is new in this package.",
      "Keys: 5 coin (2 per credit)  \xC2\xB7  arrows steer  \xC2\xB7  X accelerate  \xC2\xB7  Z brake  \xC2\xB7  A / S shift  \xC2\xB7  V view  \xC2\xB7  Esc menu", "link.cfg",
      9, { M_NET_FULL("roms"), M_NET_WIN("roms"), M_TRY2("roms"), M_TRY4("roms"),
        { "Test: 8 cabinets", "Opens 8 small linked windows. Only cabinet 1 has sound. Needs a powerful PC.", false, 8, {
            { "roms --window 1 --link 1 --link-cabinets 8", NULL }, { "roms --window 1 --link 2 --link-cabinets 8 --mute", NULL },
            { "roms --window 1 --link 3 --link-cabinets 8 --mute", NULL }, { "roms --window 1 --link 4 --link-cabinets 8 --mute", NULL },
            { "roms --window 1 --link 5 --link-cabinets 8 --mute", NULL }, { "roms --window 1 --link 6 --link-cabinets 8 --mute", NULL },
            { "roms --window 1 --link 7 --link-cabinets 8 --mute", NULL }, { "roms --window 1 --link 8 --link-cabinets 8 --mute", NULL } } },
        M_PARTY2K("roms"), M_PARTY2P("roms"), M_PARTY4K("roms"), M_PARTY4P("roms") } },
    { "victlap", "Ace Driver: Victory Lap", "1996", "Ace Driver Victory Lap", "VictoryLap-ArcadeLAN.exe",
      "Ace Driver's 1996 sequel: a three-round championship for up to 4 linked cabinets. New in this package.",
      "Keys: 5 coin (2 per credit)  \xC2\xB7  arrows steer  \xC2\xB7  X accelerate  \xC2\xB7  Z brake  \xC2\xB7  A / S shift  \xC2\xB7  V view  \xC2\xB7  Esc menu", "link.cfg",
      8, { NET_MODES("roms") } },   /* (8 cabinets link, but 8 windows on one PC did not keep up: left out) */
    { "airco22", "Air Combat 22", "1995", "Air Combat 22", "AirCombat22-ArcadeLAN.exe",
      "Jet-fighter missions for a single cabinet. New in this package.",
      "Keys: 5 coin  \xC2\xB7  Enter start / view  \xC2\xB7  arrows fly  \xC2\xB7  W boost  \xC2\xB7  S brake  \xC2\xB7  Space gun  \xC2\xB7  Alt missile  \xC2\xB7  Esc menu", NULL,
      2, { M_SOLO_FULL("roms"), M_SOLO_WIN("roms", " 2") } },
    { "alpiner", "Alpine Racer", "1995", "Alpine Racer", "AlpineRacer-ArcadeLAN.exe",
      "Downhill skiing for a single cabinet. New in this package.",
      "Keys: 5 coin  \xC2\xB7  Enter decide / change view  \xC2\xB7  Z, X select  \xC2\xB7  arrows ski  \xC2\xB7  Esc menu", NULL,
      2, { M_SOLO_FULL("roms"), M_SOLO_WIN("roms", " 2") } },
    { "alpinr2", "Alpine Racer 2", "1996", "Alpine Racer 2", "AlpineRacer2-ArcadeLAN.exe",
      "Downhill skiing: racing, gate racing, free skiing and moguls; 2 linked cabinets race head to head. New in this package.",
      "Keys: 5 coin  \xC2\xB7  Enter decide / change view  \xC2\xB7  Z, X select  \xC2\xB7  arrows ski  \xC2\xB7  Esc menu", "link.cfg",
      5, { M_NET_FULL("roms"), M_NET_WIN("roms"), M_TRY2("roms"), M_PARTY2K("roms"), M_PARTY2P("roms") } },
    { "alpines", "Alpine Surfer", "1996", "Alpine Surfer", "AlpineSurfer-ArcadeLAN.exe",
      "Snowboarding for a single cabinet: freeriding and gate racing. New in this package.",
      "Keys: 5 coin  \xC2\xB7  Enter decide / change view  \xC2\xB7  Z, X select  \xC2\xB7  arrows ride  \xC2\xB7  Esc menu", NULL,
      2, { M_SOLO_FULL("roms"), M_SOLO_WIN("roms", " 2") } },
    { "aquajet", "Aqua Jet", "1996", "Aqua Jet", "AquaJet-ArcadeLAN.exe",
      "Jet-ski racing for a single cabinet: the novice and expert courses. New in this package.",
      "Keys: 5 coin  \xC2\xB7  Enter start / view  \xC2\xB7  arrows lean  \xC2\xB7  Up throttle  \xC2\xB7  Z / X lean forward / back  \xC2\xB7  Esc menu", NULL,
      2, { M_SOLO_FULL("roms"), M_SOLO_WIN("roms", " 2") } },
    { "adillor", "Armadillo Racing", "1997", "Armadillo Racing", "ArmadilloRacing-ArcadeLAN.exe",
      "Trackball racing for up to 4 linked cabinets: red, blue, yellow and green. New in this package.",
      "Controls: mouse, trackball or arrows roll  \xC2\xB7  touchpad: two-finger scroll  \xC2\xB7  5 coin  \xC2\xB7  1 start  \xC2\xB7  Esc menu", "link.cfg",
      8, { NET_MODES("roms") } },
    { "crszone", "Crisis Zone", "1999", "Crisis Zone", "CrisisZone.exe",
      "Light-gun shooting with a riot shield, for a single cabinet. The original project's build.",
      "Controls: mouse or light gun aims and shoots  \xC2\xB7  the pedal takes cover  \xC2\xB7  Esc menu", NULL,
      2, { { "Full screen", "The normal choice for a cabinet.", true, 1, { { "--window --fullscreen", NULL } } },
           { "Window", "The same in a window.", false, 1, { { "--window", NULL } } } } },
    { "cybrcomm", "Cyber Commando", "1994", "Cyber Commando", "CyberCommando-ArcadeLAN.exe",
      "Robot battles: 2 linked cabinets fight each other. Linked play is new in this package.",
      "Keys: 5 coin (2 per credit)  \xC2\xB7  arrows move  \xC2\xB7  Z fire  \xC2\xB7  X missile  \xC2\xB7  C view  \xC2\xB7  Esc menu", "link.cfg",
      5, { M_NET_FULL("roms"), M_NET_WIN("roms"), M_TRY2("roms"), M_PARTY2K("roms"), M_PARTY2P("roms") } },
    { "cybrcycc", "Cyber Cycles", "1995", "Cyber Cycles", "CyberCycles-ArcadeLAN.exe",
      "Motorbike racing for up to 4 linked cabinets. New in this package.",
      "Keys: 5 coin  \xC2\xB7  Enter start / view  \xC2\xB7  arrows lean  \xC2\xB7  Up throttle  \xC2\xB7  Down brake  \xC2\xB7  Esc menu", "link.cfg",
      8, { NET_MODES("roms") } },
    { "cybsled", "Cyber Sled", "1993", "Cyber Sled", "CyberSled-ArcadeLAN.exe",
      "Twin-stick hover-tank battles: 2 linked cabinets, left and right, fight each other. Linked play is new in this package.",
      "Keys: 5 coin  \xC2\xB7  1 start  \xC2\xB7  arrows drive (Shift strafes)  \xC2\xB7  Ctrl fire  \xC2\xB7  Esc menu", "link.cfg",
      5, { M_NET_FULL("extracted"), M_NET_WIN("extracted"), M_TRY2("extracted"), M_PARTY2K("extracted"), M_PARTY2P("extracted") } },
    { "dirtdash", "Dirt Dash", "1995", "Dirt Dash", "DirtDash.exe",
      "Off-road rally racing for a single cabinet. The original project's build.",
      "Keys: 5 coin (2 per credit)  \xC2\xB7  arrows steer  \xC2\xB7  X gas  \xC2\xB7  Z brake  \xC2\xB7  Q / E shift  \xC2\xB7  C view  \xC2\xB7  Esc menu", NULL,
      2, { M_SOLO_FULL("roms"), M_SOLO_WIN("roms", "") } },
    { "gunwars", "Gunmen Wars", "1998", "Gunmen Wars", "GunmenWars-ArcadeLAN.exe",
      "Team shoot-outs for up to 3 linked cabinets (set up in the game's Esc menu, Network page). New in this package.",
      "Controls: mouse turns  \xC2\xB7  left button fires  \xC2\xB7  right button decide  \xC2\xB7  W A S D move  \xC2\xB7  5 coin  \xC2\xB7  Esc menu (Options: NamCam webcam / photo)", NULL,
      5, { { "Full screen", "The normal choice for a cabinet. With Link play on (Settings) it joins the other cabinets.", true, 1,
               { { "--window --fullscreen", NULL } } },
           { "Window", "The same, in a window. Useful while setting up or testing.", false, 1, { { "--window", NULL } } },
           { "Test: 2 cabinets", "Opens 2 linked windows side by side. Controller 1 plays the left, controller 2 the right; the keyboard and mouse play the window you click.", false, 2, {
               { "--window", "GW_LINK=1:2:23180;S23_CABINET_DIR=try-1;S23_WINPOS=1/2;S23_PAD=0" },
               { "--window", "GW_LINK=2:2:23180;S23_CABINET_DIR=try-2;S23_WINPOS=2/2;S23_PAD=1;S23_MUTE=1" } } },
           { "Party: 2 players, keyboard and controller", "Split screen on this PC: player 1 on the keyboard and mouse (left), player 2 on a controller (right). Connect the controller first.", false, 2, {
               { "--window", "GW_LINK=1:2:23180;S23_CABINET_DIR=try-1;S23_WINPOS=1/2;S23_PARTY=1;S23_PAD=none" },
               { "--window", "GW_LINK=2:2:23180;S23_CABINET_DIR=try-2;S23_WINPOS=2/2;S23_PARTY=1;S23_PAD=0;S23_MUTE=1" } } },
           { "Party: 2 players, 2 controllers", "Split screen on this PC, one controller each (controller 1 left, controller 2 right). Connect the controllers first.", false, 2, {
               { "--window", "GW_LINK=1:2:23180;S23_CABINET_DIR=try-1;S23_WINPOS=1/2;S23_PARTY=1;S23_PAD=0" },
               { "--window", "GW_LINK=2:2:23180;S23_CABINET_DIR=try-2;S23_WINPOS=2/2;S23_PARTY=1;S23_PAD=1;S23_MUTE=1" } } } } },
    { "propcycl", "Prop Cycle", "1996", "Prop Cycle", "PropCycle.exe",
      "Pedal-powered flying for a single cabinet. The original project's build.",
      "Keys: 5 coin  \xC2\xB7  Enter start  \xC2\xB7  arrows steer and lean  \xC2\xB7  Esc menu", NULL,
      1, { { "Play", "Opens the game. Choose full screen or a window in its Esc menu or with F11: it remembers.", true, 1, { { "", NULL } } } } },
    { "rapidrvr", "Rapid River", "1997", "Rapid River", "RapidRiver-ArcadeLAN.exe",
      "White-water rafting for a single cabinet. New in this package.",
      "Keys: 5 coin (2 per credit)  \xC2\xB7  1 start  \xC2\xB7  Up paddle  \xC2\xB7  Left / Right steer  \xC2\xB7  Esc menu", NULL,
      2, { { "Full screen", "The normal choice for a cabinet.", true, 1, { { "--window --fullscreen", NULL } } },
           { "Window", "The same in a window.", false, 1, { { "--window", NULL } } } } },
    { "raverace", "Rave Racer", "1995", "Rave Racer", "RaveRacer-ArcadeLAN.exe",
      "Racing for up to 4 linked cabinets.",
      "Keys: 5 coin  \xC2\xB7  1 start  \xC2\xB7  arrows steer  \xC2\xB7  X accelerate  \xC2\xB7  Z brake  \xC2\xB7  Esc menu", "link.cfg",
      8, { NET_MODES("roms") } },
    { "timecris", "Time Crisis", "1995", "Time Crisis", "TimeCrisis.exe",
      "Light-gun shooting for a single cabinet. The original project's build.",
      "Controls: mouse or light gun aims and shoots  \xC2\xB7  right button or Z is the pedal  \xC2\xB7  5 coin (3 per credit)  \xC2\xB7  Esc menu", NULL,
      2, { M_SOLO_FULL("roms"), M_SOLO_WIN("roms", "") } },
    { "timecrs2", "Time Crisis II", "1997", "Time Crisis II", "TimeCrisis2-ArcadeLAN.exe",
      "The twin light-gun cabinet: the left and right sides link for 2 players.",
      "Controls: mouse or light gun aims and shoots  \xC2\xB7  the pedal takes cover  \xC2\xB7  Esc menu", "tc2_link.cfg",
      8, {
        { "Left side (red player), full screen", "This PC is the left side of the twin cabinet. The normal choice for a cabinet.", true, 1,
            { { "--window --fullscreen", "TC2_SIDE=left" } } },
        { "Right side (blue player), full screen", "This PC is the right side of the twin cabinet. The normal choice for a cabinet.", true, 1,
            { { "--window --fullscreen", "TC2_SIDE=right" } } },
        { "Both sides on one PC, 2 monitors and 2 guns", "One PC runs the whole twin cabinet. The first gun to fire becomes the left player.", true, 2, {
            { "--window", "TC2_OWN_GUN=1;TC2_LINK=L:23021:127.0.0.1:23022;TC2_CABINET_DIR=screen-left;TC2_MONITOR=1" },
            { "--window", "TC2_OWN_GUN=1;TC2_LINK=R:23022:127.0.0.1:23021;TC2_CABINET_DIR=screen-right;TC2_MONITOR=2" } } },
        { "Left side (red player), window", "The left side, in a window.", false, 1, { { "--window", "TC2_SIDE=left" } } },
        { "Right side (blue player), window", "The right side, in a window.", false, 1, { { "--window", "TC2_SIDE=right" } } },
        { "Test: both sides", "Opens the left and right sides in 2 linked windows.", false, 2, {
            { "--window", "TC2_LINK=L:23011:127.0.0.1:23012;TC2_CABINET_DIR=try-left;TC2_WINPOS=left" },
            { "--window", "TC2_LINK=R:23012:127.0.0.1:23011;TC2_CABINET_DIR=try-right;TC2_WINPOS=right" } } },
        { "Party: 2 players, keyboard and controller", "Split screen on this PC: the red player on the mouse and keyboard (left), the blue player on a controller (right). Connect the controller first.", false, 2, {
            { "--window", "TC2_LINK=L:23031:127.0.0.1:23032;TC2_CABINET_DIR=party-left;S23_WINPOS=1/2;S23_PARTY=1;S23_PAD=none" },
            { "--window", "TC2_LINK=R:23032:127.0.0.1:23031;TC2_CABINET_DIR=party-right;S23_WINPOS=2/2;S23_PARTY=1;S23_PAD=0;S23_MUTE=1" } } },
        { "Party: 2 players, 2 controllers", "Split screen on this PC, one controller each: controller 1 the red player (left), controller 2 the blue (right). Connect the controllers first.", false, 2, {
            { "--window", "TC2_LINK=L:23031:127.0.0.1:23032;TC2_CABINET_DIR=party-left;S23_WINPOS=1/2;S23_PARTY=1;S23_PAD=0" },
            { "--window", "TC2_LINK=R:23032:127.0.0.1:23031;TC2_CABINET_DIR=party-right;S23_WINPOS=2/2;S23_PARTY=1;S23_PAD=1;S23_MUTE=1" } } } } },
    { "tokyowar", "Tokyo Wars", "1996", "Tokyo Wars", "TokyoWars-ArcadeLAN.exe",
      "Team tank battles for up to 4 linked cabinets.",
      "Keys: 5 coin  \xC2\xB7  1 start  \xC2\xB7  arrows drive  \xC2\xB7  Ctrl fire  \xC2\xB7  Esc menu", "link.cfg",
      8, { NET_MODES("roms") } },
};
#define NGAMES ((int)(sizeof games / sizeof *games))

/* ---- PATHS AND THE SETTINGS FILE ------------------------------------------------------------------------------------ */
static char base[MAX_PATH];                     /* the package folder (this program's), with a trailing backslash */
static FILE *log_file;
static void logm(const char *fmt, ...)
{
    if (!log_file) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(log_file, "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(log_file, fmt, a); va_end(a);
    fputc('\n', log_file); fflush(log_file);
}
/* the game list's second line: how many cabinets play together */
static const char *cabinets_text(const game_t *g)
{
    static const struct { const char *id, *text; } t[] = {
        { "acedrive", "Up to 8 linked cabinets" }, { "victlap", "Up to 4 linked cabinets" }, { "adillor", "Up to 4 linked cabinets" }, { "cybrcomm", "2 linked cabinets" },
        { "cybrcycc", "Up to 4 linked cabinets" }, { "cybsled", "2 linked cabinets" }, { "gunwars", "Up to 3 linked cabinets" }, { "alpinr2", "2 linked cabinets" },
        { "raverace", "Up to 4 linked cabinets" }, { "timecrs2", "Twin cabinet, 2 players" }, { "tokyowar", "Up to 4 linked cabinets" } };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(g->id, t[i].id)) return t[i].text;
    return "Single cabinet";
}
static void game_dir(const game_t *g, char *out, size_t n) { snprintf(out, n, "%sgames\\%s", base, g->folder); }
static void game_exe(const game_t *g, char *out, size_t n) { snprintf(out, n, "%sgames\\%s\\%s", base, g->folder, g->exe); }

static struct { int auto_game, auto_mode, delay, restart, windowed, tests, link; } S = { -1, 0, 10, 1, 0, 0, 0 };
/* LINK PLAY (Settings, off by default): one switch for this PC. A game started on its own (Full screen / Window / a TC2 side) gets
 * link=on|off written into its link settings just before it starts, so off = it plays at once, on = it looks for the other cabinets.
 * The Test / Party ways link by their own command line and are not touched. */
static const char *link_file(const game_t *g) { return g->cfg ? g->cfg : !strcmp(g->id, "gunwars") ? "gunwars.cfg" : NULL; }
/* a way to play the menu shows: the "Test: ..." ways (several linked windows on this PC) only with Settings > Test modes on */
static bool way_shown(const way_t *m) { return S.tests || strncmp(m->label, "Test: ", 6) != 0; }
static const int delays[] = { 5, 10, 20, 30, 60 };

static void cfg_load(void)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%slauncher.cfg", base);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char k[64], v[128];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %127[^\r\n]", k, v) != 2) continue;
        if (!strcmp(k, "auto_start")) {
            S.auto_game = -1;
            char gid[64]; int m = 0;
            if (sscanf(v, "%63[^:]:%d", gid, &m) == 2)
                for (int i = 0; i < NGAMES; i++) if (!strcmp(games[i].id, gid) && m >= 0 && m < games[i].nmodes) { S.auto_game = i; S.auto_mode = m; }
        }
        else if (!strcmp(k, "auto_delay")) S.delay = atoi(v);
        else if (!strcmp(k, "if_closed")) S.restart = strcmp(v, "menu") != 0;
        else if (!strcmp(k, "screen")) S.windowed = !strcmp(v, "window");
        else if (!strcmp(k, "test_modes")) S.tests = !strcmp(v, "on");
        else if (!strcmp(k, "link_play")) S.link = !strcmp(v, "on");
    }
    fclose(f);
    if (S.delay < 1) S.delay = 10;
}
static void cfg_save(void)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%slauncher.cfg", base);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "# " APP_NAME " launcher settings (its Settings page edits this file)\n");
    fprintf(f, "# auto_start: the game and way to play started when Windows starts (<game>:<way>, see --list), or off\n");
    if (S.auto_game >= 0) fprintf(f, "auto_start=%s:%d\n", games[S.auto_game].id, S.auto_mode); else fprintf(f, "auto_start=off\n");
    fprintf(f, "# auto_delay: seconds of countdown first (a key or button gives the menu instead)\nauto_delay=%d\n", S.delay);
    fprintf(f, "# if_closed: restart = start the game again when it closes (automatic start), menu = come back to this menu\nif_closed=%s\n",
            S.restart ? "restart" : "menu");
    fprintf(f, "screen=%s\n", S.windowed ? "window" : "fullscreen");
    fprintf(f, "# test_modes: show the Test ways (several linked windows on this PC) in the menu\ntest_modes=%s\n", S.tests ? "on" : "off");
    fprintf(f, "# link_play: on = the games link with the other cabinets, off = each game plays on its own\nlink_play=%s\n", S.link ? "on" : "off");
    fclose(f);
}

/* Windows starts the launcher at sign-in when auto start is on: HKCU\Software\Microsoft\Windows\CurrentVersion\Run */
static bool run_key_set(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    LONG r;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --auto", exe);
        r = RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(k, RUN_VALUE);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    logm("Windows start-up entry %s: %s", on ? "set" : "removed", r == ERROR_SUCCESS ? "ok" : "FAILED");
    return r == ERROR_SUCCESS;
}

/* ---- THE GAMES' OWN SETTINGS FILES: one key across every game that has it (link lights, session recordings) ---------- */
static bool cfg_get(const char *path, const char *key, char *val, size_t n)
{
    FILE *f = fopen(path, "r"); if (!f) return false;
    char line[256]; bool found = false;
    while (fgets(line, sizeof line, f)) {
        char k[64], v[128];
        if (line[0] != '#' && sscanf(line, " %63[^= ] = %127s", k, v) == 2 && !strcmp(k, key)) { snprintf(val, n, "%s", v); found = true; }
    }
    fclose(f);
    return found;
}
static void cfg_set(const char *path, const char *key, const char *val)
{
    char buf[16384]; size_t len = 0; buf[0] = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char k[64];
            if (line[0] != '#' && sscanf(line, " %63[^= ]", k) == 1 && !strcmp(k, key)) continue;   /* the old line goes */
            size_t l = strlen(line);
            if (len + l + 2 < sizeof buf) { memcpy(buf + len, line, l); len += l; if (line[l - 1] != '\n') buf[len++] = '\n'; buf[len] = 0; }
        }
        fclose(f);
    }
    f = fopen(path, "w"); if (!f) return;
    fputs(buf, f); fprintf(f, "%s=%s\n", key, val); fclose(f);
}
static void cfg_path(const game_t *g, char *out, size_t n) { snprintf(out, n, "%sgames\\%s\\%s", base, g->folder, g->cfg); }
/* lights: "off" or a corner (default bottom-left); lights_was remembers the corner while off. record: on (default) / off */
static bool all_on(const char *key, const char *dflt)
{
    for (int i = 0; i < NGAMES; i++) {
        if (!games[i].cfg) continue;
        char p[MAX_PATH], v[64]; cfg_path(&games[i], p, sizeof p);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
        if (!cfg_get(p, key, v, sizeof v)) snprintf(v, sizeof v, "%s", dflt);
        if (strcmp(v, "off")) return true;
    }
    return false;
}
static void all_set(const char *key, bool on)
{
    for (int i = 0; i < NGAMES; i++) {
        if (!games[i].cfg) continue;
        char p[MAX_PATH]; cfg_path(&games[i], p, sizeof p);
        if (!strcmp(key, "lights")) {
            char was[64] = "bottom-left", now[64] = "bottom-left";
            cfg_get(p, "lights", now, sizeof now); cfg_get(p, "lights_was", was, sizeof was);
            if (on) { cfg_set(p, "lights", strcmp(was, "off") ? was : "bottom-left"); }
            else { if (strcmp(now, "off")) cfg_set(p, "lights_was", now); cfg_set(p, "lights", "off"); }
        } else cfg_set(p, key, on ? "on" : "off");
    }
    logm("%s %s for every game", key, on ? "on" : "off");
}

/* ---- STARTING A GAME, AND WATCHING IT -------------------------------------------------------------------------------- */
static HANDLE procs[MAXCOPY]; static int nprocs;
static int run_game = -1, run_mode;
/* a way's arguments as run: a window way also says "not full screen" for this run (--set: engine/eng_args.h) -- a game whose own
 * settings remember full screen opened full screen from the Window button; its settings file is not changed */
static const char *way_args(const char *a)
{
    static char b[512];
    snprintf(b, sizeof b, "%s%s", a, strstr(a, "--window") && !strstr(a, "--fullscreen") ? " --set window_mode=0" : "");
    return b;
}
static bool launch(int gi, int mi)
{
    const game_t *g = &games[gi]; const way_t *m = &g->m[mi];
    char dir[MAX_PATH], exe[MAX_PATH];
    game_dir(g, dir, sizeof dir); game_exe(g, exe, sizeof exe);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) { logm("cannot start %s: %s is missing", g->name, exe); return false; }
    nprocs = 0;
    if (m->n == 1 && link_file(g)) {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%sgames\\%s\\%s", base, g->folder, link_file(g));
        cfg_set(p, "link", S.link ? "on" : "off");
    }
    if (m->n > 1) SetEnvironmentVariableA("N22_ONE_PC", "1");   /* copies on one PC: the games pace by timer, not vsync (engine/eng_vsync.h) */
    for (int c = 0; c < m->n; c++) {
        char cmd[1024]; snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, way_args(m->c[c].args));
        char envs[512] = "";                       /* the copy's own settings: set for the child, cleared again after */
        if (m->c[c].env) snprintf(envs, sizeof envs, "%s", m->c[c].env);
        for (char *t = strtok(envs, ";"); t; t = strtok(NULL, ";")) { char *eq = strchr(t, '='); if (eq) { *eq = 0; SetEnvironmentVariableA(t, eq + 1); *eq = '='; } }
        STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
        BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi);
        if (m->c[c].env) { snprintf(envs, sizeof envs, "%s", m->c[c].env);
            for (char *t = strtok(envs, ";"); t; t = strtok(NULL, ";")) { char *eq = strchr(t, '='); if (eq) { *eq = 0; SetEnvironmentVariableA(t, NULL); } } }
        if (!ok) { logm("cannot start %s (error %lu)", cmd, GetLastError()); continue; }
        CloseHandle(pi.hThread); procs[nprocs++] = pi.hProcess;
        logm("started %s - %s: %s", g->name, m->label, cmd);
        if (c + 1 < m->n) Sleep(300);
    }
    SetEnvironmentVariableA("N22_ONE_PC", NULL);
    if (!nprocs) return false;
    run_game = gi; run_mode = mi;
    return true;
}
/* is any copy of this game's program running (ours, or one the game started itself: the link's safety net restarts it)? */
static bool game_running(int gi)
{
    char want[MAX_PATH]; game_exe(&games[gi], want, sizeof want);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = { sizeof pe }; bool found = false;
    for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, games[gi].exe)) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        char path[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameA(h, 0, path, &n) && !_stricmp(path, want)) found = true;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}
static bool ours_running(void)
{
    for (int i = 0; i < nprocs; i++) if (WaitForSingleObject(procs[i], 0) == WAIT_TIMEOUT) return true;
    return false;
}
/* the first visible window of a game we started: the launcher (still in front) hands it the focus before hiding -- hidden at once,
 * Windows gave the focus to whatever was behind it and the game's window opened behind */
static BOOL CALLBACK find_game_win(HWND h, LPARAM lp)
{
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (!IsWindowVisible(h)) return TRUE;
    for (int i = 0; i < nprocs; i++) if (GetProcessId(procs[i]) == pid) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}
/* back in front when the game closes: the game had the focus and is gone, so Windows refuses SetForegroundWindow (the launcher
 * stayed behind) -- unless this process sent the last input: a tap of Alt, the usual way round the lock */
static void take_focus(SDL_Window *w)
{
    SDL_ShowWindow(w); SDL_RaiseWindow(w);
    SDL_SysWMinfo i; SDL_VERSION(&i.version);
    if (!SDL_GetWindowWMInfo(w, &i)) return;
    HWND h = i.info.win.window;
    if (GetForegroundWindow() == h) return;
    keybd_event(VK_MENU, 0, 0, 0); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(h); BringWindowToTop(h);
}
static HWND game_window(void) { HWND h = NULL; EnumWindows(find_game_win, (LPARAM)&h); return h; }
static void forget_procs(void) { for (int i = 0; i < nprocs; i++) CloseHandle(procs[i]); nprocs = 0; }

/* ---- WINDOWS: write the .bat launchers, this PC's addresses ---------------------------------------------------------- */
static void write_advanced(const char *dir)
{
    CreateDirectoryA(dir, NULL);
    for (int gi = 0; gi < NGAMES; gi++) {
        const game_t *g = &games[gi];
        for (int mi = 0; mi < g->nmodes; mi++) {
            const way_t *m = &g->m[mi];
            char lab[128]; int n = 0;                     /* a file name: "PARTY: 2 players" -> "PARTY - 2 players" */
            for (const char *c = m->label; *c && n < (int)sizeof lab - 3; c++) {
                if (*c == ':') { lab[n++] = ' '; lab[n++] = '-'; }
                else if (!strchr("\\/*?\"<>|", *c)) lab[n++] = *c;
            }
            lab[n] = 0;
            char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s - %s.bat", dir, g->folder, lab);   /* (the folder: a name's ':' would make an NTFS stream) */
            FILE *f = fopen(p, "wb"); if (!f) continue;
            static const char rule[] = "rem ==========================================================================\r\n";
            fprintf(f, "@echo off\r\n%srem  %s - %s\r\nrem  %s\r\nrem\r\nrem  This does the same as the button in " APP_NAME ".exe.\r\n"
                       "rem  (Written by \"" APP_NAME ".exe --write-advanced\".)\r\n%s", rule, g->name, m->label, m->hint, rule);
            fprintf(f, "setlocal\r\ncd /d \"%%~dp0..\\games\\%s\"\r\n", g->folder);
            if (m->n > 1) fprintf(f, "rem Several copies on one PC: they pace by timer, not vsync (vsync favours the window in front)\r\nset \"N22_ONE_PC=1\"\r\n");
            for (int c = 0; c < m->n; c++) {
                if (m->n > 1) fprintf(f, "\r\nrem Window %d of %d\r\n", c + 1, m->n); else fprintf(f, "\r\n");
                if (m->c[c].env) { char e[512]; snprintf(e, sizeof e, "%s", m->c[c].env);
                    for (char *t = strtok(e, ";"); t; t = strtok(NULL, ";")) fprintf(f, "set \"%s\"\r\n", t); }
                fprintf(f, "start \"\" \"%s\" %s\r\n", g->exe, way_args(m->c[c].args));
                if (c + 1 < m->n) fprintf(f, "rem A short pause, so the windows open in order\r\nping -n 1 -w 300 127.0.0.1 >nul\r\n");
            }
            fclose(f);
        }
    }
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\About these files.txt", dir);
    FILE *f = fopen(p, "wb");
    if (f) {
        fprintf(f, "ABOUT THESE FILES\r\n=================\r\n\r\n"
                   "Each .bat file here starts one game in one way, exactly like the matching button in\r\n"
                   APP_NAME ".exe. They are for desktop shortcuts, other front ends and scripts.\r\n\r\n"
                   "They are named  <game> - <way to play>.bat  and only work from this folder inside the\r\n"
                   "package (they find the games in ..\\games).\r\n\r\n"
                   "The launcher writes them again with:  \"" APP_NAME ".exe\" --write-advanced <folder>\r\n");
        fclose(f);
    }
}
static int my_addresses(char out[][48], int max)
{
    ULONG sz = 16384; IP_ADAPTER_ADDRESSES *aa = malloc(sz); int n = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz) == ERROR_BUFFER_OVERFLOW) {
        free(aa); aa = malloc(sz);
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz) != NO_ERROR) { free(aa); return 0; }
    }
    for (IP_ADAPTER_ADDRESSES *a = aa; a && n < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
            const unsigned char *b = (const unsigned char *)&((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr;
            if (b[0] == 169 && b[1] == 254) continue;                    /* Windows' own address for an adapter with no network: not a LAN */
            char name[32]; WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name, NULL, NULL);
            snprintf(out[n++], 48, "%u.%u.%u.%u (%.24s)", b[0], b[1], b[2], b[3], name);
        }
    }
    free(aa);
    return n;
}

/* ---- DRAWING ----------------------------------------------------------------------------------------------------------- */
static struct nk_context *ctx;
static struct nk_font *f_title, *f_big, *f_mid, *f_small;
static GLuint pics[NGAMES]; static int pic_w[NGAMES], pic_h[NGAMES];
static GLuint logos[NGAMES]; static int logo_w[NGAMES], logo_h[NGAMES];   /* games\<Game>\logo.bmp: the title logo, 32-bit with alpha */
/* THE LOOK (2026-11-16, an "unslop" polish pass, after unslop by Damir Sokolovsky, @heydamir, CC BY 4.0): a cabinet's service
 * menu -- one graphite ramp for every surface and three text roles, and the coin-door amber ONLY for what is chosen (the bar beside
 * the chosen game or setting, the chosen way to play) and the countdown's game. Hierarchy from size and colour, not weight; key
 * hints are keycaps. */
static const struct nk_color C_DARK = { 15, 15, 16, 255 },   /* the deepest surface: the title and bottom bars */
                             C_BG = { 20, 20, 21, 255 },     /* the page */
                             C_PANEL = { 25, 25, 27, 255 },  /* a row, a button */
                             C_SEL = { 35, 35, 37, 255 },    /* the chosen / hovered row: one surface step up */
                             C_LINE = { 237, 237, 237, 20 }, /* a hairline: primary text at 8% */
                             C_TEXT = { 237, 237, 237, 255 }, C_SEC = { 161, 161, 166, 255 }, C_DIM = { 133, 133, 139, 255 },
                             C_AMBER = { 255, 176, 0, 255 };

/* a BMP into a texture (its alpha kept: SDL reads a 32-bit BMP's alpha mask); 0 = none */
static GLuint load_bmp(const char *p, int *w, int *h)
{
    SDL_Surface *s = SDL_LoadBMP(p); if (!s) return 0;
    SDL_Surface *r = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0); SDL_FreeSurface(s); if (!r) return 0;
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    /* mipmaps: a logo / picture is drawn at a fraction of its size; plain GL_LINEAR skipped pixels (jagged, speckled logos) */
    glTexParameteri(GL_TEXTURE_2D, 0x8191 /* GL_GENERATE_MIPMAP (GL 1.4) */, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, r->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, r->w, r->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, r->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    *w = r->w; *h = r->h; SDL_FreeSurface(r);
    return t;
}
static void load_pictures(void)
{
    for (int i = 0; i < NGAMES; i++) {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%sgames\\%s\\picture.bmp", base, games[i].folder);
        pics[i] = load_bmp(p, &pic_w[i], &pic_h[i]);
        snprintf(p, sizeof p, "%sgames\\%s\\logo.bmp", base, games[i].folder);
        logos[i] = load_bmp(p, &logo_w[i], &logo_h[i]);
    }
}
static void fonts(float h)
{
    struct nk_font_atlas *atlas;
    nk_sdl_font_stash_begin(&atlas);
    const char *reg = "C:\\Windows\\Fonts\\segoeui.ttf", *semi = "C:\\Windows\\Fonts\\seguisb.ttf";   /* Segoe UI 400 and 600 (no bold) */
    if (GetFileAttributesA(semi) == INVALID_FILE_ATTRIBUTES) semi = reg;
    bool have = GetFileAttributesA(reg) != INVALID_FILE_ATTRIBUTES;
    const float k = h / 1080.0f;
    if (have) {
        f_title = nk_font_atlas_add_from_file(atlas, semi, 44 * k, NULL);   /* the product name, the countdown */
        f_big = nk_font_atlas_add_from_file(atlas, reg, 32 * k, NULL);      /* game names, the Settings title */
        f_mid = nk_font_atlas_add_from_file(atlas, reg, 26 * k, NULL);
        f_small = nk_font_atlas_add_from_file(atlas, reg, 21 * k, NULL);
    } else {
        f_title = nk_font_atlas_add_default(atlas, 44 * k, NULL); f_big = nk_font_atlas_add_default(atlas, 32 * k, NULL);
        f_mid = nk_font_atlas_add_default(atlas, 26 * k, NULL); f_small = nk_font_atlas_add_default(atlas, 21 * k, NULL);
    }
    nk_sdl_font_stash_end();
    nk_style_set_font(ctx, &f_mid->handle);
}
static float tw(struct nk_font *f, const char *s) { return f->handle.width(f->handle.userdata, f->handle.height, s, (int)strlen(s)); }
static void text(struct nk_command_buffer *cb, struct nk_font *f, float x, float y, const char *s, struct nk_color c)
{
    nk_draw_text(cb, nk_rect(x, y, tw(f, s) + 4, f->handle.height + 4), s, (int)strlen(s), &f->handle, nk_rgba(0, 0, 0, 0), c);
}
static void text_c(struct nk_command_buffer *cb, struct nk_font *f, float cx, float y, const char *s, struct nk_color c) { text(cb, f, cx - tw(f, s) / 2, y, s, c); }
/* a one-sentence hint without its full stop (longer hints keep theirs) */
static const char *no_stop(const char *s)
{
    static char b[256]; snprintf(b, sizeof b, "%s", s);
    const size_t n = strlen(b);
    if (n && b[n - 1] == '.' && !strstr(b, ". ")) b[n - 1] = 0;
    return b;
}
/* KEYCAPS (the launcher's one signature detail): each key in a small raised cap with a hairline, then what it does. keys is
 * "Enter|A" (caps side by side); returns the x after the label. */
static float keyhint(struct nk_command_buffer *cb, float x, float y, float k, const char *keys, const char *label)
{
    char b[64]; snprintf(b, sizeof b, "%s", keys);
    const float h = 32 * k;
    for (char *t = strtok(b, "|"); t; t = strtok(NULL, "|")) {
        const float w = tw(f_small, t) + 16 * k;
        nk_fill_rect(cb, nk_rect(x, y, w, h), 6 * k, C_LINE);
        nk_fill_rect(cb, nk_rect(x + 1, y + 1, w - 2, h - 2), 5 * k, C_PANEL);
        text(cb, f_small, x + 8 * k, y + 3 * k, t, C_SEC);
        x += w + 4 * k;
    }
    text(cb, f_small, x + 4 * k, y + 3 * k, label, C_DIM);
    return x + 4 * k + tw(f_small, label) + 28 * k;
}

/* ---- THE SCREENS ------------------------------------------------------------------------------------------------------ */
enum { SC_MENU, SC_SETTINGS, SC_COUNTDOWN, SC_RUNNING, SC_RESTART, SC_QUIT };
static int screen = SC_MENU, sel_game, sel_mode, focus;     /* focus 0: the games, 1: the ways to play */
static int sel_set; static Uint64 count_until; static char message[160]; static Uint64 message_until;
static bool auto_session;                                   /* started by Windows (--auto): the watchdog's restart applies */
static bool want_quit;
enum { SET_LINK, SET_AUTO, SET_DELAY, SET_CLOSED, SET_SCREEN, SET_TESTS, SET_LIGHTS, SET_RECORD, SET_ADDR, SET_N };

static void say(const char *m) { snprintf(message, sizeof message, "%s", m); message_until = SDL_GetTicks64() + 4000; }

/* the auto start choices: every way to play marked auto_ok, in order */
static int auto_next(int dir)
{
    int list[64][2], n = 0, cur = -1;
    list[n][0] = -1; list[n][1] = 0; n++;
    for (int g = 0; g < NGAMES; g++) for (int m = 0; m < games[g].nmodes; m++) if (games[g].m[m].auto_ok && n < 64) { list[n][0] = g; list[n][1] = m; n++; }
    for (int i = 0; i < n; i++) if (list[i][0] == S.auto_game && (S.auto_game < 0 || list[i][1] == S.auto_mode)) cur = i;
    if (cur < 0) cur = 0;
    cur = (cur + dir + n) % n;
    S.auto_game = list[cur][0]; S.auto_mode = list[cur][1];
    return cur;
}

static Uint64 run_since;                          /* when the game was started (the launcher waits for its window) */
static void start_mode(int g, int m, bool from_auto)
{
    if (!launch(g, m)) { char b[160]; snprintf(b, sizeof b, "%s could not start. Check that games\\%s\\%s is there.", games[g].name, games[g].folder, games[g].exe); say(b); screen = SC_MENU; return; }
    auto_session = from_auto;
    screen = SC_RUNNING; run_since = SDL_GetTicks64();
}

static int bar = -1;                              /* the bottom bar's buttons with the keys / a pad: -1 none, 0 Settings (Games), 1 Quit */
static void act(int key, SDL_Window *win)        /* key: SDLK_UP/DOWN/LEFT/RIGHT/RETURN/ESCAPE (pads are mapped to these) */
{
    if (screen == SC_COUNTDOWN || screen == SC_RESTART) { screen = SC_MENU; logm("countdown stopped: the menu"); return; }
    if (bar >= 0 && (screen == SC_MENU || screen == SC_SETTINGS)) {   /* reached by Down past the last item; Up / Back return */
        if (key == SDLK_LEFT || key == SDLK_RIGHT) bar ^= 1;
        else if (key == SDLK_UP || key == SDLK_ESCAPE) bar = -1;
        else if (key == SDLK_RETURN) { screen = bar == 1 ? SC_QUIT : screen == SC_MENU ? SC_SETTINGS : SC_MENU; bar = -1; }
        return;
    }
    if (screen == SC_MENU) {
        const game_t *g = &games[sel_game];
        if (focus == 0) {
            if (key == SDLK_UP) { sel_game = (sel_game + NGAMES - 1) % NGAMES; sel_mode = 0; }
            else if (key == SDLK_DOWN) { if (sel_game == NGAMES - 1) bar = 0; else { sel_game++; sel_mode = 0; } }
            else if (key == SDLK_RIGHT || key == SDLK_RETURN) { focus = 1; sel_mode = 0; }
            else if (key == SDLK_ESCAPE) screen = SC_QUIT;
        } else {
            if (key == SDLK_UP) do sel_mode = (sel_mode + g->nmodes - 1) % g->nmodes; while (!way_shown(&g->m[sel_mode]));
            else if (key == SDLK_DOWN) { int n = sel_mode; do n = (n + 1) % g->nmodes; while (!way_shown(&g->m[n])); if (n <= sel_mode) bar = 0; else sel_mode = n; }
            else if (key == SDLK_LEFT || key == SDLK_ESCAPE) focus = 0;
            else if (key == SDLK_RETURN) start_mode(sel_game, sel_mode, false);
        }
    } else if (screen == SC_SETTINGS) {
        const int d = key == SDLK_LEFT ? -1 : 1;
        if (key == SDLK_UP) sel_set = (sel_set + SET_N - 1) % SET_N;
        else if (key == SDLK_DOWN) { if (sel_set == SET_N - 1) bar = 0; else sel_set++; }
        else if (key == SDLK_ESCAPE) screen = SC_MENU;
        else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_RETURN) {
            switch (sel_set) {
            case SET_LINK: S.link = !S.link; cfg_save(); break;
            case SET_AUTO: auto_next(d); cfg_save(); run_key_set(S.auto_game >= 0); break;
            case SET_DELAY: { int i = 0; while (i < 4 && delays[i] != S.delay) i++; i = (i + d + 5) % 5; S.delay = delays[i]; cfg_save(); break; }
            case SET_CLOSED: S.restart = !S.restart; cfg_save(); break;
            case SET_TESTS: S.tests = !S.tests; cfg_save(); if (!way_shown(&games[sel_game].m[sel_mode])) sel_mode = 0; break;
            case SET_SCREEN: S.windowed = !S.windowed; cfg_save();
                SDL_SetWindowFullscreen(win, S.windowed ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); break;
            case SET_LIGHTS: all_set("lights", !all_on("lights", "bottom-left")); break;
            case SET_RECORD: all_set("record", !all_on("record", "on")); break;
            }
        }
    }
}

/* one frame of the current screen; mouse clicks act directly */
static void draw(int W, int H, SDL_Window *win)
{
    const float k = H / 1080.0f, pad = 40 * k;
    if (!nk_begin(ctx, "launcher", nk_rect(0, 0, (float)W, (float)H), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) { nk_end(ctx); return; }
    struct nk_command_buffer *cb = nk_window_get_canvas(ctx);
    const struct nk_input *in = &ctx->input;
    nk_fill_rect(cb, nk_rect(0, 0, (float)W, (float)H), 0, C_BG);
    /* the title bar */
    nk_fill_rect(cb, nk_rect(0, 0, (float)W, 112 * k), 0, C_DARK);
    nk_fill_rect(cb, nk_rect(0, 112 * k, (float)W, 1), 0, C_LINE);
    text(cb, f_title, pad, 28 * k, "Namco 22 Arcade LAN", C_TEXT);     /* (Quit, bottom right, closes it: no second close button) */
    text(cb, f_small, pad + tw(f_title, "Namco 22 Arcade LAN") + 24 * k, 48 * k, "System 22 and System 23 games, one PC in each cabinet, linked over a wired network", C_SEC);
    const float top = 144 * k, bottom = H - 96 * k;

    if (screen == SC_MENU) {
        /* the games: a list that scrolls (the mouse wheel / two fingers on a touchpad, the bar on its right, or the chosen game kept
         * in view when the arrows or a pad move it) */
        const float lw = W * 0.30f, rowh = 76 * k, step = rowh + 8 * k, lh = bottom - top;
        const struct nk_rect list = nk_rect(pad, top, lw - pad, lh);
        const float maxs = NGAMES * step - 8 * k > lh ? NGAMES * step - 8 * k - lh : 0;
        static float scroll; static int shown_sel = -1, dragging;
        if (nk_input_is_mouse_hovering_rect(in, list) && in->mouse.scroll_delta.y != 0) scroll -= in->mouse.scroll_delta.y * step * 0.6f;
        if (sel_game != shown_sel) {                                         /* moved by the keys / a pad (or a hover): into view */
            if (top + sel_game * step - scroll < top) scroll = sel_game * step;
            if (top + sel_game * step + rowh - scroll > bottom) scroll = sel_game * step + rowh - lh;
            shown_sel = sel_game;
        }
        const struct nk_rect bar = nk_rect(list.x + list.w + 6 * k, top, 10 * k, lh);
        if (maxs > 0) {
            if (nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, nk_rect(bar.x - 8 * k, bar.y, bar.w + 16 * k, bar.h), nk_true)) dragging = 1;
            if (!in->mouse.buttons[NK_BUTTON_LEFT].down) dragging = 0;
            const float th = lh * lh / (lh + maxs);                          /* the thumb */
            if (dragging) scroll = (in->mouse.pos.y - top - th / 2) / (lh - th) * maxs;
            if (scroll < 0) scroll = 0;
            if (scroll > maxs) scroll = maxs;
            nk_fill_rect(cb, bar, 5 * k, C_PANEL);
            nk_fill_rect(cb, nk_rect(bar.x, top + scroll / maxs * (lh - th), bar.w, th), 5 * k, dragging ? C_SEC : C_SEL);
        } else scroll = 0;
        nk_push_scissor(cb, list);
        for (int i = 0; i < NGAMES; i++) {
            struct nk_rect r = nk_rect(pad, top + i * step - scroll, lw - pad, rowh);
            if (r.y + r.h < top || r.y > bottom) continue;                   /* scrolled out of the list */
            const bool in_list = nk_input_is_mouse_hovering_rect(in, list) && !dragging;
            if (in_list && nk_input_is_mouse_hovering_rect(in, r) && in->mouse.delta.x + in->mouse.delta.y != 0) { if (sel_game != i) sel_mode = 0; sel_game = i; shown_sel = i; focus = 0; }
            if (in_list && nk_input_mouse_clicked(in, NK_BUTTON_LEFT, r)) { sel_game = i; shown_sel = i; focus = 1; sel_mode = 0; }
            const bool s = i == sel_game;
            nk_fill_rect(cb, r, 8 * k, s ? C_SEL : C_PANEL);                 /* chosen: one surface step up and the amber bar */
            if (s) nk_fill_rect(cb, nk_rect(r.x, r.y + 16 * k, 4 * k, r.h - 32 * k), 2 * k, focus == 0 ? C_AMBER : C_DIM);
            text(cb, f_big, r.x + 24 * k, r.y + 8 * k, games[i].name, s ? C_TEXT : C_SEC);
            char sub[64]; snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", games[i].year, cabinets_text(&games[i]));
            text(cb, f_small, r.x + 24 * k, r.y + 44 * k, sub, C_DIM);
        }
        nk_push_scissor(cb, nk_rect(0, 0, (float)W, (float)H));
        /* the chosen game: picture, lines, the ways to play */
        const game_t *g = &games[sel_game];
        const float rx = lw + pad, rw = W - rx - pad;
        float ph = rw * 0.58f * 9 / 16; if (ph > (bottom - top) * 0.46f) ph = (bottom - top) * 0.46f;
        const float pw = ph * 16 / 9, px = rx;                              /* the picture (16:9, an action shot): the screen's one bright thing */
        nk_fill_rect(cb, nk_rect(px - 1, top - 1, pw + 2, ph + 2), 8 * k, C_LINE);   /* a hairline round it */
        nk_fill_rect(cb, nk_rect(px, top, pw, ph), 8 * k, C_DARK);
        if (pics[sel_game]) {
            float iw = pw, ih = pw * pic_h[sel_game] / pic_w[sel_game];
            if (ih > ph) { ih = ph; iw = ph * pic_w[sel_game] / pic_h[sel_game]; }
            struct nk_image im = nk_image_id((int)pics[sel_game]);
            nk_draw_image(cb, nk_rect(px + (pw - iw) / 2, top + (ph - ih) / 2, iw, ih), &im, nk_rgb(255, 255, 255));
        } else text_c(cb, f_mid, px + pw / 2, top + ph / 2 - 14 * k, "(no picture)", C_DIM);
        if (logos[sel_game]) {                                              /* the game's own title logo beside it, on the page */
            const float lx = px + pw + 40 * k, lbw = rx + rw - lx, lbh = ph * 0.8f;
            if (lbw > 120 * k) {
                float iw = lbw, ih = lbw * logo_h[sel_game] / logo_w[sel_game];
                if (ih > lbh) { ih = lbh; iw = lbh * logo_w[sel_game] / logo_h[sel_game]; }
                struct nk_image im = nk_image_id((int)logos[sel_game]);
                nk_draw_image(cb, nk_rect(lx + (lbw - iw) / 2, top + (ph - ih) / 2, iw, ih), &im, nk_rgb(255, 255, 255));
            }
        }
        float y = top + ph + 20 * k;
        text(cb, f_mid, rx, y, g->line1, C_TEXT); y += 36 * k;
        text(cb, f_small, rx, y, g->line2, C_DIM); y += 52 * k;
        /* the ways to play, in groups under a heading each: PLAY (a cabinet), TEST ON THIS PC, PARTY. The left column has PLAY (and the
         * tests when there are Party ways), the right one the Party ways (or the tests); a button shows its label without the group's
         * prefix ("Test: ", "Party: "). The arrows go through them in their own order: down the left column, then the right. */
        int kind[12], has_party = 0, has_test = 0;
        for (int i = 0; i < g->nmodes; i++) {
            if (!way_shown(&g->m[i])) { kind[i] = -1; continue; }
            kind[i] = !strncmp(g->m[i].label, "Party: ", 7) ? 2 : !strncmp(g->m[i].label, "Test: ", 6) ? 1 : 0;
            has_party |= kind[i] == 2; has_test |= kind[i] == 1;
        }
        const int two = has_party || has_test;
        const float mh = 48 * k, gap = 8 * k, headh = 32 * k, cgap = 24 * k,
                    mw = two ? (rw - cgap) / 2 : pw;                         /* one column: as wide as the picture above it */
        static const char *heads[3] = { "Play", "Test on this PC", "Party  \xC2\xB7  split screen on this PC" };
        float cy[2] = { y, y }; int last_kind[2] = { -1, -1 }; float lowest = y;
        for (int i = 0; i < g->nmodes; i++) {
            if (kind[i] < 0) continue;                                       /* a Test way, Test modes off */
            const int col = !two ? 0 : kind[i] == 2 ? 1 : kind[i] == 1 ? (has_party ? 0 : 1) : 0;
            if (kind[i] != last_kind[col]) {                                 /* a new group: its heading (closer to its buttons than to the group above) */
                if (last_kind[col] >= 0) cy[col] += 16 * k;
                const char *hd = heads[kind[i]];
                if (kind[i] == 0 && link_file(g)) hd = S.link ? "Play  \xC2\xB7  link play on" : "Play  \xC2\xB7  link play off (Settings)";
                text(cb, f_small, rx + col * (mw + cgap), cy[col], hd, kind[i] == 0 && link_file(g) && S.link ? C_AMBER : C_SEC);
                cy[col] += headh; last_kind[col] = kind[i];
            }
            struct nk_rect r = nk_rect(rx + col * (mw + cgap), cy[col], mw, mh);
            cy[col] += mh + gap;
            if (r.y + r.h > bottom) continue;
            if (r.y + r.h > lowest) lowest = r.y + r.h;
            if (nk_input_is_mouse_hovering_rect(in, r) && in->mouse.delta.x + in->mouse.delta.y != 0) { focus = 1; sel_mode = i; }
            const bool s = focus == 1 && i == sel_mode;
            if (!s) nk_fill_rect(cb, nk_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 8 * k, C_LINE);
            nk_fill_rect(cb, r, 8 * k, s ? C_AMBER : C_PANEL);                /* the chosen way: the amber, nothing else is */
            char lab[96];                                                    /* the label under its heading: no "Test: " / "Party: ", no
                                                                              * " on this PC" (the heading says it), a capital first */
            snprintf(lab, sizeof lab, "%s", g->m[i].label + (kind[i] == 2 ? 7 : kind[i] == 1 ? 6 : 0));
            { char *t = strstr(lab, " on this PC"); if (t && kind[i] == 1 && !t[11]) *t = 0; }
            if (lab[0] >= 'a' && lab[0] <= 'z') lab[0] = (char)(lab[0] - 32);
            text(cb, f_mid, r.x + 20 * k, r.y + 9 * k, lab, s ? C_DARK : C_TEXT);
            if (nk_input_mouse_clicked(in, NK_BUTTON_LEFT, r)) { sel_mode = i; focus = 1; start_mode(sel_game, i, false); }
        }
        if (focus == 1) {                                                    /* what the chosen way does, under the buttons */
            const float hy = lowest + 16 * k < bottom - 24 * k ? lowest + 16 * k : bottom - 24 * k;
            text(cb, f_small, rx, hy, no_stop(g->m[sel_mode].hint), C_SEC);
        }
    } else if (screen == SC_SETTINGS) {
        static const char *names[SET_N] = { "Link play", "Start a game when Windows starts", "Countdown before the game starts", "When the game closes",
                                             "Launcher display", "Test modes", "Link status lights (all games)", "Session recordings (all games)",
                                             "This PC's network address" };
        static const char *hints[SET_N] = {
            "On: each game looks for the other cabinets when it starts (Start or S plays alone). Off: games play on their own straight away.",
            "When Windows signs in, the launcher opens, counts down and starts this game. On a cabinet PC, set Windows to sign in automatically.",
            "How long the countdown waits before the game starts. Any key or button during the countdown opens the menu instead.",
            "After an automatic start: restart the game whenever it closes, so the cabinet always returns to attract mode.",
            "Show this launcher full screen or in a window.",
            "Shows the Test ways to play: several linked windows on this PC, to check the link without a second cabinet.",
            "Small lights in a corner of each linked game that show which cabinets it can hear.",
            "Each game keeps its last session's inputs (a few KB) so a problem can be replayed. Off keeps none.",
            "The address to type into the other cabinets' Network page when setting the addresses by hand." };
        text(cb, f_big, pad, top, "Settings", C_TEXT);
        float y = top + 64 * k; const float rowh = 60 * k;
        const float w = W - 2 * pad < 1200 * k ? W - 2 * pad : 1200 * k;   /* a column, so a name and its value are read together */
        static char addrs[4][48]; static int na; static Uint64 addr_at;      /* this PC's addresses, read every 2 s */
        if (SDL_GetTicks64() >= addr_at) { na = my_addresses(addrs, 4); addr_at = SDL_GetTicks64() + 2000; }
        for (int i = 0; i < SET_N; i++) {
            struct nk_rect r = nk_rect(pad, y + i * (rowh + 8 * k), w, rowh);
            if (nk_input_is_mouse_hovering_rect(in, r) && in->mouse.delta.x + in->mouse.delta.y != 0) sel_set = i;
            const bool s = i == sel_set;
            nk_fill_rect(cb, r, 8 * k, s ? C_SEL : C_PANEL);
            if (s) nk_fill_rect(cb, nk_rect(r.x, r.y + 16 * k, 4 * k, r.h - 32 * k), 2 * k, C_AMBER);
            text(cb, f_mid, r.x + 24 * k, r.y + 14 * k, names[i], s ? C_TEXT : C_SEC);
            char v[200] = "";
            switch (i) {
            case SET_LINK: snprintf(v, sizeof v, "%s", S.link ? "On" : "Off"); break;
            case SET_AUTO: if (S.auto_game < 0) snprintf(v, sizeof v, "Off"); else snprintf(v, sizeof v, "%s: %s", games[S.auto_game].name, games[S.auto_game].m[S.auto_mode].label); break;
            case SET_DELAY: snprintf(v, sizeof v, "%d seconds", S.delay); break;
            case SET_CLOSED: snprintf(v, sizeof v, "%s", S.restart ? "Restart the game" : "Return to this menu"); break;
            case SET_SCREEN: snprintf(v, sizeof v, "%s", S.windowed ? "Window" : "Full screen"); break;
            case SET_TESTS: snprintf(v, sizeof v, "%s", S.tests ? "Shown" : "Hidden"); break;
            case SET_LIGHTS: snprintf(v, sizeof v, "%s", all_on("lights", "bottom-left") ? "On" : "Off"); break;
            case SET_RECORD: snprintf(v, sizeof v, "%s", all_on("record", "on") ? "On" : "Off"); break;
            case SET_ADDR: if (na) snprintf(v, sizeof v, "%s%s%s", addrs[0], na > 1 ? "   " : "", na > 1 ? addrs[1] : ""); else snprintf(v, sizeof v, "No network"); break;
            }
            text(cb, f_mid, r.x + w - tw(f_mid, v) - 24 * k, r.y + 14 * k, v, s ? C_TEXT : C_DIM);
            if (nk_input_mouse_clicked(in, NK_BUTTON_LEFT, r)) { sel_set = i; act(SDLK_RETURN, win); }
        }
        if (hints[sel_set][0]) text(cb, f_small, pad + 24 * k, y + SET_N * (rowh + 8 * k) + 12 * k, no_stop(hints[sel_set]), C_SEC);   /* under the rows */
    } else if (screen == SC_COUNTDOWN || screen == SC_RESTART) {
        const game_t *g = &games[S.auto_game >= 0 ? S.auto_game : 0];
        const int left = (int)((count_until > SDL_GetTicks64() ? count_until - SDL_GetTicks64() : 0) + 999) / 1000;
        char b[200]; snprintf(b, sizeof b, "%s %s in %d", screen == SC_RESTART ? "Restarting" : "Starting", g->name, left);
        text_c(cb, f_title, W / 2.0f, H * 0.38f, b, C_TEXT);
        text_c(cb, f_mid, W / 2.0f, H * 0.38f + 80 * k, g->m[S.auto_mode].label, C_AMBER);
        text_c(cb, f_mid, W / 2.0f, H * 0.62f, "Press any key or button to open the menu", C_DIM);
    } else if (screen == SC_QUIT) {
        text_c(cb, f_big, W / 2.0f, H * 0.40f, "Close the launcher?", C_TEXT);
        const float kw = 2 * (16 * k + 4 * k) + tw(f_small, "Enter") + tw(f_small, "A") + 8 * k + tw(f_small, "Quit") + 28 * k
                       + 2 * (16 * k + 4 * k) + tw(f_small, "Esc") + tw(f_small, "B") + 8 * k + tw(f_small, "Cancel");
        float kx = (W - kw) / 2;
        kx = keyhint(cb, kx, H * 0.40f + 64 * k, k, "Enter|A", "Quit");
        keyhint(cb, kx, H * 0.40f + 64 * k, k, "Esc|B", "Cancel");
    }
    /* the bottom bar: the keys as keycaps, then Settings / Games and Quit */
    nk_fill_rect(cb, nk_rect(0, H - 64 * k, (float)W, 64 * k), 0, C_DARK);
    nk_fill_rect(cb, nk_rect(0, H - 64 * k, (float)W, 1), 0, C_LINE);
    if (screen == SC_MENU || screen == SC_SETTINGS) {
        const float ky = H - 48 * k;
        float kx = pad;
        if (screen == SC_MENU) {
            kx = keyhint(cb, kx, ky, k, "Arrows|D-pad", "Choose"); kx = keyhint(cb, kx, ky, k, "Enter|A", "Start");
            kx = keyhint(cb, kx, ky, k, "Esc|B", "Back"); keyhint(cb, kx, ky, k, "S|Y", "Settings");
        } else {
            kx = keyhint(cb, kx, ky, k, "Up|Down", "Choose"); kx = keyhint(cb, kx, ky, k, "Left|Right|Enter", "Change");
            keyhint(cb, kx, ky, k, "Esc|B", "Back");
        }
        const char *bt[2] = { screen == SC_MENU ? "Settings" : "Games", "Quit" };
        float bx = W - pad;
        for (int i = 1; i >= 0; i--) {
            const float bw = tw(f_mid, bt[i]) + 40 * k; bx -= bw;
            struct nk_rect r = nk_rect(bx, H - 56 * k, bw, 48 * k);
            const bool h = nk_input_is_mouse_hovering_rect(in, r) || bar == i;
            if (bar == i) nk_fill_rect(cb, nk_rect(r.x - 3 * k, r.y - 3 * k, r.w + 6 * k, r.h + 6 * k), 10 * k, C_AMBER);   /* chosen with the keys / a pad */
            nk_fill_rect(cb, nk_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 8 * k, C_LINE);
            nk_fill_rect(cb, r, 8 * k, h ? C_SEL : C_PANEL);                 /* hover: one surface step up */
            text(cb, f_mid, r.x + 20 * k, r.y + 9 * k, bt[i], h ? C_TEXT : C_SEC);
            if (nk_input_mouse_clicked(in, NK_BUTTON_LEFT, r)) { if (i == 1) screen = SC_QUIT; else screen = screen == SC_MENU ? SC_SETTINGS : SC_MENU; }
            bx -= 12 * k;
        }
    }
    if (message[0] && SDL_GetTicks64() < message_until) {                   /* a message: a raised box with a hairline */
        const float mw = tw(f_mid, message) + 48 * k;
        const struct nk_rect r = nk_rect((W - mw) / 2, H - 144 * k, mw, 52 * k);
        nk_fill_rect(cb, nk_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 10 * k, C_LINE);
        nk_fill_rect(cb, r, 10 * k, C_SEL);
        text_c(cb, f_mid, W / 2.0f, r.y + 11 * k, message, C_TEXT);
    }
    nk_end(ctx);
}

/* ---- MAIN --------------------------------------------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    GetModuleFileNameA(NULL, base, MAX_PATH);
    { char *s = strrchr(base, '\\'); if (s) s[1] = 0; }
    bool want_auto = false;
    const char *shot = NULL; int shot_screen = SC_MENU, shot_game = 0, shot_focus = 0;   /* --shot FILE.bmp [menu|ways|settings] [game]: a picture of one screen */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') { i++; shot_screen = !strcmp(argv[i], "settings") ? SC_SETTINGS : SC_MENU; shot_focus = !strcmp(argv[i], "ways"); }
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') shot_game = atoi(argv[++i]);
            S.windowed = 2;
        }
        else if (!strcmp(argv[i], "--auto")) want_auto = true;
        else if (!strcmp(argv[i], "--window")) S.windowed = 2;
        else if (!strcmp(argv[i], "--write-advanced") && i + 1 < argc) { write_advanced(argv[++i]); return 0; }
        else if (!strcmp(argv[i], "--list")) {
            for (int g = 0; g < NGAMES; g++) for (int m = 0; m < games[g].nmodes; m++) printf("%s:%d  %s - %s\n", games[g].id, m, games[g].name, games[g].m[m].label);
            return 0;
        }
    }
    { char p[MAX_PATH]; snprintf(p, sizeof p, "%slauncher.log", base); log_file = fopen(p, "a"); }
    const int forced_window = S.windowed == 2;
    cfg_load();
    if (forced_window) S.windowed = 1;
    logm("---- " APP_NAME " started%s", want_auto ? " by Windows (--auto)" : "");

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { MessageBoxA(NULL, SDL_GetError(), APP_NAME, MB_ICONERROR); return 1; }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_DisplayMode dm; SDL_GetDesktopDisplayMode(0, &dm);
    SDL_Window *win = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, shot ? 1600 : dm.w * 3 / 4, shot ? 900 : dm.h * 3 / 4,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (S.windowed ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP));
    if (!win) { MessageBoxA(NULL, SDL_GetError(), APP_NAME, MB_ICONERROR); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(win); SDL_GL_MakeCurrent(win, gl); SDL_GL_SetSwapInterval(1);
    ctx = nk_sdl_init(win);
    ctx->style.window.border = 0; ctx->style.window.padding = nk_vec2(0, 0);      /* the screen is drawn by hand: no window frame */
    ctx->style.window.fixed_background = nk_style_item_color(C_BG);
    int W, H; SDL_GetWindowSize(win, &W, &H); int fonts_h = H;
    fonts((float)H);
    load_pictures();
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

    if (want_auto && S.auto_game >= 0) { screen = SC_COUNTDOWN; count_until = SDL_GetTicks64() + (Uint64)S.delay * 1000; sel_game = S.auto_game; }
    else if (want_auto) logm("--auto, but no automatic start is set: the menu");
    if (shot) { screen = shot_screen; sel_game = shot_game % NGAMES; focus = shot_focus; }
    Uint64 next_check = 0, gone_since = 0; int frames = 0;
    while (!want_quit) {
        SDL_Event e;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { if (screen != SC_RUNNING) want_quit = true; continue; }
            if (e.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(e.cdevice.which);
            if (screen == SC_RUNNING) continue;                          /* the game has the screen */
            int key = 0;
            if (e.type == SDL_KEYDOWN) {
                key = e.key.keysym.sym;
                if (key == SDLK_KP_ENTER || key == SDLK_SPACE) key = SDLK_RETURN;
                if (key == SDLK_BACKSPACE) key = SDLK_ESCAPE;
                if ((key == SDLK_s || key == SDLK_F1) && (screen == SC_MENU || screen == SC_SETTINGS)) { screen = screen == SC_MENU ? SC_SETTINGS : SC_MENU; key = 0; }
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: key = SDLK_UP; break;        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: key = SDLK_DOWN; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: key = SDLK_LEFT; break;    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key = SDLK_RIGHT; break;
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: key = SDLK_RETURN; break;
                case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: key = SDLK_ESCAPE; break;
                case SDL_CONTROLLER_BUTTON_Y: if (screen == SC_MENU || screen == SC_SETTINGS) screen = screen == SC_MENU ? SC_SETTINGS : SC_MENU; break;
                }
            } else if (e.type == SDL_CONTROLLERAXISMOTION && (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY || e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)) {
                static int held[2];                                      /* the stick as a D-pad: once per push */
                const int a = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY, v = e.caxis.value;
                const int d = v > 20000 ? 1 : v < -20000 ? -1 : 0;
                if (d && d != held[a]) key = a ? (d > 0 ? SDLK_DOWN : SDLK_UP) : (d > 0 ? SDLK_RIGHT : SDLK_LEFT);
                held[a] = d;
            } else if ((e.type == SDL_MOUSEBUTTONDOWN) && (screen == SC_COUNTDOWN || screen == SC_RESTART)) key = SDLK_ESCAPE;
            if (screen == SC_QUIT && key) { if (key == SDLK_RETURN) want_quit = true; else if (key == SDLK_ESCAPE) screen = SC_MENU; continue; }
            if (key) act(key, win);
            if (!shot) nk_sdl_handle_event(&e);                               /* (--shot: no mouse, the picture shows the chosen row) */
        }
        nk_input_end(ctx);
        const Uint64 now = SDL_GetTicks64();
        if ((screen == SC_COUNTDOWN || screen == SC_RESTART) && now >= count_until) {
            logm("countdown over: starting %s - %s", games[S.auto_game].name, games[S.auto_game].m[S.auto_mode].label);
            start_mode(S.auto_game, S.auto_mode, true);
        } else if (screen == SC_RUNNING && SDL_GetWindowFlags(win) & SDL_WINDOW_SHOWN) {
            HWND gw = game_window();                                     /* hide once the game's window is up, and give it the focus */
            if (gw || now - run_since > 20000) { if (gw) SetForegroundWindow(gw); SDL_HideWindow(win); }
        }
        if (screen == SC_RUNNING) {                                      /* the watchdog: every second */
            if (now >= next_check) {
                next_check = now + 1000;
                if (ours_running() || game_running(run_game)) gone_since = 0;
                else if (!gone_since) gone_since = now;
                else if (now - gone_since >= 3000) {                     /* gone for 3 s (a restart by the game itself is quicker) */
                    forget_procs(); gone_since = 0;
                    logm("%s has closed", games[run_game].name);
                    take_focus(win);
                    if (auto_session && S.restart && S.auto_game >= 0) { screen = SC_RESTART; count_until = now + 5000; }
                    else screen = SC_MENU;
                }
            }
            SDL_Delay(100);
            continue;
        }
        SDL_GetWindowSize(win, &W, &H);
        if (H != fonts_h && H > 100) { fonts((float)H); fonts_h = H; }
        draw(W, H, win);
        int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh); glClearColor(C_BG.r / 255.0f, C_BG.g / 255.0f, C_BG.b / 255.0f, 1); glClear(GL_COLOR_BUFFER_BIT);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        if (shot && ++frames == 5) {                                     /* --shot: the picture of this screen, then close */
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 24, SDL_PIXELFORMAT_RGB24);
            unsigned char *row = malloc((size_t)dw * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            for (int y = 0; y < dh; y++) { glReadPixels(0, dh - 1 - y, dw, 1, GL_RGB, GL_UNSIGNED_BYTE, row); memcpy((unsigned char *)s->pixels + y * s->pitch, row, (size_t)dw * 3); }
            SDL_SaveBMP(s, shot); SDL_FreeSurface(s); free(row);
            want_quit = true;
        }
        SDL_GL_SwapWindow(win);
    }
    logm("closed");
    nk_sdl_shutdown(); SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
