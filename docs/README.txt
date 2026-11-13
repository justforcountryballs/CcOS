ccos - Linux 6.12 kernel + BusyBox 1.36.1 + ccos desktop

bzImage             the compiled Linux kernel
initramfs.cpio.gz   the root filesystem (BusyBox, init, desktop)
run.bat             double-click to boot ccos in QEMU
ccos-data.img       ccos's save disk (created on first run; holds your settings)
ccos-apps.qcow2     ccos's apps disk (created on first run; holds apps from Flathub)
source\ccos-apps.sh  prepares the apps disk and starts D-Bus (for Flatpak apps)
source\ccos-flatpak-run.sh  starts a Flatpak app (and skips Firefox's first-run screens)
source\ccos-savedisk.sh  finds the save disk (QEMU disk, or the stick's ccos-store part)
source\make_usbimg.sh  builds ccos-usb.img (the USB stick version)
source\mkpart.c    makes the stick's second part for your files and apps
source\kernel.config  the Linux kernel settings ccos is built with
source\init.sh      /init, the first program the kernel runs (boot steps)
source\splash.c     the loading screen: banner, spinning dots, progress bar
source\desktop.c    the desktop: taskbar, start menu, windows and all the apps
source\ccos-shutdown.sh  shutdown / restart / boot-into-shell screen and logic
source\ccos-network.sh   starts cable + Wi-Fi networking at boot
source\wifi-action.sh    gets an IP address when Wi-Fi connects
source\dhcp-script.sh    applies the network settings from DHCP
source\playsound.c  plays .wav files (the startup sound)
source\make_tune.py  the script that synthesized the startup tune
source\wayland.c    makes the desktop a Wayland compositor: real Linux apps
                     (like the Terminal) draw into ccos windows
source\wl\         Wayland protocol code made by wayland-scanner
source\media_apps.c  Photos, Videos and Music
source\media.c      ccos-media: plays a video/sound file with FFmpeg for them
source\avstubs\     makes small stand-ins for video *encoders* FFmpeg links to
                     (ccos only plays, so they're never used; saves 60 MB)
source\store.c      Software Manager, the app store
source\store_index.c  turns Flathub's app catalog into Software Manager's list
source\xwm.c        a tiny window manager for X11 apps (inside the "X11 apps" window)
source\ccos-sound.sh  starts PulseAudio, the sound server apps play through
source\third\      stb_image (PNG) and nanosvg (SVG): app icons from Flathub
source\solitaire.c  the Solitaire game
source\cp437map.h   maps international letters onto the font
source\THIRD-PARTY.txt  the open-source projects ccos uses
source\font8x16.h   classic bitmap font used by the desktop and splash
source\make_smoothfont.py  pre-renders the smooth font into ccos's text grid

Desktop controls:
  Network icon (next to the clock)   click for the network panel: connection
                                     status, Networking on/off, cable Connect /
                                     Disconnect, Wi-Fi on/off, nearby Wi-Fi
                                     networks (click one, type the password,
                                     Connect; Forget removes a saved one),
                                     IP address details.
                                     Wi-Fi needs real hardware: QEMU only gives
                                     ccos a network cable. Intel Wi-Fi cards
                                     (like the Wireless-AC 9560) are supported.
  Taskbar                            pinned apps sit on the taskbar; right-click
                                     a taskbar button for Pin/Unpin and Close.
                                     Right-click start menu programs or desktop
                                     icons to pin them too.
  Desktop icons                      click to select, double-click to open,
                                     drag to move (they snap to a grid and
                                     remember where you put them).
                                     Right-click > Delete removes a shortcut
                                     (the app stays). Right-click a start menu
                                     program > Add to Desktop puts one back.
  Start button /o\ or Windows key   open / close the start menu
  Esc                                close the menu
  Windows                            drag by the title bar; double-click the
                                     title to maximize; _ minimize,
                                     [] maximize/restore, X close; drag an
                                     edge or corner to resize (Notepad, File
                                     Explorer, Terminal, Flathub apps)
  F11                                full screen (and back). Browsers do it
                                     themselves: F11 in Firefox, or a video's
                                     full-screen button. The taskbar hides;
                                     the Windows key still opens the menu.
  Caps Lock                          a popup above the taskbar says on/off,
                                     and the keyboard's Caps Lock light works
  Volume keys (on laptops the F2/F3 keys with the speaker pictures)
                                     change the volume 0-100 with a popup;
                                     the mute key mutes. Also in Settings >
                                     Sound (drag the slider, click the speaker
                                     to mute).
  Brightness keys (laptops)          screen brightness, with a popup. Also in
                                     Settings > Power.
  Battery icon (laptops, next to the network icon)
                                     how full it is; a lightning bolt while
                                     charging. Click it for Settings > Power.
                                     Warnings at 20% and 10%; at 4% ccos shuts
                                     down by itself after a minute unless you
                                     plug in, so nothing is lost.
  Closing the lid                    puts the computer to sleep (Settings >
                                     Power can change that to Do nothing).
                                     Open it or press a key to wake it up.
  Notifications                      cards above the clock with a soft chime:
                                     USB drives, battery, and messages from
                                     apps (Firefox, Discord, mail...). Click
                                     one to go to that app. The chime can be
                                     turned off in Settings > Sound.
  USB sticks and drives              plug one in: a notification says it's
                                     ready and it shows in File Explorer
                                     (Computer > Removable Drives, and the left
                                     side). FAT32, exFAT, NTFS, ext4 and CDs work.
                                     Before pulling it out click the eject
                                     button next to its name (or right-click >
                                     Eject) and wait for "Safe to remove".
  The clock (bottom right)           click it: the time, the date and a month
                                     calendar (arrows for other months), and
                                     your time zone. "Change time zone and time
                                     settings" opens Settings > Clock.
  Taskbar buttons                    show / minimize a window
  Print Screen (PrtSc)               a screenshot of the whole screen: saved in
                                     Images > Screenshots and copied, so
                                     Ctrl+V pastes it into apps (Discord...)
  Windows + Shift + S                drag a box around part of the screen for a
                                     screenshot of just that part (Esc cancels)
  Windows + Alt + R                  start / stop recording the screen (Record)
  Windows + V                        clipboard history: the last 25 things you
                                     copied (text and pictures). Pick one (click,
                                     or arrows + Enter) and it's pasted where you
                                     were. Delete removes one. Text copied in
                                     ccos apps now pastes into Firefox and other
                                     apps too, and the other way round.
  The bell (right of the clock)      notification centre: every notification is
                                     kept here after its card goes away. The
                                     number on the bell is how many you missed.
  Start menu search                  open the start menu and just type: apps,
                                     Settings pages ("wifi", "time", "dark")
                                     and files/folders on D: and in Downloads.
                                     Enter opens the top one, Up/Down pick,
                                     Esc clears.
  Start menu, bottom buttons:
    Shut down                        shut down ccos
    Restart                          restart ccos
    Sleep                            sleep (wake it with a key or the power button)
  QEMU menu: Machine > Power Down    also shuts down properly
  Ctrl+Alt+Backspace                 quit to the shell (type: desktop  to go back)

Apps (start menu):
  Terminal     a real Linux terminal (foot). Type commands like on the plain
               command line, but in a window: several at once, mouse selection,
               copy = Ctrl+Shift+C, paste = Ctrl+Shift+V, scroll with the wheel.
  Photos       look at pictures (JPG, PNG, GIF, BMP, SVG...). Left/Right
               arrows or the buttons go through the folder; + / - or the
               mouse wheel zoom, 0 fits it to the window; drag a zoomed
               picture to move around; double-click for full screen.
  Printing     in Photos click the printer button (or Ctrl+P); in File
               Explorer right-click a picture > Print... ccos finds printers on
               your Wi-Fi by itself (most printers from the last 10 years:
               HP, Canon, Epson, Brother... anything with AirPrint or "IPP
               Everywhere"), no drivers needed. Choose paper (A4, Letter,
               photo 10 x 15 cm...), orientation, whole picture or fill the
               page, color or black & white, quality, copies, and borderless
               if the printer can. The preview shows how it'll come out. The
               printer you used last is picked automatically next time. If
               yours isn't found, type its IP address (on its screen or
               settings page) under "Not listed?". A notification says when
               it's printed. Terminal: ccos-print list / ccos-print image FILE URI
  Videos       plays MP4, MKV, WebM, AVI, MOV... with sound. Space or a click
               pauses; drag the bar to jump; Left/Right = 5 s back/forward;
               the corner button, F or a double-click = full screen.
  Music        plays MP3, OGG, FLAC, WAV, M4A, OPUS... shows the song's name,
               artist and album cover; plays the rest of the folder in order.
               Without a file, Photos, Videos and Music show a Library of
               what's in Documents, Downloads, Pictures, Videos and Music.
               Double-clicking a picture, video or sound in File Explorer
               opens the right one. Firefox saves downloads to Downloads.
  Style (Settings > Appearance): Classic is the glass look with the blocky
               pixel font. Modern is flat and solid, with the smooth font,
               smooth icons and new _ [] X buttons; it has Light and Dark
               modes. Dark mode also darkens ccos's own apps (Photos,
               Videos, Solitaire and Flathub apps keep their own colours).
  Desktop background: Settings > Appearance has 7 pictures made for ccos
               (Water is the first one). Or use your own: right-click a picture
               in File Explorer > Set as desktop background. Right-click an
               empty spot on the desktop > Customize opens Settings >
               Appearance, where you choose how your picture sits: Fill, Fit,
               Stretch, Tile, Center or Span, or Remove picture.
  Taskbar (Settings > Appearance): Bar is the usual taskbar with the start
               menu. Dock is a floating shelf in the middle of the bottom:
               point at it and the icons grow, with the name above the one
               under the mouse. First comes /o\ (Finder), then pinned apps
               and open windows (a dot under the open ones), then the Recycle
               Bin. Right-click an icon to pin or unpin it. The clock, network,
               battery and a power button (Sleep, Restart, Shut down) are in
               the bottom right corner.
  Finder       (Dock only: click /o\ or press the Windows key) everything in
               one window, in tabs: Apps, Files, Images (with small previews),
               Videos, Music and Downloads. Just type to search the tab, Tab
               switches tabs, click something (or Enter for the first one) to
               open it (the Finder then closes). Newest files come first.
  Animations   windows pour into their Dock or taskbar icon when minimized
               and pour back out, grow in when they open and fade when they
               close. Settings > Appearance > Window animations turns them off.
  File Explorer's Favorites: Documents (yellow), Images (blue), Downloads
               (green) and Videos (purple). Images and Videos are on D:,
               Downloads is where Firefox and other Flathub apps save.
  Software Manager  (the orange bag) the app store: thousands of apps from
               Flathub. Explore shows popular apps and categories; type to
               search; click an app for its page, then Install. Installing
               shows its progress (the first app also downloads shared parts,
               a few hundred MB). Installed lists your apps with Open, and
               Update all; open an app's page to Remove it. A web browser:
               Firefox, Chrome or Brave are in Popular apps.
  File Explorer  browse ccos's drives and folders. Also in the start menu's
               right column: Documents and Computer.
               C: = ccos System (the OS itself, kept in memory, read-only)
               D: = ccos Data (the save disk: your settings and files)
               Double-click a folder to open it, a text file to open it in
               Notepad. New folder / Rename (F2) / Delete work on D: only.
  Record       records the whole screen into a video (MP4) in Videos. Click
               the red button: the window gets out of the way and a red
               "REC 0:12" button on the taskbar counts; click it (or press
               Windows + Alt + R) to stop. "Record the sound apps make" also
               records what you hear (videos, games, Discord...).
  Camera       the laptop's webcam. White button = a photo (Images > Camera),
               red button = a video with the microphone (Videos > Camera),
               press again to stop. The preview is mirrored, like a mirror;
               the saved photos and videos aren't. If it says "No camera
               found", check the laptop's camera switch or F-key.
  Paint        draw: pencil, brush, eraser, line, box, circle, fill, colour
               picker; 4 sizes, 16 colours. Ctrl+Z / Ctrl+Y undo / redo,
               Ctrl+V pastes the last copied picture (a screenshot), Ctrl+S
               saves to Images ("Drawing ....png"). Right-click a picture in
               File Explorer > Edit in Paint.
  Calendar     a month, with a dot on days that have something. Pick a day,
               type a time (18:30) and what it is, Add reminder; at that time
               a notification pops up, even with Calendar closed. Saved on D:.
  System Monitor  processor and memory graphs for the last minute, D:'s free
               space, the processor temperature, and the running programs
               (busiest first). Pick one and End task stops it.
  Calculator   like the Windows 7 one; also takes keyboard input
               (numbers, + - * /, Enter, Backspace, Esc = clear)
  Solitaire    classic Klondike: click the deck to turn cards, drag cards
               between columns (one lower, other colour; Kings on empty spaces),
               build each suit Ace to King at the top. Double-click sends a card
               up. Game menu: New game (F2), Draw one / Draw three.
  Minesweeper  left-click to open, right-click to flag; click a number
               whose flags are all placed to open around it. F2 = new game.
               Game menu: Beginner / Intermediate / Expert
  Notepad      File menu: New, Open, Save, Save As (Ctrl+N / O / S).
               Files are saved in Documents on ccos's save disk.
               Select text by dragging with the mouse, double-clicking a word,
               or Shift + arrows/Home/End. Ctrl+A select all, Ctrl+C copy,
               Ctrl+X cut, Ctrl+V paste. Also in the Edit menu and on right-click.
  Settings     tabs: Appearance (background, style), Clock (24-hour clock,
               time zone: automatic or pick one (type a city or country to
               search), internet time with Sync now, and "Windows is on this
               computer too"),
               Sound (volume, startup sound, notification chime), Mouse
               (pointer color and size, touchpad speed), Power (battery,
               brightness, turn the screen off after, sleep after, what the
               lid does, Sleep now), Bluetooth (on/off, Search for devices,
               Pair / Connect / Disconnect / Remove), About (system info and
               the graphics chip switch)
  Time         ccos sets its clock from the internet (pool.ntp.org) whenever it's
               online, and finds your time zone from your internet connection
               (ip-api.com) unless you pick one yourself. Windows keeps the
               computer's own clock in local time, and so does ccos (Settings >
               Clock > "Windows is on this computer too"), so both always show
               the right time. Turn that off only if Windows isn't installed.
  Bluetooth    Settings > Bluetooth. Put headphones, a speaker, a mouse or a
               phone in pairing mode, click Search for devices, then Pair.
               Paired devices are remembered and reconnect with Connect.
               Bluetooth headphones play sound like any other speaker.
               (Keyboards that ask you to type a code can't pair yet.)
  Screen off and sleep: by default the screen dims and turns off after 5
               minutes without the mouse or keyboard (not while a video
               plays or something is full screen); move the mouse to wake
               it. Settings > Power can also put ccos to sleep after a while.
File Explorer:
  Selecting                          click; Ctrl+click adds/removes one;
                                     Shift+click picks a range; Ctrl+A all.
                                     Or press on empty space and drag: a blue
                                     box selects every file it touches.
  Copy / Cut / Paste                 Ctrl+C, Ctrl+X, Ctrl+V (or the buttons at
                                     the top, or right-click). Works between
                                     folders and USB sticks; big copies show a
                                     window with progress and Cancel. Same
                                     name already there -> "name (2)".
  Dragging files                     drag selected files onto a folder, a place
                                     on the left (Documents, a USB stick...) or
                                     a folder in the address bar. Same drive =
                                     move, another drive = copy (like Windows);
                                     hold Ctrl to copy. A label by the pointer
                                     says which it will do.
  Delete / F2                        Delete moves the selection to the Recycle
                                     Bin; Shift+Delete deletes for good. F2
                                     renames one file.
  Recycle Bin                        on the desktop and on the left in File
                                     Explorer. Deleted files from D: wait here:
                                     it shows where each came from and when.
                                     Restore puts a file back where it was;
                                     Empty Recycle Bin deletes them for good.
                                     (Files on USB sticks are deleted straight
                                     away, after asking.)
  Right-click a file                 Open, Cut, Copy, Rename, Delete, Compress
                                     to zip (and Extract all for zips, Set as
                                     background and Print for pictures)
  Right-click empty space            Paste, New > (Folder, Text Document, Web
                                     Page, Shell Script, Markdown Notes, CSV
                                     Spreadsheet, Zip File), Select all
  Zip files and archives             "Compress to zip" packs the selection into
                                     one .zip; double-click a .zip (or Extract
                                     all) to unpack it into a folder. Zips from
                                     Windows/macOS work, Polish names too.
                                     .rar, .7z, .tar, .tar.gz, .tar.xz and
                                     .tar.bz2 unpack the same way (not ones
                                     with a password).
  Downloads                          everything apps download (Firefox...) goes
                                     here. Downloads > Flathub has a launcher
                                     for every app installed from Flathub:
                                     double-click one to start it.

Polish letters:
  The keyboard is Polish (programmer's) at first: Alt Gr + a/c/e/l/n/o/s/x/z
  types ą ć ę ł ń ó ś ź ż (with Shift for capitals), in ccos and in apps.
  Settings > Keyboard & mouse switches to English (US). File names, Notepad,
  Wi-Fi names and everything else show Polish letters.

Settings and documents are saved to ccos-data.img. Delete it to reset everything.

Apps from Flathub (Flatpak):
  ccos can run real Linux apps from flathub.org, like Firefox, GIMP,
  LibreOffice or GNOME Calculator. The easy way is Software Manager; the
  Terminal works too:
    flatpak install flathub org.mozilla.firefox        (Firefox)
    flatpak install flathub org.gnome.Calculator       (GNOME Calculator)
    flatpak search gimp                                (find an app's ID)
    flatpak list                                       (what's installed)
    flatpak uninstall org.gnome.Calculator             (remove one)
  Answer y when it asks. The first app downloads the shared "runtime"
  (a few hundred MB), so it takes a while; later apps are quicker.
  Installed apps appear at the bottom of the start menu's program list
  (the first 5) with their own icons, and get a desktop shortcut.
  Right-click to pin to the taskbar or add to the desktop.
  They're also in File Explorer: Downloads > Flathub (double-click to start).
  From the Terminal:  ccos-flatpak-run org.mozilla.firefox
  Apps live on ccos-apps.qcow2 (it grows as you install; delete it to remove
  all apps). On the USB stick, apps are kept on the stick's ccos-store
  part, so they're still there after a restart.
  Apps that draw their own title bar (GTK apps like Calculator) show their
  own _ [] X buttons inside the ccos window; either set works.
  ccos needs 2 GB of memory for these apps (run.bat gives it that).
  Sound: apps play through PulseAudio (started at boot), so YouTube etc.
  have sound. USB headsets are picked up a few seconds after plugging in.
  X11 apps and games: apps that don't speak Wayland (many games, Java/
  Minecraft, older programs) open inside an "X11 apps" window. Alt + drag
  moves their windows; F11 makes the X11 apps window full screen. It closes
  by itself when the last X11 app quits.
  Drivers: Intel, AMD and Nvidia (basic) graphics; Intel, Realtek, MediaTek,
  Qualcomm/Atheros and Broadcom Wi-Fi and Bluetooth; Intel "SOF" sound (newer
  laptops) and USB sound; SD card readers; USB and wired network adapters;
  game controllers (Xbox, PlayStation, Switch, Steam); webcams; USB serial
  (Arduino and similar); Lenovo, ThinkPad, Asus and Acer laptop keys. On
  purpose there is no driver for the laptop's own NVMe SSD, so ccos can't
  see or change Windows.
  Graphics chip: Settings > About > "Let apps draw with the graphics chip"
  (experimental, off at first). With it on, apps you start afterwards
  (Firefox, games) draw with the Intel graphics instead of the processor;
  ccos copies their finished pictures to the screen. If an app misbehaves,
  turn it off and restart the app.
  Apps working with ccos: when an app wants a file ("Upload", "Open..."),
  a file window appears with your Documents, Images, Downloads, Videos and
  USB drives on the left. Links from apps open in Firefox. Notifications
  from apps appear as ccos notifications.

Starting from the USB stick:
  Your files and settings are on the stick. If ccos can't find the stick
  right after starting (some laptops find USB sticks a few seconds late), it
  shows "CcOS needs to find your USB boot stick." Unplug the stick and plug
  it back in; when it says "USB boot stick found!", the desktop opens with
  your settings. "Skip" (or Esc) opens the desktop straight away; if the
  stick turns up later, your settings are loaded then.

Updates (Settings > Update):
  New ccos versions are published on github.com/justforcountryballs/CcOS
  (Releases). ccos looks once a day and shows a notification when there's
  one; "Install now" downloads it and puts it on the stick next to the
  version you have. Restart, and the new one starts. Your files, settings
  and apps stay where they are (on the stick's ccos-store part).
  If the new version doesn't start properly (crashes, or the power goes out
  during the first start), the stick goes back to the version before all by
  itself on the next start, and a notification tells you. The start menu of
  the stick (the first screen, GRUB) also has "ccos (previous version)".
  Only updates signed with the ccos release key are installed. An update
  file (.ccupd) can also be installed by double-clicking it in File Explorer.
  In QEMU, updates can't replace run.bat's files: the Update page links to the
  release so you can download the new ccos.zip instead.
  Sticks made before version 1.0 don't have the updater yet: write this
  version to them once with Rufus; from then on, updates come by themselves.

Shell commands: poweroff, reboot (show the shutdown screen)
Closing the QEMU window = pulling the plug (no shutdown screen)

Adding things to the start menu: edit left_items / right_items near the
top of source\desktop.c (ask Claude to rebuild it for you).

Needs QEMU: open PowerShell and run
  winget install SoftwareFreedomConservancy.QEMU

Running ccos on a real computer (USB stick, nothing gets installed):
  1. Get Rufus (rufus.ie) on Windows and plug in a USB stick (8 GB or more
     recommended: apps take room; everything on the stick will be erased).
  2. In Rufus: Device = your stick, Boot selection = ccos-usb.img.gz
     (Rufus reads it as it is, no need to unpack it). Start, and say yes
     to erasing the stick. Rufus writes it as a whole-disk image.
  3. Windows may then say the stick needs formatting: click Cancel. Never
     format it from Windows, or your ccos files and apps are gone.
  4. Restart the laptop and press F12 at the Lenovo logo (on some Lenovos:
     Fn+F12, or the small Novo button), then pick the USB stick.
  5. A menu appears for 3 seconds; "ccos" starts by itself.
     If the screen stays black, restart and pick "ccos (safe graphics)".

  The first start takes a few seconds longer: ccos turns the empty rest of
  the stick into its own part, "ccos-store". Your settings, Wi-Fi networks,
  files (D:), Downloads and installed apps are all kept there, so nothing is
  forgotten on restart. D: shows how much room is left. Writing the stick
  again with Rufus starts fresh (it erases ccos-store too).
  (An older stick made from ccos.iso still works: it keeps settings in
  ccos\ccos-data.img, but apps aren't kept. Write ccos-usb.img.gz to get both;
  its first start brings the old stick's settings along only if you copy
  ccos-data.img into the new stick's ccos folder first.) The laptop's own drive is never touched: the
  ccos kernel doesn't even have a driver for NVMe/SATA drives, so Windows'
  disk is invisible to it. Pull the stick out and Windows starts as usual.

  Touchpad: one finger moves the pointer, tap = click, two-finger tap =
  right click, two fingers up/down = scroll, pressing the pad = click
  (bottom-right corner = right click). Power button = shut down.
  Wi-Fi: Intel Wireless-AC 9560 (your Lenovo V15 G3) and other Intel cards.
