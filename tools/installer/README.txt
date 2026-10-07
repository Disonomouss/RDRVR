RDRVR - Red Dead Redemption in VR
Version: @VERSION@

Needs
- Red Dead Redemption (PC, 2024), Steam or Rockstar Games Launcher.
- RedHook v0.8 by K3rhos (https://www.nexusmods.com/reddeadredemption/mods/192), installed in the game folder, with
  [DirectXHook] Disabled=true in its RedHook.ini (RDRVR draws the frame itself; RedHook's DirectX hook gets in the way).
- A PC VR headset with its OpenXR runtime set as the active one (SteamVR, Meta Quest Link, Virtual Desktop, WMR, ...).

Install
- Install RedHook first (above).
- Unzip this folder and run Install.cmd. It finds the game through Steam (or asks for the folder). To name it:
    Install.cmd -GameDir "D:\SteamLibrary\steamapps\common\Red Dead Redemption"
- Quit the game first. Files it would replace are backed up to RDRVR_backup in the game folder. No game file is
  changed.

Uninstall
- Run RDRVR_Uninstall.cmd in the game folder. It removes the mod's files and logs and puts back what was backed up.
  RedHook stays (remove it as its own instructions say).
- Your in-headset settings stay in %LOCALAPPDATA%\RDRVR (delete that folder to remove them too).

What is installed in the game folder
- dinput8.dll, RDRVR.Gameplay.red, RDRVR.ini: the mod. RDRVR.ini holds the settings it starts with; what you change
  in the in-headset menu is saved in %LOCALAPPDATA%\RDRVR\RDRVR.user.ini and wins over it.
- RDRVR_click_*.wav: the empty gun's clicks.

Note: the game sends its crash reports to Rockstar. RDRVR is not affiliated with Rockstar Games or Take-Two.
Licence: MIT (LICENSE.txt); third-party notices in THIRD-PARTY-NOTICES.txt.
