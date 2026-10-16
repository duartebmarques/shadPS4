## Install on a Steam Deck (3 steps)

You need your own copy of **inFAMOUS Second Son (CUSA00004)** dumped from your PS4, with update
**1.07**, as folders or `.pkg` files, copied anywhere in your home folder or on the SD card.

1. **Desktop Mode:** press the Steam button > Power > Switch to Desktop.
2. **Run the installer:** open **Konsole** (app menu > System), paste this line
   (right-click > Paste) and press Enter. Answer the questions with Enter.
   ```
   bash <(curl -fsSL https://github.com/duartebmarques/shadPS4/releases/latest/download/install.sh)
   ```
3. **Play:** double-click "Return to Gaming Mode" on the desktop, then Library > Non-Steam >
   **inFAMOUS Second Son**.

Once, while the game runs: press **...** > Performance, turn on *Use per-game profile*, set
*Manual GPU clock* to **1600 MHz**, and turn the frame limit **off** (the game is capped at 30 fps).

The first start builds shaders, so expect stutter for the first minutes. Shake the Deck to shake
the controller (graffiti); the trackpads act as the PS4 touchpad.

Already installed? Run `~/Applications/shadPS4-infamous/install.sh --update`. Your saves,
controls and patch choices are kept.

Full guide and troubleshooting: [README](https://github.com/duartebmarques/shadPS4#readme).
Without internet: download `shadps4-@TAG@-linux.zip` below, extract it and double-click `install.sh`.
