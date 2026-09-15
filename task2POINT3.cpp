#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <portaudio.h>

#include "smbPitchShift.h"


// Constants
const int FRAMES_PER_BUFFER = 512;
const int SAMPLE_RATE = 48000;
const int NUM_CHANNELS = 1;
const int MAX_QUEUE_SIZE = 10;

enum class Mode { PitchShift, Passthrough };    // Represents the two operating modes of the audio processor.

Mode currentMode = Mode::Passthrough;
float pitchFactor = 2.0f;
std::mutex stateMutex;

std::queue<std::vector<float>> audioQueue;
std::mutex queueMutex;
std::condition_variable queueCV;

// Shared stop signal, atomic so it can be safely read/written by multiple threads
std::atomic<bool> running(true);

// Helper function to check if a PortAudio function has returned an error.
bool checkError(PaError err, const char* context) {
    if (err != paNoError) {
        std::cerr << "[ERROR] " << context << ": " << Pa_GetErrorText(err) << "\n";
        return false;
    }
    return true;
}

// Reads audio blocks from the microphone and pushes them onto the shared queue.
void inputThread(PaStream* inputStream) {
    while (running)
    {
        // Create a block (every loop) to store the incoming audio samples.
        std::vector<float> block(FRAMES_PER_BUFFER);

        // Read 512 samples from the mic into block, blocking until full.
        // .data() gives Pa_ReadStream a raw float pointer to the vector's internal array.
        PaError err = Pa_ReadStream(inputStream, block.data(), FRAMES_PER_BUFFER);
        if (err != paNoError && err != paInputOverflowed) {
            std::cerr << "[Input] Read error: " << Pa_GetErrorText(err) << "\n";
            running = false;
            break;
        }

        {
            // Lock the queue so processThread cannot access it at the same time.
            std::lock_guard<std::mutex> lock(queueMutex);

            // Pushes the last block captured into audioQueue.
            if ((int)audioQueue.size() < MAX_QUEUE_SIZE)
                audioQueue.push(block);
        } // Lock releases here.

        // Wakes up processThread so it knows there is a new block to process.
        queueCV.notify_one();
    }
}

// Pops audio blocks from the queue, pitch shifts them, and writes to the speakers.
void processThread(PaStream* outputStream) {
    while (running)
    {
        // Creates an empty block to be filled from the queue every iteration of the loop.
        std::vector<float> block;

        {
            std::unique_lock<std::mutex> lock(queueMutex);

            // Sleep until inputThread pushes a block or running becomes false.
            // .wait temporarly unlocks the mutex allowing the input thread to push a block
            // to audioQueue and once that has happened the inputThreads .notify_one() wakes
            // the processThread back up and it automatically starts from this point and locks
            // the mutex.
            queueCV.wait(lock, [] { return !audioQueue.empty() || !running; });

            // If woken up due to program exit and nothing left to process, exit.
            if (!running && audioQueue.empty()) break;

            // Take the oldest block from the front of the queue.
            block = audioQueue.front();
            audioQueue.pop();
        } // Lock releases here.

        {
            // Lock stateMutex to safely read currentMode and pitchFactor,
            // preventing the keyboard thread from modifying them mid-read.
            std::lock_guard<std::mutex> lock(stateMutex);

            // Only pitch shift if the user has enabled pitch shift mode (press 's').
            // In passthrough mode this is skipped and the audio is left unmodified.
            if (currentMode == Mode::PitchShift)
                smbPitchShift(pitchFactor, FRAMES_PER_BUFFER, 1024, 32, SAMPLE_RATE, block.data(), block.data());
        } // Lock releases here.

        // Write the pitch shifted block to the speakers.
        // Pa_WriteStream blocks until the hardware is ready to accept the data.
        PaError err = Pa_WriteStream(outputStream, block.data(), FRAMES_PER_BUFFER);
        if (err != paNoError && err != paOutputUnderflowed) {
            std::cerr << "[Process] Write error: " << Pa_GetErrorText(err) << "\n";
            running = false;
            break;
        }
    }
}

