#ifndef SMBPITCHSHIFT_H
#define SMBPITCHSHIFT_H

void smbPitchShift(float pitchShift, long numSampsToProcess, long fftFrameSize, 
                   long osamp, float sampleRate, float *indata, float *outdata);

#endif