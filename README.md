# Real-Time Audio Pitch Shifter

A multithreaded C++ application that captures live microphone input, pitch-shifts it in real time using a Short-Time Fourier Transform, and plays it back — with live keyboard control over mode and pitch factor.


## Credit

The core pitch-shifting algorithm (`smbPitchShift.cpp` / `.h`) is **third-party code by Stephan M. Bernsee**, provided under the Wide Open License — it is not my work. Everything else in this repo (the `task2POINT*.cpp` files: audio I/O, threading, synchronization, and keyboard control) is my own implementation, built on top of that algorithm as the assignment specified.

## Overview

Four progressively more complex programs, each building on the last:

### Task 2.1a — Single-Threaded Passthrough
Reads audio blocks from the microphone via PortAudio (`Pa_ReadStream`, blocking I/O — no callback API) and writes them straight to the output device, unmodified. Establishes the basic PortAudio stream lifecycle wrapped in an `Audio` class (`init()` / `run()` / `stop()`).

### Task 2.1b — Single-Threaded Pitch Shifting
Same structure as 2.1a, but each block is passed through `smbPitchShift()` before being written to output. Runs entirely on one thread — read, shift, write, repeat — so it's simple but not free-running (write blocks the next read).

### Task 2.2 — Multithreaded Producer/Consumer
Splits reading and processing into two concurrent threads:
- **`inputThread`** reads blocks from the microphone and pushes them onto a shared `std::queue<std::vector<float>>`
- **`processThread`** pops blocks, pitch-shifts them, and writes them to the output stream

Synchronization: a `std::mutex` guards the queue, a `std::condition_variable` lets `processThread` sleep until a block is available instead of busy-waiting, and a bounded queue size (`MAX_QUEUE_SIZE`) prevents unbounded memory growth if processing falls behind. A shared `std::atomic<bool> running` flag allows clean shutdown from either thread.

### Task 2.3 — Adds Live Keyboard Control
Adds a third thread (`keyboardThread`) that reads single-character commands and updates shared state (`currentMode`, `pitchFactor`) under a separate `stateMutex`, decoupled from the audio queue's mutex so keyboard input never blocks the audio pipeline:

| Key | Action |
|---|---|
| `s` | Switch to pitch-shift mode |
| `p` | Switch to passthrough mode |
| `u` | Increase pitch factor by 0.5 (disabled in passthrough) |
| `d` | Decrease pitch factor by 0.5, clamped to a minimum of 0.5 (disabled in passthrough) |
| `q` | Quit — signals all threads to stop and wakes any waiting threads |

`processThread` checks `currentMode` under the same `stateMutex` before deciding whether to pitch-shift a block or pass it through unmodified.

## Design Notes

- Bounded audio queue with a drop-on-full policy in the input thread avoids unbounded latency buildup if the processing thread can't keep up.
- Two separate mutexes (`queueMutex` for the audio queue, `stateMutex` for mode/pitch state) rather than one shared lock, so keyboard-driven state changes don't contend with the audio hot path.
- Passthrough mode disables pitch up/down keys rather than silently ignoring them, giving explicit feedback via console output.
- `Pa_ReadStream`/`Pa_WriteStream` overflow/underflow errors (`paInputOverflowed`, `paOutputUnderflowed`) are tolerated rather than treated as fatal, since they're expected under normal real-time jitter.

## Building and Running

Requires a C++ compiler (G++/MinGW/Clang) and PortAudio installed and linked. Example compile command:

```
g++ -o pitch_shifter task2POINT3.cpp smbPitchShift.cpp -lportaudio -pthread
```

Sample rate should match your audio device's configured rate (commonly 44.1kHz or 48kHz).