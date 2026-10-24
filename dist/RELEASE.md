## inFAMOUS Second Son and First Light on a Steam Deck

You need your own dumps of the games (folders or `.pkg` files) anywhere in your home folder or on
the SD card: **Second Son** (CUSA00004, update **1.07**) and/or **First Light** (CUSA00897).

1. **Desktop Mode:** Steam button > Power > Switch to Desktop.
2. **Get the app:** download `shadps4-@TAG@-linux.zip` below and extract it: one folder,
   `shadPS4-infamous`. Double-click `shadPS4.sh` in it: the first start finds your games, then the
   game picker opens (choose a game > **Play**, **Patches** or **Add to Steam**).
3. **Steam:** on the game's page choose **Add to Steam**, then in Gaming Mode open
   Library > Non-Steam > your game.

First Light is a **work in progress**: playable, but heavy fights drop to about 20 fps.

Or the one line in Konsole (installs into `~/Applications/shadPS4-infamous`):
```
bash <(curl -fsSL https://github.com/duartebmarques/shadPS4/releases/latest/download/install.sh)
```

Once, while a game runs: press **...** > Performance, turn on *Use per-game profile*, set *Manual
GPU clock* to **1600 MHz**, and turn the frame limit **off**. In the game, set the frame rate option
to **30fps limit**.

**Important:** after every game start, turn *Manual GPU clock* off and on again. A Steam bug
([SteamOS#2586](https://github.com/ValveSoftware/SteamOS/issues/2586)) leaves the GPU unlocked
although the menu shows 1600 MHz, and the game can drop under 10 fps.

Already installed (any version)? Run the one-line install again, or `./shadPS4.sh --update` in the
app folder. Saves, controls and patch choices are kept.

Full guide and troubleshooting: [README](https://github.com/duartebmarques/shadPS4#readme).
