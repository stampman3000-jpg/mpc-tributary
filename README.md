# mpc-overprobe

A throwaway probe for one question: can a plugin running on an Akai MPC open and claim the Overbridge audio interface of an Elektron Digitone II (plugged into the MPC's USB host port), while the Digitone's MIDI interface keeps working?

It makes **no audio**. The plugin outputs silence. It is a feasibility step for a later plugin that plays the Digitone II's audio on the track the plugin sits on. That track goes to the MPC main like any instrument. Stream on the same track copies it to the Mac. Nothing is recorded, and there is no second track.

## What it does

1. Lists every USB device the plugin can see, from sysfs. For each one it shows vendor and product ids, and each interface with its class, subclass and kernel driver.
2. Finds a Digitone II: `1935:0b34` (Overbridge), `1935:1034` (Audio/MIDI) or `1935:0134` (MIDI).
3. Opens its usbfs node and checks which kernel driver, if any, holds interface 1.
4. Claims interface 1 (audio in) and interface 2 (audio out). It does not claim interface 4 or 5.
5. Switches both to alt setting 3. It asks the device for its Overbridge name (two vendor reads), waits 100 ms, then sends silent blocks out of endpoint `0x03` (header `0x07ff`) while reading endpoint `0x83` for about a second. Finished transfers are collected directly, because `poll()` does not wake on this MPC when one completes. A good incoming packet is 1012 bytes, header `0x0700`, with the sample counter rising by 7. The log then shows a peak for main and for tracks 1 to 6. It then switches both interfaces back to alt setting 0 and releases them.
6. Reports each step with the errno name (EBUSY, EACCES, ENOENT and so on).
7. Checks that the MIDI interface (5) still has the same driver as before. It shows the result as `IF5 MIDI untouched` or `IF5 MIDI CHANGED!`.
8. Shows a short verdict on the plugin screen, and appends a full log to the MPC's drive.

It never detaches a kernel driver, never resets the device, never sets a configuration, and never touches interfaces 4 or 5. A plugin scan does not run it: it runs only when you press **RUN PROBE**.

## What is known about the Digitone II

From [Overwitch](https://github.com/dagargo/overwitch) (GPL-3) and its udev hwdb. Overwitch was read for ids and interface numbers only. No code is copied.

| Mode | USB id | In Overwitch |
|---|---|---|
| Overbridge | `1935:0b34` | Yes. Device table name "Digitone II", protocol version 2.1 |
| Audio/MIDI | `1935:1034` | Listed in the udev hwdb only |
| MIDI | `1935:0134` | Listed in the udev hwdb only |

Overwitch supports the Digitone II. Its README lists "Digitone II" among the supported Overbridge 2 devices.

Interfaces Overwitch uses on the 2.1 protocol:

- Interface 1: audio in, endpoint `0x83`, alt setting 3
- Interface 2: audio out, endpoint `0x03`, alt setting 3
- Interface 4: control
- Interface 5: MIDI

Overwitch detaches kernel drivers from 4 and 5 and claims 1 and 2. This probe claims only 1. The descriptors on the MPC are read at runtime, so the log shows whether the MPC sees the same layout.

## Build

You need Docker and a checkout of [sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins). The build uses that repo's `build_port.sh` toolchain, which makes a 32-bit ARM VST2 `.so` for the MPC.

```
MPC_VST=/path/to/mpc-vst-plugins "$MPC_VST/tools/build_port.sh" "$PWD/vst/vst.json"
```

Output goes to `vst/build/` (ignored by git):

- `vst/build/overprobe.so`: the plugin
- `vst/build/skin/johnny - VST - Overprobe/`: the screen
- `vst/build/pluginlist-entry.xml`: the registration line (see below for its `file=` path)

Host test of the decision logic, in a Linux container (it needs Linux USB headers, so it does not run on macOS):

```
sh test/run.sh
```

The test builds a fake USB tree and checks the verdicts. It cannot test the real claim, because only the MPC has a real Digitone II on usbfs.

## Install on the MPC

These steps assume the layout used for Stream and Chop: plugin files in the Synths folder on `/media/EOS_DIGITAL`, reached over SSH as root.

1. Copy the plugin into its own folder, using a temporary name and then renaming it. Never delete the folder.

   ```
   ssh root@<MPC> "mkdir -p '/media/EOS_DIGITAL/Synths/johnny - VST - Overprobe'"
   scp -O vst/build/overprobe.so "root@<MPC>:/media/EOS_DIGITAL/Synths/johnny - VST - Overprobe/overprobe.so.new"
   ssh root@<MPC> "cd '/media/EOS_DIGITAL/Synths/johnny - VST - Overprobe' && mv overprobe.so.new overprobe.so && md5sum overprobe.so"
   tar -C vst/build/skin -cf - "johnny - VST - Overprobe" | ssh root@<MPC> "tar -C /media/EOS_DIGITAL/Synths -xf -"
   ```

2. Register it once. MPC lists plugins in `pluginList-arm` inside `MPC.settings`, and a new plugin needs one MPC restart. Use the line from `vst/build/pluginlist-entry.xml`, but with the path that matches your drive. The build writes `/sdcard/vst/overprobe.so`, which is wrong for this setup:

   ```
   <PLUGIN name="Overprobe" descriptiveName="Overprobe" format="VST" category="Synth" manufacturer="johnny" version="1.0" file="/media/EOS_DIGITAL/Synths/johnny - VST - Overprobe/overprobe.so" uid="4f767072" isInstrument="1" fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>
   ```

   Back up `MPC.settings` first and stop the MPC software before editing it. The file location on the MPC One was not verified for this build, so check it on your unit first.

3. Run it. Plug the Digitone II into the MPC's USB host port before you start. Insert Overprobe on a track, open its screen, and press **RUN PROBE**. Use an empty track, because the plugin outputs silence.

4. Read the verdict on the screen. Then copy the full log off the MPC (or take the drive to the Mac):

   ```
   scp -O root@<MPC>:/media/EOS_DIGITAL/mpc-overprobe.log .
   ```

5. Check that MIDI still works: play the Digitone II's MIDI, or send notes from a MIDI track to it.

Each press appends a block to the log, starting with `=== mpc-overprobe run N`. If the drive is missing, the log goes to `/sdcard/mpc-overprobe.log`.

## Reading the verdict

| Verdict | Meaning |
|---|---|
| `AUDIO PACKETS OK` | Packets arrived for about a second: 1012 bytes, header `0x0700`, counter rising by 7, and at least one track had a non-zero level. The screen names the hottest track. The log lists main and tracks 1 to 6. |
| `STREAM SILENT` | The packets had the right shape for about a second, and main plus tracks 1 to 6 were still all zeros. |
| `NO PACKETS` | Alt 3 came up, but nothing arrived on `0x83` before the wait ran out. |
| `SHORT PACKETS` | Packets arrived, but not at 1012 bytes. The log has the lengths. |
| `BAD COUNTER` | The packet size was right, but the sample counter did not rise by 7. |
| `HDR NOT 0700` | Size and counter were right, but the header was not `0x0700`. The log shows the header. |
| `ALT3 OK, EP 0x83` | Interface 1 accepted alt setting 3 and endpoint `0x83` showed up, but the packet read did not run. |
| `ALT3 OK, NO EP 0x83` | Alt 3 was accepted and put back, but endpoint `0x83` did not appear. The log lists what did. |
| `ALT3 FAILED` | The switch to alt 3 failed. The CLAIM IF1 readout is the errno name. Alt 0 was still attempted. |
| `ALT0 FAILED` | Alt 3 worked, but the switch back to the idle setting failed. Check the log before using the Digitone again. |
| `RELEASE FAILED` | The interface was claimed but not released. Check the log. |
| `BUSY: IF1 HELD` | Something else owns interface 1 (EBUSY). The log's "kernel driver" line shows which one. |
| `CLAIM FAILED` | The claim failed for another reason. The CLAIM IF1 readout is the errno name. |
| `NO USBFS NODE` | The MPC gives plugins no usbfs device node. Then no libusb or raw approach works from a plugin. |
| `OPEN FAILED` | The device node exists but could not be opened (for example EACCES). |
| `IF1 IS MIDI, SKIPPED` | Interface 1 is USB MIDI on this unit, so the probe refused to claim it. |
| `D2 HAS NO IF1` | The Digitone II is there but has no interface 1 in this mode. |
| `NO DIGITONE II SEEN` | No Elektron device with a Digitone II id. Check the cable and the log's device list. |

## Limits and risks

- The claim-and-release test has run on an MPC One. The alt-setting test is the current step. The host test still only covers the decision logic, on a fake USB tree.
- The probe runs inside MPC's process, as the plugin's user. Its only writes are the log files.
- The screen text may refresh only after the next audio block. If it looks stale, press Play once, or read the log.
- Registering the plugin means editing `MPC.settings`. Keep a backup of that file.
