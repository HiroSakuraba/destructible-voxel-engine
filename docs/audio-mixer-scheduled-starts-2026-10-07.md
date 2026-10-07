# Mixer: sounds scheduled to start inside a render chunk

Found in the Clockwork timing review (main `942dba9`); this change is on main `0c0f977`.

## The bug

`AudioMixer` renders in chunks of up to 1,024 frames. At the start of each chunk it decided
which sample voices are mixed (physical) and which only keep time (virtual), and a voice
whose start frame had not been reached yet was left out of that decision. The render loop,
though, processed every voice whose start frame fell inside the chunk: from the start frame
on it advanced the voice's read position, but because the voice was not physical it mixed
nothing. So a sound scheduled to start inside a chunk lost its first samples, up to a whole
chunk (about 21 ms at 48 kHz), and a sound shorter than the rest of the chunk was never
heard. Any `PlaySampleDesc::sampleFrame` that is not at a chunk boundary was affected,
including delayed actions from compiled audio event graphs.

## The fix

The chunk's voice loop now runs in spans split at every frame where a scheduled voice
starts. Voices are classified at the start of each span, so a voice is picked (and can take
a physical slot from lower-priority voices) from its own start frame. Split points are
collected into a fixed array (at most one per logical voice), so the audio callback still
does no allocation. Each voice is spatialized once per chunk however many spans the chunk
has, as before. Chunks with no scheduled starts run exactly as before.

## Validation

New `dve_audio_mixer_start_tests`:

- A 64-frame click scheduled at offsets 0, 1, 256, 900, 1,023, 1,024, 1,324 and 3,048 frames
  after a chunk boundary first sounds exactly that many frames after the offset-0 click, and
  its energy over 256 frames from the onset matches the offset-0 click within 0.1%. With the
  old mixer, the click at offset 1 was never heard.
- With 60 silent looping background voices holding all 48 physical slots, a critical click
  scheduled mid-chunk takes a slot at its start frame and is heard in full.

Full build (Release, SDL3 unavailable): 258 of 259 CTest tests passed. `dve_cpack_test`
fails because `dve_desktop_editor` is not built without SDL3, as on main here.
