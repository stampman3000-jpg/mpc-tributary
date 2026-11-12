# Capture

The second tab on Tributary. It records the sound already playing on this track, for a whole number of bars, and writes a wav you can load yourself. It does not drop the file onto a pad. The monitoring on the track keeps playing while it records and while it saves.

This is not on the MPC yet. The computer checks are below. The last section is what to listen for once a build is installed.

## The knobs

Q-links come in fours. First page:

1. **Bars** — 1, 2, 4, 8, or 16. It starts on 4.
2. **Arm** — turn it on. The line says **Armed**. Nothing is written yet. When the MPC is playing, the take starts on the next bar.
3. **Source** — **mix** records what you hear (the switches that are on). Any other choice records only that output. The switches you are listening to do not change.
4. **Folder** — where the wav goes. The line under the status is the path, in words.

Second page:

5. **Rearm** — after a take is saved, it arms again for the next bar.
6. **Cancel** — drops the take. Nothing is written. If the file is already being saved, Cancel leaves it.

The status line is one of:

- **Armed** — waiting for the next bar.
- **Recording 2/4** — bar 2 of a 4-bar take.
- **Saved** — the wav is in the folder.
- **Error: …** — the take was thrown away, and the line says why.
- **Cancelled**

## Where the file goes

| Folder knob | Path |
|---|---|
| plugin | `Tributary Captures` on the drive Tributary loaded from. On this MPC that is the SD card: `/media/EOS_DIGITAL/Tributary Captures` |
| internal | `/storage/Tributary Captures` |
| sd | `/media/EOS_DIGITAL/Tributary Captures` |
| usb | `Tributary Captures` on another stick under `/media`, if one is plugged in |

If that drive is not there, the path line says **not plugged in** and Arm will not start.

The name looks like `DN2_120bpm_4bars_20261112-1915.wav`. Machine, tempo, bars, then the time the file was written. A second take in the same minute gets `-2` on the end. It will not overwrite.

The machine names are short: Digitone II is `DN2`, Digitakt II is `DT2`, Syntakt is `ST`, and so on.

## How the timing works

The clock is the MPC's, once per audio block. Not the screen, and not the clock on the wall. The block says where the playhead is, the tempo, and whether it is playing. A fuller clock also has the time signature, the bar start, and the loop points. That is the same set the official MPC host reports. The Akai SDK describes it in the host contract, and its `host-sim` tool is how an SDK plugin is exercised on the computer. Tributary is not built with that SDK. The capture tests feed those same fields in directly.

The open-mpc wrapper used to build Tributary only forwards the playhead, the tempo, and playing, and only if the build has `HAS_SONG_POS`. Until a wrapper also forwards the meter, Capture counts **4/4**. A 3/4 project would be the wrong length. The computer test does check 3/4 when the meter is passed in.

The file starts **162 samples** after the bar line. That is the monitoring spare: 176 Digitone samples at 48 kHz, which is 162 samples at the MPC's 44.1 kHz, about 3.7 ms. The sound that belongs on the bar arrives that late. Starting the file then puts beat 1 at the start of the wav, in line with something you sequenced on the MPC. The spare itself is not changed. `HEAR_LAG` in `src/probe.c` is still 176.

The length is the bars, at the tempo the take started at, at 44.1 kHz. 4 bars at 120 is 352800 samples. 1 bar at 120 is 88200.

If the tempo changes during the take, or you hit stop, or the playhead jumps into the middle of a bar, the take is thrown away and the status says why. A jump back to the loop start is kept, so a take can cross the MPC loop. If the wrapper did not say where the loop is, a jump from one bar line back to an earlier bar line is treated as that loop. A jump that lands in the middle of a bar is thrown away.

The longest take is **16 bars at 60 BPM** in 4/4. That is 64 seconds, 2822400 samples, about 11 MB while it is held and about 16 MB on disk. Slower than that, or a longer meter that would pass the cap, is refused so Arm does not grab a huge block of the MPC's memory. The memory is taken when you press Arm, not inside the audio block. The wav is written by a side thread after the last sample.

The wav is **24-bit stereo, 44.1 kHz**. The live sound is 16-bit, so the extra 8 bits are empty. A float file would not be any more accurate, and 24-bit is what the MPC sampler loads without a fuss.

## What the computer already checked

`sh test/run.sh` prints `ALL PASSED` twice. The capture half also runs on a Mac with the `cc` line in the README. No Digitone is involved.

Checked on the computer:

- The take starts on the exact sample of the bar, including when that sample is in the middle of a block of 32, 128, or 256.
- The file is the exact number of samples for the bars and the tempo. 4/4 and 3/4.
- The real 162-sample wait, and a shorter 10-sample wait, both shift the start by that many samples.
- A loop that jumps back to its start keeps the samples in order. A bar-to-bar jump does too when the loop points were not passed in.
- A tempo change, a stop, a jump into the middle of a bar, and a jump to a different bar when the loop points say that is not the loop, each throw the take away and do not leave a file.
- Played back to back, the join is the next sample of the same wave.
- A second file does not overwrite the first.
- 16 bars at 60 is accepted. 16 bars at 59 is refused.
- Arming does not change the audio on the track.

Not checked on hardware. The official SDK `host-sim` was not pointed at Tributary. It loads an SDK-built plugin and plays it like the MPC host. It does not know about the Capture tab. The timing tests stand in for that clock.

## When you want it on the MPC

Not this pass. When you do:

1. Build against a current [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) that has `song_pos` in `wrapper/engine.h`. `vst.json` already sets `HAS_SONG_POS`. The October 2026 toolchain does not call `song_pos`, so a binary from that tree would never see the playhead and Capture would stay on Armed. Do not copy `vst2_wrap.c` into this repo.
2. The commands are the ones in the README. They compile `src/probe.c` and `src/capture.c`.
3. Copy `tributary.so` in with a temporary name, then `mv` it over the old file, in both the project folder and `VST2 Plugins/johnny - VST - Tributary`. Leave the previous `.so` beside it. Update the skin the same way as a normal Tributary install. The `plugin.json` that is already on the card can stay. Bump its version string to `1.2.0` if you want the browser to show the new one.
4. Take Tributary off the track and put it back, or restart when you were going to anyway. A new project does not rescan by itself.
5. The card is nearly full. One wav is small. A long session of takes is not.

The Akai SDK is the reference for what the host reports and for the folder layout. Tributary stays on the open-mpc wrapper.

## What to check on the MPC

- Arm, press play, and the status goes from Armed to Recording on the next bar, not when you touch the knob.
- The track you are hearing does not mute or change while it records or saves.
- Load the wav on a pad and play it against a kick you sequenced on beat 1. The start should sit with that kick, not late by a noticeable gap. The 3.7 ms wait is small. If it is obviously late or early, say so before anyone changes `HEAR_LAG`.
- Loop the wav. The join should not click.
- Change tempo while it is recording. The line should say the tempo changed, and no file should appear.
- Hit stop while it is recording. Same, with transport stopped.
- Let a take run across the MPC's loop. It should save, not say the transport moved.
- Rearm on, one take, and it should arm itself for the next bar.
