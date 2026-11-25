RIBBON 2.0 - WINDOWS 11 BUILD & SETUP KIT
=========================================

QUICK START (Windows 11)
------------------------
1. Extract this ZIP folder anywhere (e.g., Desktop or D:\Ribbon).
2. Double-click `build.bat`.
   - If you already have MinGW-w64 (g++) or Visual Studio C++ installed,
     it compiles `dist\Ribbon.exe` automatically.
   - If you have Inno Setup 6 installed, it also compiles `dist\RibbonSetup.exe`.
3. If you don't have a C++ compiler yet, open Windows Terminal / PowerShell and run:
     winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
     winget install -e --id JRSoftware.InnoSetup
   Then close & re-open the terminal (or restart Explorer) and run `build.bat`.

INSTALLING WITHOUT INNO SETUP
-----------------------------
After `build.bat` creates `dist\Ribbon.exe`, you can either:
- Run `dist\Ribbon.exe` directly as a portable app, OR
- Right-click `Install-Ribbon-Portable.ps1` -> "Run with PowerShell" to install it
  to your Start Menu, Desktop, and associate `.rib` project files automatically.

CONTROLS & SHORTCUTS
--------------------
- Space / Esc        : Play / Stop
- Left-click & Drag  : Toggle / paint steps across the grid
- Alt+Click / Mid    : Toggle step Accent (boosted punch)
- Right-click (synth): Shift note up in current Scale (Shift+Right-click = down)
- Mouse Wheel        : Adjust Tempo, Swing, Sliders, or step Pitch directly
- 1 - 8              : Select channel (Kick, Snare, Hat, Clap, Bass, Keys, Pad, Lead)
- M / S              : Mute / Solo selected channel
- F1 - F4            : Switch Pattern A / B / C / D
- Tab                : Toggle Song Mode (chains A -> B -> C -> D)
- Ctrl+Z             : Undo (32 levels)
- Ctrl+C / Ctrl+V    : Copy / Paste pattern
- Ctrl+S / Ctrl+O    : Save / Open .rib project (supports v1 and v2 files)
- Ctrl+E             : Export 16-bit 44.1kHz Stereo WAV (with reverb/delay tail)
