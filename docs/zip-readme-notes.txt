All credit for the Raw Input Mouse Fix goes to slopkhayzo. Their MIT licence is LICENSE-RawInputMouseFix.txt in
this folder; their page: https://github.com/slopkhayzo/DeadSpace3-RawInputMouseFix

With "Use the game's sensitivity slider" on (the default), the game's own Mouse Sensitivity slider (settings >
controls) sets the speed: at 50 the fix runs at the Sensitivity multiplier, 0.50 to start with. The Mods panel
(pause, "mods", Raw Input Mouse Fix) has its four settings, which apply at once. The rest is in
ds3_raw_mouse.ini, which it writes in this folder on its first start.

If you used slopkhayzo's own DS3RawMouse.asi with an ASI loader, remove that copy: only one of the two can run, and
whichever starts second stays off. To keep your old settings, copy your old ds3_raw_mouse.ini into this folder. A
file from the standalone fix has no UseGameSlider line, so the game's slider then sets the speed; switch "Use the
game's sensitivity slider" off in the Mods panel to aim exactly as before.

It writes DS3RawMouse.log in this folder, and its main lines also go to ds3_mods.log in the game folder. Attach
both to a bug report.
