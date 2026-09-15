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
const PaSampleFormat SAMPLE_FORMAT = paFloat32;
const int RUN_SECONDS = 30;
const float PITCH_SHIFT_FACTOR = 2.0f;
const int FFT_FRAME_SIZE = 1024;
const int OVERSAMP = 32;
const int MAX_QUEUE_SIZE = 10;

std::queue<std::vector<float>> audioQueue;

std::mutex queueMutex;

std::condition_variable queueCV;

// Shared stop signal, atomic so it can be safely read/written by multiple threads
std::atomic<bool> running(true);

// Helper function to check if a PortAudio function has returned an error.
bool checkPaError(PaError err, const char* context)
{
    if (err != paNoError) {
        std::cerr << "[ERROR] " << context << ": " << Pa_GetErrorText(err) << "\n";
        return false;
    }
    return true;
}

// Reads audio blocks from the microphone and pushes them onto the shared queue.
void inputThread(PaStream* inputStream)
{
    std::cout << "[Input ] Thread started.\n";

    while (running)
    {
        // Create a block (every loop) to store the incoming audio samples.
        std::vector<float> block(FRAMES_PER_BUFFER);

        // Reads audio samples from microphone into buffer 
        // Read 512 samples from the microphone into block, blocks until full
        // Pa_ReadStream expects a raw float pointer so .data() is used to get the internal array
        // from the block vector and write values into the block vector.
        // err will store 0 if everything is successful.
        PaError err = Pa_ReadStream(inputStream, block.data(), FRAMES_PER_BUFFER);

        if (err != paNoError && err != paInputOverflowed)
        {
            std::cerr << "[Input ] Read error: "
                      << Pa_GetErrorText(err) << "\n";
            running = false;
            break;
        }

        {
            // Lock the queue so processThread cannot access it at the same time.
            std::lock_guard<std::mutex> lock(queueMutex);

            // Pushes the last block captured into audioQueue.
            if ((int)audioQueue.size() < MAX_QUEUE_SIZE)
            {
                audioQueue.push(block);
            } else {
                std::cout << "[Input ] Queue full, dropping block.\n";
            }
        } // Lock releases here.

        // Wakes up processThread so it knows there is a new block to process.
        queueCV.notify_one();
    }

    std::cout << "[Input ] Thread stopped.\n";
}

// Pops audio blocks from the queue, pitch shifts them, and writes to the speakers.
void processThread(PaStream* outputStream)
{
    std::cout << "[Process] Thread started.\n";

    while (running)
    {
        // Creates an empty block to be filled from the queue every iteration of the loop.
        std::vector<float> block;

        // Locks the audioQueue vector so the input thread cant modify it.
        {
            std::unique_lock<std::mutex> lock(queueMutex);

            // Sleep until inputThread pushes a block or running becomes false.
            // .wait temporarly unlocks the mutex allowing the input thread to push a block
            // to audioQueue and once that has happened the inputThreads .notify_one() wakes
            // the processThread back up and it automatically starts from this point and locks
            // the mutex.
            queueCV.wait(lock, []
            {
                return !audioQueue.empty() || !running;
            });

            // If woken up due to program exit and nothing left to process, exit.
            if (!running && audioQueue.empty())
                break;

            // Take the oldest block from the front of the queue.
            block = audioQueue.front();
            audioQueue.pop();
        } // Lock releases here.

        // Pitch shift the block in place (same buffer used for input and output).
        smbPitchShift(PITCH_SHIFT_FACTOR, FRAMES_PER_BUFFER, FFT_FRAME_SIZE, OVERSAMP, SAMPLE_RATE, block.data(), block.data());

        // Write the pitch shifted block to the speakers.
        // Pa_WriteStream blocks until the hardware is ready to accept the data.
        PaError err = Pa_WriteStream(outputStream, block.data(), FRAMES_PER_BUFFER);

        if (err != paNoError && err != paOutputUnderflowed)
        {
            std::cerr << "[Process] Write error: " << Pa_GetErrorText(err) << "\n";
            running = false;
            break;
        }
    }

    std::cout << "[Process] Thread stopped.\n";
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
        PaError err = Pa_Initialize();
        if (!checkPaError(err, "Pa_Initialize")) exit(1);

        // Configure microphone settings (device, channels, format, latency).
        PaStreamParameters inputParams;     // Declares PaStreamParameters struct.
        inputParams.device = Pa_GetDefaultInputDevice();
        inputParams.channelCount = NUM_CHANNELS;
        inputParams.sampleFormat = SAMPLE_FORMAT;
        inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultHighInputLatency;
        inputParams.hostApiSpecificStreamInfo = NULL;

        // Configure speaker settings (device, channels, format, latency).
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();
        outputParams.channelCount = NUM_CHANNELS;
        outputParams.sampleFormat = SAMPLE_FORMAT;
        outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultHighOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        // Attempts to create an input stream with the parameters defined in inputParams and stores it in inputStream.
        err = Pa_OpenStream(&inputStream, &inputParams, NULL, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, NULL, NULL);
        if (!checkPaError(err, "Pa_OpenStream (input)"))
        {
            Pa_Terminate();
            exit(1);
        }

        // Attempts to create an output stream with the parameters defined in outputParams and stores it in outputStream.
        err = Pa_OpenStream(&outputStream, NULL, &outputParams, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, NULL, NULL);
        if (!checkPaError(err, "Pa_OpenStream (output)"))
        {
            Pa_CloseStream(inputStream);
            Pa_Terminate();
            exit(1);
        }
    }

    /* Run Method */
    void run() {
        // Start the input stream, activating the microphone.
        PaError err = Pa_StartStream(inputStream);
        if (!checkPaError(err, "Pa_StartStream (input)"))
        {
            Pa_CloseStream(inputStream);
            Pa_CloseStream(outputStream);
            Pa_Terminate();
            exit(1);
        }

        // Start the output stream, activating the speaker.
        err = Pa_StartStream(outputStream);
        if (!checkPaError(err, "Pa_StartStream (output)"))
        {
            Pa_StopStream(inputStream);
            Pa_CloseStream(inputStream);
            Pa_CloseStream(outputStream);
            Pa_Terminate();
            exit(1);
        }

        std::cout << "Running for " << RUN_SECONDS << " seconds...\n";
        std::cout << "Pitch shift factor: " << PITCH_SHIFT_FACTOR
                  << "  (2.0 = octave up, 0.5 = octave down)\n";

        // Starts both threads
        // One to read from the microphone
        // One to shift the pitch and write to speaker
        std::thread t1(inputThread, inputStream);
        std::thread t2(processThread, outputStream);

        // Pauses the main for 30 seconds and allows threads to keep running in the background.
        Pa_Sleep(RUN_SECONDS * 1000);

        std::cout << "Shutting down...\n";

        // Sets stop signal to allow threads to exit their while loops.
        running = false;

        // Will wake up the process thread if sleeping so it can exit its loop.
        queueCV.notify_all();

        // Waits until threads are finished their last processes.
        t1.join();
        t2.join();
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


int main()
{
    std::cout << "=== ME313 Task 2.2 Pitch Shifter ===\n";

    Audio audio;
    audio.init();
    audio.run();

    return 0;
}