AutoMusic - PSP kernel plugin for ARK-4.
Plays ms0:/MUSIC/*.mp3 automatically on the XMB.
Note button: short press = next song, hold = stop / resume.

Build: upload this folder to a new GitHub repo (also create
.github/workflows/build.yml by hand if it did not upload), then download
the AutoMusic artifact from the Actions tab -> AutoMusic.prx

Install: copy AutoMusic.prx to ms0:/SEPLUGINS/ and add this line to
ms0:/SEPLUGINS/PLUGINS.TXT :
  vsh, ms0:/seplugins/AutoMusic.prx, on

If anything goes wrong at boot, hold Select (or Start+Select) while
powering on to disable plugins, then delete the line.
