# Tributary

Plays one output from an Elektron Digitone II, over USB, on the MPC track the plugin sits on. That track goes to the MPC main like any instrument. Put Stream on the same track if you want a copy on the Mac. Nothing is recorded inside the plugin, and it does not need a second track.

Several copies can sit on several tracks. They share one USB read. Each copy plays one stereo pair, or one mono channel sent to both sides.

## What you need

- A Digitone II on OS 1.10A or newer, set to Overbridge, plugged into the MPC's USB host port.
- The MPC as the clock. Set the Digitone to follow MIDI clock, and press play on the MPC.

The Digitone can be the clock instead. Play and tempo do reach the MPC, and they wobble. Its clock shares the USB cable with the audio, so the ticks arrive in bunches and the MPC keeps catching up. The screen says **MPC is the clock** along the bottom for this reason. The title of the plugin is Tributary.

## On the screen

The light is the session:

- **Red** means no Digitone, or the stream is off.
- **Amber** means it is connecting.
- **Green** means audio is coming in.

**SOURCE** picks which output this copy plays. On a Digitone II that is main, tracks 1 to 16, delay, reverb, chorus, and the inputs.

**ACTIVE** turns this copy on or off. The first copy to turn on opens the USB stream. Another copy just joins that stream. Turning **ACTIVE** off on the copy that opened it lets the Digitone go. Turning it off on another copy mutes that copy only.

Take the plugin off the track when you are finished. The last copy releases the audio interfaces. The Digitone's MIDI interface is left on its driver the whole time, and loading Tributary wires the sequencer link between the Digitone and the MPC.

## How to record

The sound is live USB audio, so a normal bounce does not capture it. Resample the track, or a submix, with the MPC sampler while it plays.

## Other Elektron machines

The SOURCE list comes from the device that is plugged in. Layouts for Digitakt, Digitakt II, Digitone, Digitone Keys, Syntakt, Analog Rytm MKII, Analog Four MKII, and Analog Heat (including MKII and +FX) are included from Overwitch's device list. Only the Digitone II has been heard with this plugin. The others are untested. Testers are welcome.

Analog Keys is not in this build. It uses the older Overbridge link, and Tributary only speaks the interrupt one used by the machines above.

## Install

These steps assume the same layout as Stream: plugin files in the Synths folder on `/media/EOS_DIGITAL`, reached over SSH as root.

1. Copy the plugin into its own folder. Use a temporary name, then rename the file. Do not delete the folder.

   ```
   ssh root@<MPC> "mkdir -p '/media/EOS_DIGITAL/Synths/johnny - VST - Tributary'"
   scp -O vst/build/tributary.so "root@<MPC>:/media/EOS_DIGITAL/Synths/johnny - VST - Tributary/tributary.so.new"
   ssh root@<MPC> "cd '/media/EOS_DIGITAL/Synths/johnny - VST - Tributary' && mv tributary.so.new tributary.so && md5sum tributary.so"
   tar -C vst/build/skin -cf - "johnny - VST - Tributary" | ssh root@<MPC> "tar -C /media/EOS_DIGITAL/Synths -xf -"
   ```

2. Register it once, if the MPC does not already list it. Plugins are named in `pluginList-arm` inside `MPC.settings`. Back that file up first. The line looks like this, with the path on your drive:

   ```
   <PLUGIN name="Tributary" descriptiveName="Tributary" format="VST" category="Synth" manufacturer="johnny" version="1.0" file="/media/EOS_DIGITAL/Synths/johnny - VST - Tributary/tributary.so" uid="54726962" isInstrument="1" fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>
   ```

   A new line needs one MPC restart. Take any older Overprobe off the track, then insert Tributary.

3. Plug the Digitone in before you start. Insert Tributary on a track, turn **ACTIVE** on, and pick a source. Use the MPC as the clock.

A log is appended to `/media/EOS_DIGITAL/tributary.log` (or `/sdcard/tributary.log` if that drive is missing).

## Build

You need Docker and a checkout of [sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins).

```
"$MPC_VST/tools/build_port.sh" "$PWD/vst/vst.json"
```

Output goes to `vst/build/` (not part of the release): `tributary.so` and `skin/johnny - VST - Tributary/`.

The host test checks the USB decisions and the device table on a fake bus. It needs Linux headers, so it runs in a container:

```
sh test/run.sh
```

## Limits

- One stereo pair, or one mono channel, per copy of the plugin.
- The audio can sit a little late against the MPC grid. The Digitone runs at 48 kHz and the MPC track at 44.1 kHz, and Tributary plays from a few milliseconds behind the newest USB sample so a late packet does not become a gap. The plugin cannot shift the MPC's other tracks to match. USB or DIN, and which machine is the clock, do not remove that wait. The spare is `HEAR_LAG` in `src/probe.c` if a later change wants to try a shorter one.
- One USB read is shared. Only the sources that are actually active are decoded.
- Other Overbridge machines are listed and untested.
- The Digitone clock over this same cable wobbles. Use the MPC as the clock.
- The screen can lag by one audio block. The log is the record of what the USB session did.

## Licence

This program is free software under the GNU General Public License, version 3. See `LICENSE`.

Device ids and the channel maps are facts from [Overwitch](https://github.com/dagargo/overwitch) by David García Goñi (dagargo), also GPL-3. No Overwitch source is copied into this tree.
