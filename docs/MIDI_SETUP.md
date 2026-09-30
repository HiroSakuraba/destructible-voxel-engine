# MIDI setup (synth and editors)

The desktop editor (`dve_desktop_editor`, SDL) and the native X11 editor
(`dve_native_editor_x11`) share one MIDI setup (`include/dve/editor_midi.hpp`). A
`MidiInputSession` opens one input port for the synthesizer, and a worker thread re-checks the
port list about every 1.5 s:

- **Choose the port** in **Settings > Audio > MIDI > MIDI Input** (left/right or click
  cycles through the list), or with the **MIDI IN** button in the synth header (next to
  MIDI THRU; on narrow windows it moves to the title bar). The options are:
  - **Auto** (the default): the first hardware port. Auto skips *Midi Through*, other
    RtMidi clients (including the editor's own output), macOS IAC buses, loopMIDI and other
    virtual ports.
  - **None**: MIDI input is off.
  - **Any listed port**: you can still pick *Midi Through* here on purpose.
- **Saved by name.** The choice is saved in the User settings
  (`.dve/user/editor_settings.txt`), not by port number. The volatile ALSA
  `client:port` id (`... 24:0`) is ignored, so a replugged device is found again.
- **Status.** Settings shows the status next to the port (for example
  `Auto: MPK mini 3 (connected)`) and in the details line. The header button turns green
  when connected and amber when not (`MIDI IN: MPK mini 3 (unplugged)`).
- **Hotplug.** Unplugging closes the port and sends sustain-off, all-notes-off and
  pitch-bend-center on all 16 channels, so no note hangs. Plugging the device back in
  reconnects it automatically.
- **Channel filter.**
  - **MIDI Channel** is Omni (the default) or channel 1–16.
  - **Channel 10 (Drum Pads)** decides what happens to channel 10:
    - *Always play* (the default) lets channel 10 through whatever the channel filter is.
    - *Ignore* drops channel 10, even in Omni.
    - *Follow channel filter* treats channel 10 like any other channel.
- **Synth MIDI out.** MIDI thru and arpeggiator output still go to the first MIDI output
  port (on Linux that is usually *Midi Through*).

If RtMidi is missing, or the operating system has no MIDI service (for example, no
`/dev/snd/seq`), the editors still start. The status then reads `MIDI unavailable`, and the
on-screen piano and computer keyboard keep working.

## Installing RtMidi

RtMidi is optional (`DVE_ENABLE_RTMIDI=ON` by default). CMake uses an installed RtMidi 6 if it
finds one. Otherwise, `-DDVE_FETCH_RTMIDI=ON` builds the pinned RtMidi 6.0.0 from source.

| Platform | Install | MIDI API |
|----------|---------|----------|
| Debian / Ubuntu | `sudo apt install librtmidi-dev` | ALSA sequencer (JACK if RtMidi was built with it) |
| Fedora | `sudo dnf install rtmidi-devel` | ALSA |
| Arch | `sudo pacman -S rtmidi` | ALSA / JACK |
| macOS | `brew install rtmidi` | CoreMIDI |
| Windows | `vcpkg install rtmidi` (pass the vcpkg toolchain file), or `-DDVE_FETCH_RTMIDI=ON` | WinMM |

The configure log says `RtMidi found: native MIDI input/output enabled` when it is found, and
`RtMidi not found: deterministic virtual MIDI remains available` when it is not.

**Linux:**
- List ports with `aconnect -l` or `amidi -l` (package `alsa-utils`). The editor shows the
  same names.
- If no ports appear, check that `/dev/snd/seq` exists (`sudo modprobe snd-seq`), and that
  your user can open it (the `audio` group on some distributions).

**Windows:** only one program can open a WinMM input at a time. Close a DAW that holds the
keyboard, or route it through loopMIDI, and pick that port explicitly.

**macOS:** class-compliant keyboards appear without drivers. To use an IAC bus, enable it in
*Audio MIDI Setup* and select it explicitly (Auto skips IAC).

## Akai MPK mini tips

- **Keys** send on channel 1. **Pads** send notes on **channel 10** by default (bank A is
  notes 36–43 on the mk3).
  - To play the synth from the pads, keep **Channel 10 (Drum Pads)** on *Always play*.
  - To keep the pads for something else, set it to *Ignore*.
  - With **MIDI Channel** on *Channel 1*, only the keys and the pads (when set to *Always
    play*) reach the synth.
- **Joystick:** left/right is pitch bend, and up is the mod wheel (CC 1). The synth handles
  both, plus channel pressure and a sustain pedal (CC 64) on the pedal jack.
- **Knobs** (K1–K8; on the mk3 factory program they send CC 70–77) are mapped with **MIDI
  learn**:
  1. Open the synth **Presets** page.
  2. Pick a **MIDI learn slot** (1–8), turn **Mapping enabled** on, and set **Controller
     CC** to the knob's CC number.
  3. Choose the **Target macro** (1–4), and set its range with **Minimum**/**Maximum**
     (**Inverted** flips it).
  4. On the **Mod Matrix** page, use Macro 1–4 as a modulation source. Two knobs can drive
     the same macro.

  The mappings are saved with the preset. If you are not sure which CC a knob sends, check
  the Akai *MPK mini Program Editor* or `aseqdump -p "MPK mini 3"`.
- **Two devices:** the MPK mini 3 shows up as `MPK mini 3:MPK mini 3 MIDI 1`. If you also
  have another controller, select the MPK explicitly so Auto does not pick the other one
  first.