// Adjusts settings based on keyboard inputs (needs you to press enter to confirm each press).
void keyboardThread() {
    std::cout << "\nControls:\n"
              << "  s: pitch shift mode\n"
              << "  p: passthrough mode\n"
              << "  u: pitch up (+0.5)\n"
              << "  d: pitch down (-0.5)\n"
              << "  q: quit\n\n";

    // Holds a single character.
    char key;

    // Block and wait for a single keypress from the user, loop until
    // the program is told to stop.
    while (running && std::cin >> key)
    {
        // Lock stateMutex before modifying currentMode or pitchFactor,
        // since processThread reads these variables concurrently.
        std::lock_guard<std::mutex> lock(stateMutex);

        if (key == 'q') {
            // Signal all threads to stop and wake up any waiting threads.
            std::cout << "[Keys] Quitting...\n";
            running = false;
            queueCV.notify_all();

        } else if (key == 's') {
            // Switch to pitch shift mode, audio will now be pitch shifted.
            currentMode = Mode::PitchShift;
            std::cout << "[Keys] Pitch shift mode  (factor: " << pitchFactor << ")\n";

        } else if (key == 'p') {
            // Switch to passthrough mode, audio will pass through unmodified.
            currentMode = Mode::Passthrough;
            std::cout << "[Keys] Passthrough mode\n";

        } else if (key == 'u') {
            // Pitch up is disabled in passthrough mode as per the spec.
            if (currentMode == Mode::Passthrough) {
                std::cout << "[Keys] Switch to pitch shift mode first (press s)\n";
            } else {
                // Increase pitch factor by 0.5.
                pitchFactor += 0.5f;
                std::cout << "[Keys] Pitch factor: " << pitchFactor << "\n";
            }

        } else if (key == 'd') {
            // Pitch down is disabled in passthrough mode as per the spec.
            if (currentMode == Mode::Passthrough) {
                std::cout << "[Keys] Switch to pitch shift mode first (press s)\n";
            } else {
                // Decrease pitch factor by 0.5, clamped to a minimum of 0.5
                // to avoid zero or negative pitch which would be invalid.
                pitchFactor = std::max(0.5f, pitchFactor - 0.5f);
                std::cout << "[Keys] Pitch factor: " << pitchFactor << "\n";
            }
        }
    } // Lock releases here.
}


class Audio {
  private:
    PaStream* inputStream;
    PaStream* outputStream;

  public:
    /* Constructor */
    Audio() : inputStream(nullptr), outputStream(nullptr) {}

    /* Initialisation Method */
    void init() {
        // Triggers a scan of available devices which can be used later.
        // If the result is not paNoError, then an error has occured.
        if (!checkError(Pa_Initialize(), "Pa_Initialize")) exit(1);

        // Configure microphone settings (device, channels, format, latency).
        PaStreamParameters inputParams, outputParams;

        inputParams.device = Pa_GetDefaultInputDevice();
        inputParams.channelCount = NUM_CHANNELS;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
        inputParams.hostApiSpecificStreamInfo = NULL;

        // Configure speaker settings (device, channels, format, latency).
        outputParams.device = Pa_GetDefaultOutputDevice();
        outputParams.channelCount = NUM_CHANNELS;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        // Attempts to create an input stream with the parameters defined in inputParams and stores it in inputStream.
        if (!checkError(Pa_OpenStream(&inputStream, &inputParams, NULL, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, NULL, NULL), "Open input stream")) { Pa_Terminate(); exit(1); }

        // Attempts to create an output stream with the parameters defined in outputParams and stores it in outputStream.
        if (!checkError(Pa_OpenStream(&outputStream, NULL, &outputParams, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, NULL, NULL), "Open output stream")) { Pa_CloseStream(inputStream); Pa_Terminate(); exit(1); }
    }

    /* Run Method */
    void run() {
        // Start the input stream, activating the microphone.
        if (!checkError(Pa_StartStream(inputStream), "Start input")) { Pa_Terminate(); exit(1); }

        // Start the output stream, activating the speaker.
        if (!checkError(Pa_StartStream(outputStream), "Start output")) { Pa_Terminate(); exit(1); }

        // Starts both threads
        // One to read from the microphone
        // One to shift the pitch and write to speaker
        std::thread t1(inputThread, inputStream);
        std::thread t2(processThread, outputStream);
        std::thread t3(keyboardThread);

        // Waits until threads are finished their last processes.
        t1.join();
        t2.join();
        t3.join();
    }

    /* Stop Method */
    void stop() {
        // Stops the streams from receiving/emmiting signals
        Pa_StopStream(inputStream);
        Pa_StopStream(outputStream);

        // Releases the stream resources allocated by Pa_OpenStream
        Pa_CloseStream(inputStream);
        Pa_CloseStream(outputStream);

        // Shuts down PortAudio
        Pa_Terminate();
    }

    /* Destructor */
    ~Audio() {
        stop();
    }
};


int main() {
    std::cout << "=== ME313 Task 2.3 Pitch Shifter with Keyboard Control ===\n";

    Audio audio;
    audio.init();
    audio.run();

    return 0;
}