#pragma once

/*
 * Colour matching maths for the Color Match filter (color-match.cpp). Plain C++ with no OBS calls, so it can
 * be tested on its own.
 *
 * Input: a few small pictures from each camera, as 8-bit RGBA in the usual (gamma-encoded) sRGB that OBS
 * outputs. Output: a correction for the target camera, made of
 *   gain[3]      white balance, tint and RGB balance: one multiplier per channel, in linear light
 *   curve[33]    one tone curve for exposure, brightness, contrast, gamma, shadows, midtones and highlights,
 *                in gamma-encoded values, sampled at 0, 1/32, ... 1 (always rising)
 *   saturation   1 = unchanged
 * plus a confidence score (0-100). The filter only applies a correction scored at or above kMinConfidence.
 *
 * The correction is deliberately simple and limited (see colorMatch()), so it can shift the whole picture the
 * way a different camera setting would, but can't repaint individual objects to look like the other camera's.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace cm {

constexpr int kCurvePoints = 33;
constexpr int kMinConfidence = 60;

struct Frame {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba; // width * height * 4, rows packed
};

struct Params {
	float gain[3] = {1.0f, 1.0f, 1.0f};
	float curve[kCurvePoints];
	float saturation = 1.0f;
	Params();
	bool isIdentity() const;
};

struct Result {
	bool ok = false;     // true: confident enough to apply `params`
	int confidence = 0;  // 0-100
	std::string code;    // OK, NO_PICTURE, FROZEN, LOW_CONFIDENCE
	std::string message; // plain words for the operator
	Params params;       // the correction (filled in even when not ok, for the log)
	std::string details; // numbers for OBS's log
};

// referenceName and targetName are how the operator knows the two cameras ("Camera 1"), used in messages.
Result colorMatch(const std::vector<Frame> &reference, const std::vector<Frame> &target,
		  const std::string &referenceName, const std::string &targetName);

// The 256-entry lookup table for a curve (monotone cubic through the 33 points), values 0-1.
void curveTable(const float curve[kCurvePoints], float out[256]);

// How different two corrections look, roughly in 8-bit steps (used to skip tiny changes in Continuous mode).
float difference(const Params &a, const Params &b);

// One pixel through the correction, exactly as the filter's shader does it (for tests).
void applyPixel(const Params &p, const uint8_t in[3], uint8_t out[3]);

// a + (b - a) * t for every value (used to fade between corrections).
Params blend(const Params &a, const Params &b, float t);

} // namespace cm
