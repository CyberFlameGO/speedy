//  Copyright 2022 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Test the original (third-party) sonic library to make sure it compresses
// speech as advertised.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <cmath>
#include <random>

#include "testing/base/public/gunit.h"

extern "C" {
#include "third_party/sonic/sonic.h"
#include "third_party/sonic/wave.h"
}

namespace {

class SonicTest : public ::testing::Test {
 protected:
  SonicTest() : stream_(nullptr) {
  }

  ~SonicTest() {
    DestroyStream();
  }

  void InitializeStream(int sampleRate, int numChannels) {
    stream_ = sonicCreateStream(sampleRate, numChannels);
  }

  void DestroyStream() {
    if (stream_ != nullptr) {
      sonicDestroyStream(stream_);
      stream_ = nullptr;
    }
  }

  std::vector<int16> ReadWaveFile(const std::string& fileName,
                                  int* sampleRate,
                                  int* numChannels);

  std::vector<int16> CompressSound(std::vector<int16>sound_input,
                                   int sampleRate, int numChannels,
                                   float speedup);

  void RunOneCompressionTest(std::vector<int16>input_sound,
                             int sample_rate, int num_channels,
                             float speedup, std::string test_name,
                             int size_tolerance);

  sonicStream stream_;
};


// Small function to output data (generally into /tmp files) so we can read them
// in with Matlab or Numpy to investigate errors.
template <class myType>
void WriteData(myType* data, int len, const char* file_name) {
  FILE *fp = fopen(file_name, "wt");
  if (fp) {
    for (int i=0; i < len; ++i) {
      fprintf(fp, "  %g\n", static_cast<float>(data[i]));
    }
    fclose(fp);
  } else {
    fprintf(stderr, "Can't create WriteData file at %s.\n", file_name);
  }
}

// Test that a sinusoid looks like a sinusoid. Compute the Teager energy
// operator: http://www.aes.org/e-lib/browse.cfm?elib=9892
// over a signal. This is equal to
//     x^2(n) - x(n-1)*x(n+1)
// and for a sinusoid should be a constant for all values of n.  Return the
// mean and variance of this operator (over the entire signal) as a quick and
// dirty check of sinusoidal quality.
template <class T>
void TeagerVariance(T* data, int total_samples, float* mean, float* variance) {
  assert(mean);
  assert(variance);
  float M2 = 0.0;
  *mean = 0.0;
  for (int n=1; n < total_samples-1; ++n) {
    float teager = static_cast<float>(data[n])*data[n] -
        static_cast<float>(data[n-1])*data[n+1];
    // Compute the variance of the Teager signal with an online algorithm:
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online_algorithm
    float delta = teager - *mean;
    *mean += delta/n;
    float delta2 = teager - *mean;
    M2 += delta*delta2;
  }
  *variance = M2 / (total_samples - 3);  // Since 1st & last samples skipped
}

// Run the teager operator over a input vector, perserving the values in an
// output vector (teagerVec)
template <class T>
void TeagerCompute(T* data, int total_samples, std::vector<float>* teagerVec) {
  assert(teagerVec);
  for (int n=1; n < total_samples-1; ++n) {
    float teager = static_cast<float>(data[n])*data[n] -
        static_cast<float>(data[n-1])*data[n+1];
    teagerVec->push_back(teager);
  }
}

// Compute the slope of some x and y data using linear regression.
template<class T>
float LinearSlope(std::vector<T> x, std::vector<T> y) {
  // From: http://www.statisticshowto.com/wp-content/uploads/2009/11/linearregressionequations.bmp
  assert(x.size() == y.size());
  int n = x.size();
  float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
  for (int i = 0; i < x.size(); ++i) {
    sumX += x[i];
    sumY += y[i];
    sumXY += x[i] * y[i];
    sumX2 += x[i] * x[i];
  }
  return (n*sumXY - sumX*sumY)/(n*sumX2 - sumX*sumX);
}

// Compute the slope of some y data (assuming y is uniformly sampled with a
// sampling interval of 1 unit.)
template<class T>
float LinearSlope(std::vector<T> y) {
  std::vector<T> x;
  for (int i=0; i < y.size(); i++) {
    x.push_back(i);
  }
  return LinearSlope(x, y);
}

// Simple test.  Just put in a sinusoid, speed it up, and make sure we get the
// right number of samples out, and that the output is still a sinusoid at the
// original frequency.
TEST_F(SonicTest, TestSpeedup) {
  constexpr int kSampleRate = 22050;
  constexpr uint32 kPitch = 100;  // In Hz
  constexpr uint32 kPeriodSamples = kSampleRate / kPitch;
  constexpr uint32 kAmplitude = 32000;
  constexpr uint32 kNumPeriods = 100;
  constexpr float kSpeed = 3.0f;
  InitializeStream(kSampleRate, 1);
  sonicSetSpeed(stream_, kSpeed);
  int16 pitch_period[kPeriodSamples];
  // We are speeding up, so this is big enough.
  int16 *output = new int16[kNumPeriods*kPeriodSamples];
  // Compute one cycle of a sinusoid for testing.
  for (uint32 xSample = 0; xSample < kPeriodSamples; ++xSample) {
    pitch_period[xSample] =
        static_cast<int16>(kAmplitude * sin(xSample * 2 * M_PI /
                                            kPeriodSamples));
  }
  // Feed the sinusoid to libsonic kNumPeriods times and compress the audio.
  uint32 total_samples = 0;
  for (uint32 epoch = 0; epoch < kNumPeriods; ++epoch) {
    EXPECT_TRUE(sonicWriteShortToStream(stream_, pitch_period, kPeriodSamples));
    total_samples += sonicReadShortFromStream(stream_, &output[total_samples],
                                              kPeriodSamples);
  }
  EXPECT_TRUE(sonicFlushStream(stream_));

  // Check the output length to make sure it is close to expected.
  int new_samples;
  do {
    new_samples = sonicReadShortFromStream(stream_, &output[total_samples],
                                           kPeriodSamples);
    total_samples += new_samples;
  } while (new_samples > 0);
  uint32 expected_samples = (kNumPeriods * kPeriodSamples) / kSpeed;
  EXPECT_GT(total_samples, (99 * expected_samples) / 100);
  EXPECT_LT(total_samples, (101 * expected_samples) / 100);
  WriteData(output, total_samples, "/tmp/sounds/sonic_compressed.txt");

  // Now test the output to make sure it's still a sinusoid.  Compute the
  // Teager operator over the original (single period) input sinusoid, because
  // it is quite noisy (due to 16 bit quantization).  Use the variance of this
  // signal's Teager operator to normalize the measure we compute of the sped-up
  // signal.
  float cycle_mean, cycle_var, speed_mean, speed_var;
  TeagerVariance(pitch_period, kPeriodSamples, &cycle_mean, &cycle_var);
  TeagerVariance(output, total_samples-300, &speed_mean, &speed_var);
  EXPECT_NEAR(cycle_mean, speed_mean, 0.01*cycle_mean);  // 1% error is enough
  EXPECT_NEAR(cycle_var, speed_var, 0.02*cycle_var);  // 1% error is enough
  delete[] output;
}

/*
 * Make sure that the libsonic code can respond to changes in the speed.
 * Create a linear chirp (over 3 seconds).  Then speed up the first 1s by 3x,
 * the next second by 1.5x, and the final second by 3x. Use the Teager operator
 * to estimate the resulting frequencies.  The slope of the first 1/4 of the
 * output should equal the slope of the last 1/4.  And the middle slope of the
 * compressed signal should be half the outside slopes.
 */
TEST_F(SonicTest, TestChirpSpeedup) {
  constexpr int kSampleRate = 22050;
  constexpr float kPitch0 = 137;             // In Hz
  constexpr float kPitch3 = kPitch0 + 47;    // at time=3s
  constexpr uint32 kAmplitude = 32000;
  constexpr uint32 kNumPeriods = 100;
  constexpr float kTotalLength = 3.0;
  constexpr uint32 kTotalSamples = static_cast<int>(kTotalLength * kSampleRate);
  int16* chirp = new int16[kTotalSamples];
  int16* output = new int16[kTotalSamples];
  assert(chirp); assert(output);
  constexpr float kSpeed = 3.0f;

  InitializeStream(kSampleRate, 1);

  // Compute a chirp that speeds up in the middle second. (Need a chirp so we
  // can tell where we are in the waveform.)
  for (uint32 i = 0; i < kTotalSamples; ++i) {
    float t = i/static_cast<float>(kSampleRate);
    // From https://en.wikipedia.org/wiki/Chirp
    float phase = kPitch0*t + (kPitch3-kPitch0)/3*t*t/2.0;  // units are cycles
    chirp[i] = static_cast<int16>(kAmplitude * sin(2*M_PI*phase));
  }
  WriteData(chirp, kTotalSamples, "/tmp/sounds/chirp_original.txt");
  std::vector<float> teagerVec;
  TeagerCompute(chirp, kTotalSamples, &teagerVec);
  WriteData(&teagerVec[0], teagerVec.size(),
            "/tmp/sounds/chirp_original_teager.txt");


  sonicSetSpeed(stream_, kSpeed);    // First third
  EXPECT_TRUE(sonicWriteShortToStream(stream_, chirp, kSampleRate));
  sonicSetSpeed(stream_, kSpeed/2);  // Second third
  EXPECT_TRUE(sonicWriteShortToStream(stream_, &chirp[kSampleRate],
                                      kSampleRate));
  sonicSetSpeed(stream_, kSpeed);    // Last third
  EXPECT_TRUE(sonicWriteShortToStream(stream_, &chirp[2*kSampleRate],
                                      kSampleRate));

  // Read back the results.
  uint32 totalSamples = 0;
  for (uint32 epoch = 0; epoch < kNumPeriods; ++epoch) {
    totalSamples += sonicReadShortFromStream(stream_, &output[totalSamples],
                                              kTotalSamples);
  }
  EXPECT_TRUE(sonicFlushStream(stream_));

  int new_samples;
  do {
    new_samples = sonicReadShortFromStream(stream_, &output[totalSamples],
                                           kTotalSamples);
    totalSamples += new_samples;
  } while (new_samples > 0);
  WriteData(output, totalSamples, "/tmp/sounds/chirp_compressed.txt");
  teagerVec.clear();
  TeagerCompute(output, totalSamples, &teagerVec);
  WriteData(&teagerVec[0], teagerVec.size(),
            "/tmp/sounds/chirp_compressed_teager.txt");

  // Take Sqrt of Teager output so result is proportional to frequency.
  for (int i=0; i < teagerVec.size(); i++) {
    teagerVec[i] = std::sqrt(teagerVec[i]);
  }

  // Extract the three pieces and extract their frequencies, and then the
  // slopes.
  std::vector<float>::const_iterator first = teagerVec.begin() + 0;
  std::vector<float>::const_iterator last = teagerVec.begin() +
      teagerVec.size()/4;
  std::vector<float> teagerPiece1(first, last);
  auto slope1 = LinearSlope(teagerPiece1);

  first = teagerVec.begin() + teagerVec.size()/4;
  last = teagerVec.begin() + teagerVec.size()*3/4;
  std::vector<float> teagerPiece2(first, last);
  auto slope2 = LinearSlope(teagerPiece2);

  first = teagerVec.begin() + teagerVec.size()*3/4;
  last = teagerVec.end();
  std::vector<float> teagerPiece3(first, last);
  auto slope3 = LinearSlope(teagerPiece3);

  LOG(INFO) << "Compressed chirp slopes: " << slope1 << " -- " << slope2 <<
      " -- " << slope3 << std::endl;

  EXPECT_NEAR(slope1, slope3, slope1*.01);
  EXPECT_NEAR(slope2, slope1/2, slope1*.01);

  delete[] chirp;
  delete[] output;
}

/*
 * To visualize this data, use the following Matlab code:

load chirp_compressed_teager.txt
load chirp_original_teager.txt

%%

N = 20;
smooth_original = chirp_original_teager;
for i=1:length(smooth_original)
    b = max(1, i-N); e = min(i+N, length(smooth_original));
    smooth_original(i) = mean(sqrt(chirp_original_teager(b:e)));
end

smooth_compressed = chirp_compressed_teager;
for i=1:length(smooth_compressed)
    b = max(1, i-N); e = min(i+N, length(smooth_compressed));
    smooth_compressed(i) = mean(sqrt(chirp_compressed_teager(b:e)));
end
%%

scale = 137/1250;  % Empirically determined

plot((1:length(chirp_original_teager))/length(chirp_original_teager), ...
    smooth_original*scale, ...
    (1:length(chirp_compressed_teager))/length(chirp_compressed_teager), ...
    smooth_compressed*scale)

hold on;
a = axis;
plot([1/4, 1/4], [a(3), a(4)], 'r--');
plot([3/4, 3/4], [a(3), a(4)], 'r--');
hold off

xlabel('Relative Time');
ylabel('Chirp Frequency');
legend('Original', 'LibSonic Spedup')
title('Measuring WSOLA Reaction Time');


*/


std::vector<int16> SonicTest::ReadWaveFile(const std::string& fileName,
                                           int* sampleRate,
                                           int* numChannels) {
  const int32 kBufferSize = 1024;
  int16 buffer[kBufferSize];
  std::vector<int16>outputVector;
  auto fp = openInputWaveFile(fileName.c_str(), sampleRate, numChannels);
  EXPECT_TRUE(fp);
  if (fp) {
    int numRead;
    do {
      numRead = readFromWaveFile(fp, buffer, kBufferSize / *numChannels);
      outputVector.insert(outputVector.end(), buffer, buffer+numRead);
    } while (numRead > 0);
    closeWaveFile(fp);
  }
  return outputVector;
}

#define  imin(a, b) ((a) < (b)? (a): (b))

// Run one compression test using a monaural input, and make sure the output
// has the correct size.
std::vector<int16> SonicTest::CompressSound(std::vector<int16>sound_input,
                                            int sampleRate, int numChannels,
                                            float speedup) {
  InitializeStream(sampleRate, numChannels);
  const int kBufferSize = 1024;   // Number of time steps (multichannel samples)
  int16 *sound_buffer = new int16[kBufferSize * numChannels];
  std::vector<int16> sound_output;
  sonicSetSpeed(stream_, speedup);
  int num_time_steps = sound_input.size()/numChannels;
  for (uint32 t = 0; t < num_time_steps; t += kBufferSize) {
    // The index t is in terms of time stamps, as is the this_buffer_size.
    // Multiply by num_channels to get the number of values we are passing.
    int this_buffer_size = imin(kBufferSize, num_time_steps - t);
    EXPECT_TRUE(sonicWriteShortToStream(stream_, &sound_input[numChannels*t],
                                        this_buffer_size));
    int samples_read = sonicReadShortFromStream(stream_, sound_buffer,
                                                kBufferSize);
    if (speedup == 2.0) printf("Got back %d samples.\n", samples_read);
    sound_output.insert(sound_output.end(), sound_buffer,
                        sound_buffer + numChannels*samples_read);
  }
  // Close the stream out, and grab the last of the samples.
  EXPECT_TRUE(sonicFlushStream(stream_));
  int samples_read;
  do {
    samples_read = sonicReadShortFromStream(stream_, sound_buffer,
                                            kBufferSize);
    if (speedup == 2.0) printf("Got back %d samples.\n", samples_read);
    sound_output.insert(sound_output.end(), sound_buffer,
                        sound_buffer + numChannels*samples_read);
  } while (samples_read > 0);
  delete[] sound_buffer;
  DestroyStream();
  return sound_output;
}

// Run one compression test using a monaural input, and make sure the output
// has the correct size.
void SonicTest::RunOneCompressionTest(std::vector<int16>input_sound,
                                      int sample_rate, int num_channels,
                                      float speedup, std::string test_name,
                                      int size_tolerance) {
  printf("RunOneCompressionTest for %s:\n", test_name.c_str());
  auto compressed_sound = CompressSound(input_sound, sample_rate, num_channels,
                                        speedup);
  int64 expected_sample_count = input_sound.size()/speedup;
  printf("%g: Expected size is %05ld, actual is %05ld, difference is %ld.\n",
         speedup, expected_sample_count, compressed_sound.size(),
         compressed_sound.size() - expected_sample_count);
  EXPECT_NEAR(compressed_sound.size(), expected_sample_count, size_tolerance);
}

// Test Sonic using a real speech utterance, and over a range of speedups. Make
// sure the final lengths are right.
TEST_F(SonicTest, TestFullSpeechRange) {
  std::string fullFileName = FLAGS_test_srcdir +
      "/google3/third_party/speedy/test_data/tapestry.wav";
  int sampleRate, numChannels;
  auto tapestryInts = ReadWaveFile(fullFileName, &sampleRate, &numChannels);
  EXPECT_EQ(tapestryInts.size(), 50381);
  EXPECT_EQ(sampleRate, 16000);
  EXPECT_EQ(numChannels, 1);

  for (float speedup=1.1; speedup < 6.3; speedup += 0.25) {
    RunOneCompressionTest(tapestryInts, sampleRate, numChannels, speedup,
                          "TestFullSpeechRange - " + std::to_string(speedup),
                          speedup * 5 * sampleRate/1000);    // 5 ms * speedup
  }
}

// Now do a Sonic test using a long stereo example (which tweaked an earlier
// version of libsonic.) Make sure the final lengths are right.
TEST_F(SonicTest, TestLongStereoSpeechRange) {
  std::string fullFileName = FLAGS_test_srcdir +
      "/google3/third_party/speedy/test_data/capture_1_00x.wav";
  int sampleRate, numChannels;
  auto soundInts = ReadWaveFile(fullFileName, &sampleRate, &numChannels);
  ASSERT_GT(soundInts.size(), 0);
  EXPECT_EQ(sampleRate, 48000);
  EXPECT_EQ(numChannels, 2);

  for (float speedup=1.1; speedup < 6.3; speedup += 0.5) {
    RunOneCompressionTest(soundInts, sampleRate, numChannels, speedup,
                          "TestLongStereoSpeechRange - " +
                          std::to_string(speedup),
                          170 * sampleRate/1000);    // 170ms
  }
}

// Test libsonic with a noisy (unvoiced) waveform.
TEST_F(SonicTest, TestFullNoiseRange) {
  const int kSampleRate = 16000;
  std::vector<int16> noiseInts;
  std::default_random_engine generator;
  std::normal_distribution<float> distribution(0.0, 1.0);
  for (int i=0; i < 50000; i++) {
    float f = distribution(generator) * 8096;
    int16 s = static_cast<int16>(fmax(-32000, std::fmin(32000, f)));
    noiseInts.push_back(s);
  }

  int numChannels = 1;
  for (float speedup=1.1; speedup < 6.3; speedup += 0.25) {
    printf("Testing noise with a speedup of %g.\n", speedup); fflush(stdout);
    RunOneCompressionTest(noiseInts, kSampleRate, numChannels, speedup,
                          "TestFullNoiseRange - " + std::to_string(speedup),
                          1.5 * kSampleRate / 100);
  }
}

// Test the sonic library by giving it a sinusoid in stereo.  Make sure that
// the resulting sinusoid doesn't have any glitches (by checking the Teager
// operator.)  And that the samples are the same in mono and stereo.
TEST_F(SonicTest, TestSinusoidStereoMatch) {
  const int sample_rate = 16000, num_channels = 1, num_samples = sample_rate;

  // Create the monaural stereo sample and speed it up.
  std::vector<int16> sinusoid_mono;
  for (int i=0; i < num_samples; i++) {
    const float F0 = 440;
    int16 sample = 16000*sin(2*M_PI*F0*i/(float)sample_rate);
    sinusoid_mono.push_back(sample);
  }
  auto wave_fp = openOutputWaveFile("/tmp/sounds/original_sinusoid.wav",
                                    sample_rate, num_channels);
  if (wave_fp) {
    writeToWaveFile(wave_fp, &sinusoid_mono[0], sinusoid_mono.size());
    closeWaveFile(wave_fp);
  }
  const float speedup = 2.0;
  auto mono_spedup = CompressSound(sinusoid_mono, sample_rate, num_channels,
                                    speedup);
  ASSERT_GE(mono_spedup.size(), 0);
  wave_fp = openOutputWaveFile("/tmp/sounds/mono_sinusoid.wav",
                               sample_rate, num_channels);
  if (wave_fp) {
    writeToWaveFile(wave_fp, &mono_spedup[0], mono_spedup.size());
    closeWaveFile(wave_fp);
  }
  std::vector<float> teager_values;
  TeagerCompute(&mono_spedup[0], mono_spedup.size(), &teager_values);
  WriteData(&teager_values[0], teager_values.size(),
            "/tmp/sounds/mono_teager.txt");

  // Copy the monaural sinusoid into a stereo vector (by duplicating samples.)
  std::vector<int16> sinusoid_stereo;
  for (int16 sample : sinusoid_mono) {
    sinusoid_stereo.push_back(sample);
    sinusoid_stereo.push_back(sample);
  }
  wave_fp = openOutputWaveFile("/tmp/sounds/stereo_sinusoid.wav",
                               sample_rate, num_channels);
  if (wave_fp) {
    writeToWaveFile(wave_fp, &sinusoid_stereo[0], sinusoid_stereo.size());
    closeWaveFile(wave_fp);
  }
  auto stereo_spedup = CompressSound(sinusoid_stereo, sample_rate,
                                     2*num_channels, speedup);
  ASSERT_GE(stereo_spedup.size(), 0);
  wave_fp = openOutputWaveFile("/tmp/sounds/stereo_sinusoid.wav",
                               sample_rate, 2*num_channels);
  if (wave_fp) {
    writeToWaveFile(wave_fp, &stereo_spedup[0], stereo_spedup.size()/2);
    closeWaveFile(wave_fp);
  }
  // Look for glitches using the Teager operator
  std::vector<int16> left_stereo;
  for (int i=0; i < stereo_spedup.size()/2; i++) {
    left_stereo.push_back(stereo_spedup[2*i]);
  }
  teager_values.clear();
  TeagerCompute(&left_stereo[0], left_stereo.size(), &teager_values);
  WriteData(&teager_values[0], teager_values.size(),
            "/tmp/sounds/stereo_teager.txt");
  const int num_to_check = 100;
  float mode = accumulate(teager_values.begin(),
                          teager_values.begin() + num_to_check, 0.0) /
      num_to_check;
  std::vector<float> teager_errors;
  for (auto s : teager_values) {
    teager_errors.push_back((std::abs(s) - mode)/mode > 0.05);
  }
  int last_i = 0;
  for (int i=0; i < teager_errors.size(); i++) {
    if (teager_errors[i]) {
      std::cout << "Error at time " << i << ", delta time is " << i - last_i <<
          std::endl;
      last_i = i;
    }
  }

  // Check to make sure that the mono and stereo agree.
  for (int i = 0; i < mono_spedup.size(); i++) {
    ASSERT_EQ(mono_spedup[i],
              stereo_spedup[2*i]) << "Testing left sample " << i;
    ASSERT_EQ(mono_spedup[i],
              stereo_spedup[2*i+1]) << "Testing right sample " << i;
  }
}


TEST_F(SonicTest, TestStereoMatch) {
  std::string fullFileName = FLAGS_test_srcdir +
      "/google3/third_party/speedy/test_data/tapestry.wav";
  int sampleRate, numChannels;
  auto tapestry_mono = ReadWaveFile(fullFileName, &sampleRate, &numChannels);
  EXPECT_EQ(tapestry_mono.size(), 50381);
  EXPECT_EQ(sampleRate, 16000);
  EXPECT_EQ(numChannels, 1);

  const float speedup = 2.0;
  auto mono_spedup = CompressSound(tapestry_mono, sampleRate, numChannels,
                                    speedup);
  ASSERT_GE(mono_spedup.size(), 0);
  auto wave_fp = openOutputWaveFile("/tmp/sounds/mono.wav",
                                    sampleRate, numChannels);
  if (wave_fp) {
    writeToWaveFile(wave_fp, &mono_spedup[0], mono_spedup.size());
    closeWaveFile(wave_fp);
  }
  std::vector<int16> tapestry_stereo;
  for (int16 sample : tapestry_mono) {
    tapestry_stereo.push_back(sample);
    tapestry_stereo.push_back(sample);
  }
  auto stereo_spedup = CompressSound(tapestry_stereo, sampleRate, 2*numChannels,
                                    speedup);
  ASSERT_GE(stereo_spedup.size(), 0);
  wave_fp = openOutputWaveFile("/tmp/sounds/stereo.wav",
                               sampleRate, 2*numChannels);
  if (wave_fp) {
    writeToWaveFile(wave_fp, &stereo_spedup[0], stereo_spedup.size()/2);
    closeWaveFile(wave_fp);
  }
  for (int i = 0; i < mono_spedup.size(); i++) {
    ASSERT_EQ(mono_spedup[i],
              stereo_spedup[2*i]) << "Testing left sample " << i;
    ASSERT_EQ(mono_spedup[i],
              stereo_spedup[2*i+1]) << "Testing right sample " << i;
  }
}


}  // namespace