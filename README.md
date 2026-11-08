# Pbar

Live-preview vertical taskbar for Windows 11 (also runs on Windows 10). A sibling of [Vbar](https://github.com/sinephase/vbar): instead of a list of buttons, every open window is a live picture of itself in a column that wraps around endlessly, with a mini terminal at the top and a smartwatch-style clock at the bottom. The whole bar is at most 150 px wide.

- **Live previews.** Each window's real contents, updated as they change. The focused window sits in the middle at full size, and each step away from it is smaller.
- **Endless column.** The list wraps around, so the mouse wheel spins it forever in either direction.
- **Hover to enlarge.** The preview under the mouse becomes the big one, its neighbours step down around it, and it pops out a few pixels past the bar's edge. When the mouse leaves, the focused window slides back to the middle.
- **Mini terminal.** PowerShell, PowerShell 7 or cmd runs hidden at the top of the bar. Type a command and read its output; select and copy text; Ctrl+C interrupts. A global hotkey (Ctrl+Alt+T) jumps to it, and pressing it again takes you back.
- **Smartwatch clock.** Big hour and minute digits, the day and date, and seconds, minutes and hours rings. Click it for a calendar.
- **Settings window.** Every option, each with a plain description of what it does, on a handful of pages. Changes are written into `pbar.ini`, so the file and the window always agree.
- **Pinned apps** (your own, plus your Windows Start menu pins) and a `░▒▓█` **start button** at the very bottom
- **2-bit icons.** Each app's real icon reduced to transparent plus three grey shades, optionally dithered.
- **Per-app audio meter.** A thin bar along the bottom of any preview whose app is playing sound.
- **System tray.** A small well of the familiar Windows indicators between the pinned apps and the clock: speaker volume (wheel to change, click to mute), network (Wi‑Fi strength or a wired plug, with a warning dot when there's no internet), and battery (laptops only). Hover any of them for details; click through to the matching Windows settings.
- **Attention flashing.** A window asking for attention blinks, then stays lit until you switch to it.
- **Right-click menus:** restore, minimize, maximize, close, end process, open file location, run as administrator
- Reserves its screen edge like the real taskbar, so maximized windows stop beside it
- Any screen, left or right edge
- Single ~3.5 MB static exe, no dependencies, no installer

## Usage

1. Download [`bin/pbar.exe`](bin/pbar.exe) (click it, then **Download raw file**) and put it in its own folder.
2. Run it. It writes `pbar.ini` next to itself on first launch.
3. Right-click the clock and choose **Settings…** to change anything.
4. To start it with Windows, turn on **Start with Windows** on the Bar page of Settings, or tick it in the clock's right-click menu. This adds a per-user startup entry pointing at wherever `pbar.exe` currently is, so if you move the exe, turn it on again.

To replace the Windows taskbar entirely: turn on taskbar auto-hide in Windows settings, then turn on **Hide the Windows taskbar** in Settings. The real taskbar comes back when pbar exits.

Pbar is designed to sit beside Vbar; each reserves its own strip of the screen.

| Mouse | Preview | Pinned app | Terminal | Clock / start button / empty space |
|---|---|---|---|---|
| Hover | Make it the big one (optionally bring it to front) | — | — | — |
| Left-click | Focus, or minimize if already focused | Launch | Type; drag to select, double-click a word | Clock: calendar · start button: Start menu |
| Middle-click | Close | — | — | — |
| Right-click | Window menu | Pin menu | Copy, clear, interrupt, restart, switch shell | Settings, position, 24-hour clock, start with Windows, reload, edit pbar.ini, exit (start button: Win+X menu) |
| Wheel | Spin the column | Scroll the pinned list | Scroll back through output | — |

**Terminal keys:** Enter runs the line, Up / Down step through history, Page Up / Page Down scroll the output, Ctrl+C copies the selection or interrupts the running command, Ctrl+V pastes, Ctrl+L clears, and Esc drops a selection, then clears the line, then goes back to the window you came from.

`pbar.exe --settings` opens the settings window, in the running pbar if there is one.

## Configuration

Everything lives in `pbar.ini` next to the exe. The settings window (right-click the clock → **Settings…**) covers every option with a description, and writes your changes into the file in place, keeping its comments and order. You can also edit the file by hand and right-click → **Reload**. Sizes are physical pixels, colours are `#RRGGBB`, and comments go on their own line.

**Bar:** `side` (`left` / `right`), `monitor` (`primary` or a display number from Settings → Display → Identify), `width` (60–150), `hidetaskbar`, `gap` (between sections), `spacing` (between items), `pad` / `vpad` (text padding), `highlight` (bevel glint length, 0 = full edges), `hoverbright`

**Text:** `font`, `fontsize`, `fontweight` (100–900), `bold`, `italic`, `aa` (0 = crisp, 1 = ClearType), `fontfile` (a font file loaded privately, without installing it), `maxlines`, `linegap`, `ellipsis`, `strip=` (title endings to trim, e.g. `- Google Chrome`)

**Previews:** `previewmargin`, `step` (each preview's size as a % of its neighbour nearer the big one), `minscale`, `thumbpad`, `titlemin` (narrower previews drop their title line), `maxaspect` / `minaspect`, `clientonly`, `minopacity`, `popout` (px the hovered preview sticks out), `animspeed`, `recenterdelay`, `hoverfocus`, `hoverdelay`, `hoverrestore`, `bordermode` (`focused` / `visible` / `off`), `activeborder`

**Terminal:** `terminal`, `shell` (`powershell` / `pwsh` / `cmd`), `termlines`, `termfontsize`, `termdir`, `termkey` (e.g. `Ctrl+Alt+T`, blank = none), `termscrollback`, `termbg`, `termtext`, `termcmd`, `termerr`, `termdim`, `termsel`

**Clock:** `clock`, `clockstyle` (`watch` / `classic`), `clock24`, `watchheight`, `watchfont`, `watchfps` (0 = tick once a second), `watchaccent`, `watchday`, `watchrings` (seconds, minutes, hours), `clockfontsize`, `clocklines`, `clockbold`, `weekstart` (0 = Sunday, 1 = Monday), `caltimeout`

**Start button & pins:** `startbutton`, `startlines`, `startheight` (px, wins over `startlines`), `startcolor`, `pin=Name|path|args|workingdir` (env vars allowed), `startpins` (import Windows Start pins), `pindir` (a folder of shortcuts), `pinsmax` (max % of bar height)

**System tray:** `systray` (the whole well), `sysvolume`, `sysnetwork`, `sysbattery`, `systileh` (row height px), `volumestep` (% the wheel changes the volume), `sysface` (tile colour, blank = same as the pinned apps)

**Colours:** `bg`, `face`, `outline`, `hi`, `activeface`, `pinface`, `clockface`, `text`, `hovertext`, `activetext`, `dim`, `flash`, `flashrate`, `flashcount`, `scrollw`, `scrollcolor`

**Icons:** `icons`, `iconsize` (0 = text line height), `iconpad`, `icondither`, `iconshades` (three colours, darkest to lightest)

**Audio meter:** `meter`, `meterh`, `metercolor`, `meterdb` (dB range across the full width, 0 = linear), `meterfall`, `meterfps`

## Building

Single file: Win32, GDI, GDI+, DWM thumbnails, WASAPI, and the Windows network/WLAN/power APIs, no other dependencies.

MSVC (x64 Native Tools prompt):
```
cl /O2 /EHsc /DUNICODE /D_UNICODE pbar.cpp /link /SUBSYSTEM:WINDOWS
```

MinGW-w64:
```
g++ -O2 -municode -mwindows -static pbar.cpp -o pbar.exe -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -luuid -lmsimg32 -ladvapi32 -lgdiplus -lcomdlg32 -lwlanapi -liphlpapi -lcomctl32
```

## License

MIT. See [LICENSE](LICENSE).
