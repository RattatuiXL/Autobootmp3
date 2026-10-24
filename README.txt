AutoMusic - PSP kernel plugin for ARK-4.
Plays ms0:/MUSIC/*.mp3 automatically on the XMB.
Note button: short press = stop / resume, double press = next song.

When another app starts playing audio, the music fades out and pauses. It
automatically resumes with a fade in after the other audio ends. Short menu
sound effects are ignored.

While a track is active, a small XMB overlay shows a spinning disc, the track
name, elapsed and total time, and a playback progress bar.

The display-flip hook technique was informed by XMBuddy, an MIT-licensed PSP
XMB overlay by Stefan Tsonev: https://github.com/StefanTsonev/XMBuddy

download the AutoMusic artifact from the Actions tab -> AutoMusic.prx

Install: copy AutoMusic.prx to ms0:/SEPLUGINS/ and add this line to
ms0:/SEPLUGINS/PLUGINS.TXT :
  vsh, ms0:/seplugins/AutoMusic.prx, on

If anything goes wrong at boot, hold Select (or Start+Select) while
powering on to disable plugins, then delete the line.
