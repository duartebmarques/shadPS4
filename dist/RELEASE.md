## inFAMOUS Second Son and First Light on a Steam Deck

You need your own dumps of the games (folders or `.pkg` files) anywhere in your home folder or on
the SD card: **Second Son** (CUSA00004, update **1.07**) and/or **First Light** (CUSA00897).

1. **Desktop Mode:** Steam button > Power > Switch to Desktop.
2. **Get the app:** download `shadps4-@TAG@-linux.zip` below and extract it: one folder,
   `shadPS4-infamous`. Double-click `shadPS4.sh` in it: the first start finds your games and adds
   each one to Steam (its own shortcut, straight into the game), then the game picker opens.
3. **Play:** in Gaming Mode, Library > Non-Steam > your game.

First Light is a **work in progress**: playable, but heavy fights drop to about 20 fps.

Or the one line in Konsole (installs into `~/Applications/shadPS4-infamous` and adds each game to Steam):
```
bash <(curl -fsSL https://github.com/duartebmarques/shadPS4/releases/latest/download/install.sh)
```

Once, while a game runs: press **...** > Performance, turn on *Use per-game profile*, set *Manual
GPU clock* to **1600 MHz**, and turn the frame limit **off**. In the game, set the frame rate option
to **30fps limit**.

Already installed (any version)? Run the one-line install again, or `./shadPS4.sh --update` in the
app folder. Saves, controls and patch choices are kept.

Full guide and troubleshooting: [README](https://github.com/duartebmarques/shadPS4#readme).
