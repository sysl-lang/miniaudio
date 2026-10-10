# miniaudio

Audio playback and capture for sysl, bound to [miniaudio](https://miniaud.io) 0.11.25 — module
`sh.sysl.miniaudio`.

```hocon
dependencies {
  miniaudio { git = "github.com/sysl-lang/miniaudio", version = "0.1.1" }
}
```

```sysl
import sh.sysl.miniaudio.{context, stream, Frames, F32}
import sysl.sync.Atomic

struct Clock
    frame: Atomic[u64]

play()
    val ctx = context().unwrap()
    val clock: &sync Clock = Clock(Atomic(0))

    val d = ctx.playback(stream(F32, 2), 48000, (f: Frames) ->
        val out = f.output_f32()                 // f.count frames, two samples each, interleaved
        val start = clock.frame.add(u64(f.count))
        ...).unwrap()

    d.start().unwrap()
```

Three types carry it. A **`Context`** is a backend — CoreAudio, PulseAudio, the null backend — and
the devices it can see. A **`Device`** is one open stream, or a pair for duplex, in the format,
channel count and sample rate the program asked for; miniaudio converts between that and whatever
the hardware runs at, and `playback_hardware()` / `capture_hardware()` say what it actually opened.
**`Frames`** is what the data callback is handed each period: the output buffer to fill, the input
buffer to read, and how many frames each holds.

## The callback runs on another thread, and the compiler checks what it captured

miniaudio calls the data callback from its own audio thread, so the callback is a
`&sync Fn(Frames) -> unit`. A closure that captures an ordinary `&T` is refused where it is written:

```
error: a closure shared between two domains may be called from either, so every count it captures
has to be atomic — but the 'cell' it captures reaches a '&Cell', whose count is not. Hold it as a
'&sync Cell'
```

What a callback shares with the rest of the program goes in a `&sync` box, and what it mutates there
is an `Atomic`. `Frames` and the slices it hands out are only valid until the callback returns.

## Lifetime

- `context(...)` and `ctx.playback/capture/duplex(...)` return `&Context` and `&Device`; miniaudio's
  state behind each comes from miniaudio's allocator and is released by a destructor.
- **A device plays for as long as something holds it.** Dropping the last `&Device` stops it and joins
  the audio thread before its storage goes; `close()` does the same early.
- A device holds its context, so a program may keep only the device.
- A device is opened stopped; `start()` begins the callbacks and `stop()` returns once the callback is
  no longer running.

## What is bound

| | |
|---|---|
| `context(backends = [])` | a context over a list of backends tried in order, or the platform's default order |
| `Context.backend`, `playback_devices`, `capture_devices` | the backend settled on; enumeration as `DeviceInfo { name, is_default }` with the backend's id kept opaque |
| `Context.playback`, `capture`, `duplex` | a device over a `stream(format, channels, device)` per side, a sample rate and an optional `period_frames` |
| `Device.start`, `stop`, `close`, `is_started`, `state` | |
| `Device.sample_rate`, `playback_format/channels`, `capture_format/channels` | the shape the callback sees — `Unknown` / `0` in the request resolve to the device's own |
| `Device.playback_hardware`, `capture_hardware` | what the backend opened: device name, format, channels, rate, period size and count |
| `Device.volume`, `set_volume` | miniaudio's master gain |
| `Frames.output_f32/s16/s32/u8/bytes`, `input_…` | the period's buffers, typed; a view of the wrong format panics, and `S24` is reached through `…_bytes` |
| `Format`, `Backend`, `State`, `Error` | with `Display`; `is_enabled`, `backend_name`, `bytes_per_sample`, `version` |

## What is deliberately absent

- **Decoders, encoders, waveform and noise generators, the node graph, the engine and the resource
  manager.** Each is a large subsystem of its own — the engine alone is a mixer, a spatializer and a
  sound-file cache — and each would be its own package's worth of design. They are compiled *out*
  (`MA_NO_DECODING` and five more in `package.hocon`'s `defines`), so a consumer does not link them.
- **Loopback devices.** Only WASAPI implements them, so there is nothing on the other platforms to
  test the binding against.
- **The notification callback** (started/stopped/rerouted/interrupted). It is a second
  cross-thread callback with a payload struct; nothing here needed it yet.
- **Per-backend config** (`wasapi.*`, `alsa.*`, `pulse.*`, `coreaudio.*`, `opensl.*`, `aaudio.*`),
  share mode, channel maps, channel-mix mode, resampler choice, `periods`, `performanceProfile`,
  `noClip`/`noPreSilencedOutputBuffer`/`noFixedSizedCallback`. Each is one `offsetof` write away
  (see below) and waits for a program that wants it.
- **`ma_context_get_device_info`'s native-format list.** Enumeration only guarantees name, id and
  default-ness; the full query is a separate, slower call.
- **Custom backends, logging, and the job/thread utilities.**

## The survey

Done before any of the binding was written. Every element of the surface bound, bucketed:

**Directly expressible**

- Every function bound: `ma_context_init/uninit`, `ma_context_get_devices`, `ma_device_init/uninit/
  start/stop/is_started/get_state`, the volume pair, `ma_device_id_equal`, `ma_version_string`,
  `ma_result_description`, `ma_get_backend_name`, `ma_is_backend_enabled`, `ma_get_bytes_per_sample`,
  `ma_calloc`/`ma_free`. Scalars, pointers and out-params only.
- The enums (`ma_format`, `ma_backend`, `ma_device_type`, `ma_device_state`, `ma_result`) through
  `c const` — duplex is `playback | capture`, the null backend is last by design, and none of it is
  transcribed.
- **`ma_context`, `ma_device` and `ma_device_config` as opaque structs** whose sizes, alignments and
  the fields the binding touches come from `c const "sizeof(…)"` / `"offsetof(…)"`. The config holds a
  union per backend and a resampler config with a vtable; laying it out in sysl would restate several
  hundred bytes of platform-dependent layout to write a dozen fields. A field is read or written at
  `usize(base) + offset`.
- **The data callback** — `void (*)(ma_device*, void*, const void*, ma_uint32)` takes no aggregate
  by value, so `&on_data` is an ordinary function address, and it finds its way back through the
  config's `pUserData`, read out of `ma_device` at its `offsetof`.
- `ma_device_info.name` is a NUL-terminated `char[256]` inside the struct, so `from_cstring` is
  correct for it (it is not a sysl string handed *to* C).

**Shim-able**

- `ma_device_config_init` returns `ma_device_config` **by value**, and the binding never declares that
  struct, so there is no sysl type for the return. `shim.c` is one line copying it into caller
  storage. Zeroing and setting defaults by hand would duplicate miniaudio's choice of default
  resampler.

**Needs the language to move — nothing.** Two questions that looked like they might:

- *`ma_device` is not relocatable* (it holds mutexes, condition variables and a thread handle the
  audio thread uses through the device's address). Inline `[N]u64` storage in a `&Device` box would
  be fine for the callback but **not for a destructor**: `Drop.drop(self)` receives a copy, and
  `ma_device_uninit` on a copy signals a copied condition variable the audio thread is not waiting on.
  So the C state lives in `ma_calloc`'d storage and the sysl struct holds the pointer — no shim, and a
  `Drop` that works. The same holds for `ma_context`.
- *A closure on the audio thread* — `&sync Fn` plus `@crossing(cb)` on the opening methods; the
  compiler's capture check is the one quoted above.

## Where it runs

**miniaudio is vendored because it `dlopen`s its backends at run time** and falls through its list —
PipeWire, PulseAudio or ALSA on Linux, CoreAudio on macOS, WASAPI on Windows, AAudio or OpenSL ES on
Android — so one build plays sound on a machine with no audio library installed, including a phone,
where no package manager could have supplied it. Homebrew carries `miniaudio` and Debian has it from
trixie, but a `pkg_config` requirement would add a link line those platforms are better off without.

**It does not run on a board.** The vendored C needs threads and a clock, and both checks fail:

```
$ clang -c --target=thumbv7m-none-eabi -O2 -I. miniaudio.c -o /dev/null
./miniaudio.h:3907:22: fatal error: 'pthread.h' file not found

$ arm-none-eabi-gcc -c -mcpu=cortex-m33 -O2 -I. miniaudio.c -o /dev/null
miniaudio.h:12013:9: error: implicit declaration of function 'nanosleep'
miniaudio.h:17585:9: error: implicit declaration of function 'pthread_attr_init'
```

`requires { posix = true }` says so up front.

## Tests

`sysl test .` — 29 tests over both layers, headless. Every device in the suite is opened on
miniaudio's **null backend**, which runs a real audio thread paced by the clock at the requested rate:
callbacks really fire on another thread, frames really advance, and no sound card is needed. A
callback records what it saw into `&sync` atomics and the test thread asserts the counts afterwards.

- The raw layer's sizes, alignments and offsets are asserted against what a C program compiled with
  the same `defines` printed.
- Shape tests assert the period (300 and 100 frames — deliberately not the backend's own 10 ms
  choice), the buffer lengths per channel, the formats each side reports, and that output arrives
  silent. A rate test opens at 8000 Hz and counts frames over 250 ms, so a rate that failed to arrive
  (the default is 48000) is six times out.
- Lifetime: stop halts and restart resumes; `close` leaves the device uninitialized and refusing to
  start; dropping a started device stops it; a device keeps its context alive.
- An explicit device is checked through miniaudio's own `playback.pID`/`capture.pID`, because the null
  backend has one device each way and a name comparison cannot tell.

**AddressSanitizer and ThreadSanitizer** — both green over the suite and over a 0.3 s sine played
through CoreAudio, with the instrumentation confirmed rather than assumed: `nm -u` of the ASan binary
shows `___asan_memcpy`, `___asan_report_load*` (41 asan symbols) and of the TSan one 10
`__tsan_read`/`__tsan_write` imports, and `otool -tV` of `_ma_device_init` shows calls into both
runtimes — **the vendored C is instrumented too**, `SYSL_EXTRA_CFLAGS` reaching every clang the build
drives. The system's CoreAudio framework is not.

**The mutation pass** broke each parameter on purpose. Eleven of fourteen mutations went red on the
first pass. Two of the three survivors were fixtures that could not disagree, and were fixed: the
period asked for was the backend's own default (480 at 48 kHz, 160 at 16 kHz), and an explicit device
id was invisible because the null backend has only one device. Both — and the capture-side id — go red
now. **One survivor stands:** a destructor that frees the device
without `ma_device_uninit` still passes the drop test, even under ASan — freeing the running device's
storage also stops its callbacks, so "the callbacks stopped" cannot tell the two apart. Nothing
observable short of binding the notification callback separates them.

A one-off hardware check on a MacBook Pro: CoreAudio, "MacBook Pro Speakers", f32 2ch 48000 Hz,
period 480 × 3, and 14,400 frames consumed in 0.3 s.

## How it is put together

```
sh/sysl/miniaudio/
  miniaudio.sysl   Error, Format, Backend, the field helpers
  context.sysl     Context, DeviceInfo, Stream
  device.sysl      Device, Frames, State, Hardware, the data trampoline
  tests.sysl
  c/
    miniaudio.c    upstream 0.11.25, unmodified
    miniaudio.h    upstream 0.11.25, unmodified
    shim.c, .h     ma_device_config_init into caller storage
    c.sysl         the declarations, sizes, offsets and numbers
    tests.sysl     that sysl and clang agree about miniaudio
```

## Upstream

miniaudio **0.11.25**, from `https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.tar.gz`:

| | SHA-256 |
|---|---|
| the release tarball | `b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274` |
| `miniaudio.h` | `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89` |
| `miniaudio.c` | `ab1984bb9804ffd7b0303813595d0b345a8a86c34da1daffc353a14b34102a65` |

Both files are byte-identical to the tarball's; the build options live in `package.hocon`, not in the
vendored source.

## Licence

miniaudio is public domain (Unlicense) or MIT No Attribution, at your choice, © David Reid. The
binding is ISC. `LICENSE` carries both.
