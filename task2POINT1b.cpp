#include <portaudio.h>
#include <vector>
#include <stdio.h>
#include <math.h>

#include "smbPitchShift.h"

using namespace std;

const int SAMPLING_FREQ = 24000;  //airpod microphone sampling rate 
const int BUFFER_SIZE = 512;
const int INPUT_CHANNEL_NO = 1;
const int OUTPUT_CHANNEL_NO = 1;
const PaSampleFormat SAMPLE_FORMAT = paFloat32;
const float PITCH_SHIFTER_FACTOR  = 2.0f;


//-----Check for errors when using PA functions-----//
static void checkErr(PaError err) {
    if (err != paNoError) {
    printf("PortAudio error: %s\n", Pa_GetErrorText(err));
    exit(EXIT_FAILURE);
    }
}


class Audio {
  private:
    PaStream* stream;
    bool running;  
    vector<float> buffer;


  public:
    /* Constructor */
    Audio() : stream(nullptr), buffer(BUFFER_SIZE), running(false) {}
    
    /* Initialisation Method */
    void init() {
        checkErr(Pa_Initialize());
        checkErr(Pa_OpenDefaultStream(&stream, INPUT_CHANNEL_NO, OUTPUT_CHANNEL_NO, paFloat32, SAMPLING_FREQ, BUFFER_SIZE, NULL, NULL));
    }

    /* Run Method */
    void run() {
        checkErr(Pa_StartStream(stream));  // start stream and check for error
        running = true;  //set flag to true

        while(running) {
            checkErr(Pa_ReadStream(stream, buffer.data(), BUFFER_SIZE));  // block and read data from mic
            pitchShift();  // pitch shift mic audio
            checkErr(Pa_WriteStream(stream, buffer.data(), BUFFER_SIZE));  // send pitch shifted data to speaker
        }
    }

    /* Stop Method */  // Method not used or task2.1 as run method contains superloop and program is single threaded
    void stop() {
        running = false;
        checkErr(Pa_StopStream(stream));
        checkErr(Pa_CloseStream(stream));
        Pa_Terminate();
    }
    
    /* Pitch Shift Method */
    void pitchShift() {
        smbPitchShift(PITCH_SHIFTER_FACTOR, (long)BUFFER_SIZE, 2048, 8, SAMPLING_FREQ, buffer.data(), buffer.data());
    }
    
    /* Destructor */
    ~Audio() { 
        if (running) {
            stop();
        }
    }

};


int main(void) {
    
    Audio audio;   // Create instance of audio 
    audio.init();  // Initialise audio stream
    audio.run();   // Start the audio stream

    return 0;
}