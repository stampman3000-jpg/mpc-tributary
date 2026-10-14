# Agents

For the next agent. Stampman3000 is a hobby builder. Explain in plain language, and keep a change small enough that he can hear it on the MPC. The README is the page for people. This file is the map and the rules. This repository is public.

## What this is

Tributary plays Elektron Overbridge audio, over USB, on the MPC track the plugin sits on. One copy sums the outputs whose switches are on. Several copies share one USB read. The Digitone II is the machine this has been heard on. The other Overbridge 2 machines are listed in `src/devices.h` and are untested.

Analog Rytm MKI, Analog Four MKI and Analog Keys are unsupported, same as Overwitch. They speak Overbridge 1, which is the older isochronous link. Tributary only speaks the interrupt one. Leave those three machines out of the device table.

## Where the code is

- `src/probe.c` is the engine. Loading does not claim USB. **ACTIVE** claims audio interfaces 1 and 2, switches both to alt setting 3, and leaves interface 4 (USB audio control) and interface 5 (MIDI) on their kernel drivers. One thread sends silent bundles out of endpoint `0x03` and reads endpoint `0x83`. `render` sums the switches that are on, from a spare of `HEAR_LAG` frames behind the newest sample. The six functions at the bottom (`create`, `destroy`, `midi`, `set_param`, `get_param`, `render`) are what the VST2 wrapper calls.
- `src/devices.h` is the device table. Product ids, frame sizes and output names are facts from [Overwitch](https://github.com/dagargo/overwitch) by David García Goñi (dagargo), also GPL-3. Digitone II (`0x0b34`) is the only map marked heard. Digitakt II uses that same map. The 24-bit sample decode in `src/probe.c` is adapted from Overwitch `engine.c` and already names Stefan Rehm and David García Goñi. Keep that credit. No Overwitch source is copied into this tree.
- `vst/` is the description the [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) toolchain reads: `vst.json`, `params.json`, `layout.conf`, and the artwork in `vst/art/`. The wrapper (`vst2_wrap.c`) stays in that checkout. It is not copied here. `vst/build/` is compiler output, including `params.h`, the skin, and `pluginlist-entry.xml`.
- `test/` is the host test. `test/host_test.c` builds a fake sysfs and usbfs and drives the engine the way the MPC does. `test/run.sh` compiles and runs it in a `gcc:12` container, because it needs Linux headers.
- `tools/` in this repo is `pack_release.py` and `plugin_list.awk`. `pack_release.py` packs one architecture into a zip under `dist/`. A catalogue release uses the scripts in the mpc-vst-plugins checkout instead. See Release.
- `tested.json` is the device record the catalogue shows. `screenshots/tributary.png` is the rendered screen.

## Build

Docker has to be running, and `MPC_VST` has to point at a checkout of sd88me/mpc-vst-plugins. The commands are in the README. Both architectures compile `src/probe.c` with the wrapper, `-pthread`, and `-lm`. `vst/build/params.h` has to exist before a raw gcc line. `"$MPC_VST/tools/build_port.sh" "$PWD/vst/vst.json"` writes it and draws the skin.

The build needs glibc 2.34, so the plugin is MPC OS 3.x only. MPC OS 2.x cannot load a binary that needs glibc 2.34. Leave the compiler images in the README as they are. Do not add an OS 2.x target.

Do not commit `vst/build/`, `dist/`, `*.so`, or any other binary. Those paths are already in `.gitignore`.

## Test

```
sh test/run.sh
```

It must print `ALL PASSED`. A real USB claim still needs an MPC. This test checks the decisions on a fake bus: which device is chosen, that interface 5 is left on its driver, the device table, and that two copies can sum different outputs from one read.

## Release

Catalogue releases go through the mpc-vst-plugins checkout. `tools/release.py` there writes `mpc-plugin.json` into the zip. `tools/catalog_check.py` is the check that zip has to pass:

```
"$MPC_VST/tools/release.py" --so vst/build/tributary.so \
    --skin "vst/build/skin/johnny - VST - Tributary" \
    --entry vst/build/pluginlist-entry.xml --version X.Y.Z \
    --about "Sums chosen Elektron Overbridge outputs onto one MPC track over USB." \
    --repo stampman3000-jpg/mpc-tributary --license GPL-3.0-only --id tributary \
    -o dist
"$MPC_VST/tools/catalog_check.py" dist/Tributary-X.Y.Z-mpc-armv7.zip --catalog
```

Repeat `release.py` for the AArch64 `.so` when that zip belongs on the same release. The catalogue reads `*-mpc-armv7.zip` unless the entry says otherwise. Attach the zips to a GitHub release of this repo. Keep the uid `54726962` and the file name `tributary.so` fixed across versions.

`tools/pack_release.py` in this repo is the hand packer the README describes. It does not write `mpc-plugin.json`. A release that should be listed uses `release.py` and `catalog_check.py`.

## Catalogue

The listing is `catalog/plugins/tributary.json` in sd88me/mpc-vst-plugins. It already names this repo, `GPL-3.0-only`, and the screenshot. Versions and checksums stay out of that file. The catalogue reads them from the GitHub release.

Catalogue pull requests go from the stampman3000-jpg fork to sd88me/mpc-vst-plugins. Push the branch on the fork and open the pull request against that upstream. Do not push the listing onto sd88me's repository yourself, and do not open that pull request from this repo.

## Rules

- The MPC must be the clock master. Set the Elektron machine to follow MIDI clock and press play on the MPC. Clock the other way shares the USB cable with the audio and wobbles. Leave interface 5's kernel driver in place, and do not set USB configuration 1. Either one recreates the Digitone sound card, so play and clock stop arriving.
- The build needs glibc 2.34, so this is MPC OS 3.x only.
- Do not commit `vst/build/` or binaries.
- A listed release uses `tools/release.py` and `tools/catalog_check.py` from mpc-vst-plugins, and the zip carries `mpc-plugin.json`.
- Catalogue pull requests go from the stampman3000-jpg fork to sd88me/mpc-vst-plugins.
- Keep the licence GPL-3.0-only. Keep the Overwitch credit to David García Goñi (dagargo). The sample decode keeps its Stefan Rehm and David García Goñi credit too.
- This is a public repository. Do not add a home address, a personal email, or private files. Git commits are `Stampman3000 <stampman3000@gmail.com>`.
- Do not rewrite history. No force-push, and no rebase or amend of commits that are already on the remote.
