/*
 * Color matching maths. See color-match-math.h for what goes in and comes out.
 *
 * How the correction is worked out
 *   1. Check both pictures are real: not black, not a frozen "no signal" screen.
 *   2. White balance, tint and RGB balance: find each camera's near-neutral areas (walls, ceiling, white
 *      shirts, grey carpet) by repeatedly balancing the picture and keeping the pixels that come out nearly
 *      grey. The ratio between the two cameras' neutrals gives one gain per channel. Medians are used
 *      throughout, so a colored banner or a red curtain on one side doesn't drag the result.
 *   3. Skin tones, only when both pictures show enough skin-colored area: nudge the gains a little so skin
 *      agrees too, and only when the two already roughly agree.
 *   4. Tone: compare the brightness distributions percentile by percentile, fit a smooth curve
 *      (offset + gain * x^gamma, which covers exposure, contrast, gamma, shadows and highlights), then let it
 *      follow the measured shape a little (half the difference, at most 3%). Clipped and crushed areas
 *      are kept out of the fit.
 *   5. Saturation: compare how colorful the two pictures are after steps 2-4.
 *   6. Confidence: lowered for little to judge color by, very different scene content, measurements that
 *      disagree from frame to frame, or an implausibly large correction.
 *
 * Every adjustment is limited (gains 0.6-1.6, saturation 0.7-1.4, curve offset and slope), so even a wrong
 * result can't make the picture unrecognisable.
 */

#include "color-match-math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace cm {

namespace {

float srgbToLinear(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c)
{
	if (c <= 0.0f)
		return 0.0f;
	return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

struct Lut {
	float lin[256];
	Lut()
	{
		for (int i = 0; i < 256; ++i)
			lin[i] = srgbToLinear(i / 255.0f);
	}
};
const Lut &lut()
{
	static const Lut l;
	return l;
}

// One sampled pixel: gamma-encoded (0-1) and linear
struct Px {
	float g[3];
	float l[3];
};

float lumaGamma(const float g[3])
{
	return 0.2126f * g[0] + 0.7152f * g[1] + 0.0722f * g[2];
}

float lumaLinear(const float l[3])
{
	return 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
}

// Up to `maxCount` pixels spread evenly over all the frames
std::vector<Px> pool(const std::vector<Frame> &frames, size_t maxCount)
{
	size_t total = 0;
	for (const Frame &f : frames)
		total += (size_t)f.width * (size_t)f.height;
	const size_t step = std::max<size_t>(1, total / std::max<size_t>(1, maxCount));
	std::vector<Px> out;
	out.reserve(total / step + 1);
	const Lut &L = lut();
	size_t k = 0;
	for (const Frame &f : frames) {
		const size_t n = (size_t)f.width * (size_t)f.height;
		for (size_t i = 0; i < n; ++i, ++k) {
			if (k % step)
				continue;
			const uint8_t *p = &f.rgba[i * 4];
			Px px;
			for (int c = 0; c < 3; ++c) {
				px.g[c] = p[c] / 255.0f;
				px.l[c] = L.lin[p[c]];
			}
			out.push_back(px);
		}
	}
	return out;
}

// A clipped or crushed pixel says nothing reliable about color
bool usable(const Px &p)
{
	const float mx = std::max({p.g[0], p.g[1], p.g[2]});
	return mx < 0.97f && lumaGamma(p.g) > 0.04f;
}

float median(std::vector<float> &v)
{
	if (v.empty())
		return 0.0f;
	const size_t mid = v.size() / 2;
	std::nth_element(v.begin(), v.begin() + (long)mid, v.end());
	return v[mid];
}

float percentile(const std::vector<float> &sorted, float q)
{
	if (sorted.empty())
		return 0.0f;
	const float pos = q * (float)(sorted.size() - 1);
	const size_t i = (size_t)pos;
	const float t = pos - (float)i;
	return i + 1 < sorted.size() ? sorted[i] * (1 - t) + sorted[i + 1] * t : sorted[i];
}

/* ------------------------------------------------------------------ picture checks */

struct PictureStats {
	float mean = 0, stddev = 0; // gamma luma, 0-255
	float motion = 0;           // mean difference between the first and last frame, 0-255
};

PictureStats pictureStats(const std::vector<Frame> &frames)
{
	PictureStats s;
	if (frames.empty())
		return s;
	const Frame &f = frames.back();
	const size_t n = (size_t)f.width * (size_t)f.height;
	double sum = 0, sq = 0;
	for (size_t i = 0; i < n; ++i) {
		const uint8_t *p = &f.rgba[i * 4];
		const double y = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
		sum += y;
		sq += y * y;
	}
	if (n) {
		s.mean = (float)(sum / n);
		s.stddev = (float)std::sqrt(std::max(0.0, sq / n - (sum / n) * (sum / n)));
	}
	const Frame &a = frames.front();
	if (frames.size() > 1 && a.width == f.width && a.height == f.height) {
		double d = 0;
		for (size_t i = 0; i < n * 4; ++i)
			if (i % 4 != 3)
				d += std::abs((int)a.rgba[i] - (int)f.rgba[i]);
		s.motion = (float)(d / (n * 3.0));
	} else {
		s.motion = 99.0f; // can't tell; don't call it frozen
	}
	return s;
}

/* ------------------------------------------------------------------ white balance */

struct Cast {
	float rg = 1, bg = 1; // the camera's neutral color: red/green and blue/green, linear
	float fraction = 0;   // share of usable pixels that came out neutral
	bool fromNeutrals = false;
};

Cast estimateCast(const std::vector<Px> &px)
{
	Cast c;
	std::vector<float> rg, bg;
	rg.reserve(px.size());
	bg.reserve(px.size());
	size_t usableCount = 0;
	// Start from the median color of the midtones ("grey world"), then keep what comes out nearly grey
	for (const Px &p : px) {
		if (!usable(p))
			continue;
		++usableCount;
		const float y = lumaLinear(p.l);
		if (y < 0.02f || y > 0.8f || p.l[1] < 1e-4f)
			continue;
		rg.push_back(p.l[0] / p.l[1]);
		bg.push_back(p.l[2] / p.l[1]);
	}
	if (rg.size() < 50)
		return c;
	c.rg = median(rg);
	c.bg = median(bg);
	const float thresholds[] = {0.25f, 0.16f, 0.11f, 0.11f};
	for (float thr : thresholds) {
		rg.clear();
		bg.clear();
		const float kr = 1.0f / std::max(c.rg, 1e-3f), kb = 1.0f / std::max(c.bg, 1e-3f);
		for (const Px &p : px) {
			if (!usable(p) || p.l[1] < 1e-4f)
				continue;
			const float lin[3] = {p.l[0] * kr, p.l[1], p.l[2] * kb};
			const float y = lumaLinear(lin);
			if (y < 0.02f || y > 0.8f)
				continue;
			const float g[3] = {linearToSrgb(lin[0]), linearToSrgb(lin[1]), linearToSrgb(lin[2])};
			const float mx = std::max({g[0], g[1], g[2]}), mn = std::min({g[0], g[1], g[2]});
			if ((mx - mn) / std::max(mx, 1e-3f) > thr)
				continue;
			rg.push_back(p.l[0] / p.l[1]);
			bg.push_back(p.l[2] / p.l[1]);
		}
		if (rg.size() < 30)
			break;
		c.fraction = usableCount ? (float)rg.size() / (float)usableCount : 0.0f;
		c.rg = median(rg);
		c.bg = median(bg);
		c.fromNeutrals = true;
	}
	return c;
}

/* ------------------------------------------------------------------ applying a correction to samples */

void applyGains(const Px &p, const float gain[3], float lin[3], float g[3])
{
	for (int c = 0; c < 3; ++c) {
		lin[c] = p.l[c] * gain[c];
		g[c] = clampf(linearToSrgb(lin[c]), 0.0f, 1.0f);
	}
}

float curveAt(const float table[256], float x)
{
	const float pos = clampf(x, 0.0f, 1.0f) * 255.0f;
	const int i = std::min(254, (int)pos);
	const float t = pos - (float)i;
	return table[i] * (1 - t) + table[i + 1] * t;
}

// The fully corrected sample, in linear light
void correct(const Px &p, const Params &prm, const float table[256], float out[3])
{
	float lin[3], g[3];
	applyGains(p, prm.gain, lin, g);
	for (int c = 0; c < 3; ++c)
		g[c] = curveAt(table, g[c]);
	const float y = lumaGamma(g);
	for (int c = 0; c < 3; ++c)
		out[c] = srgbToLinear(clampf(y + (g[c] - y) * prm.saturation, 0.0f, 1.0f));
}

void oklab(const float l[3], float &L, float &a, float &b)
{
	const float lm = std::cbrt(0.4122214708f * l[0] + 0.5363325363f * l[1] + 0.0514459929f * l[2]);
	const float mm = std::cbrt(0.2119034982f * l[0] + 0.6806995451f * l[1] + 0.1073969566f * l[2]);
	const float sm = std::cbrt(0.0883024619f * l[0] + 0.2817188376f * l[1] + 0.6299787005f * l[2]);
	L = 0.2104542553f * lm + 0.7936177850f * mm - 0.0040720468f * sm;
	a = 1.9779984951f * lm - 2.4285922050f * mm + 0.4505937099f * sm;
	b = 0.0259040371f * lm + 0.7827717662f * mm - 0.8086757660f * sm;
}

// Color and brightness histograms, for "do the two cameras see similar things?"
struct Hist {
	static constexpr int AB = 12, LB = 16;
	std::vector<double> ab = std::vector<double>(AB * AB + 1, 0.0);
	std::vector<double> lum = std::vector<double>(LB, 0.0);
	void add(const float lin[3])
	{
		float L, a, b;
		oklab(lin, L, a, b);
		if (std::hypot(a, b) < 0.02f)
			ab[AB * AB] += 1;
		else {
			const int ia = std::clamp((int)((a + 0.2f) / 0.4f * AB), 0, AB - 1);
			const int ib = std::clamp((int)((b + 0.2f) / 0.4f * AB), 0, AB - 1);
			ab[ia * AB + ib] += 1;
		}
		lum[std::clamp((int)(L * LB), 0, LB - 1)] += 1;
	}
	static double intersect(const std::vector<double> &x, const std::vector<double> &y)
	{
		const double sx = std::accumulate(x.begin(), x.end(), 0.0),
			     sy = std::accumulate(y.begin(), y.end(), 0.0);
		if (sx <= 0 || sy <= 0)
			return 0;
		double s = 0;
		for (size_t i = 0; i < x.size(); ++i)
			s += std::min(x[i] / sx, y[i] / sy);
		return s;
	}
};

/* ------------------------------------------------------------------ tone curve */

struct ToneFit {
	float a = 0, b = 1, g = 1;
	bool limited = false; // hit one of the limits
	float at(float x) const { return a + b * std::pow(clampf(x, 0.0f, 1.0f), g); }
};

ToneFit fitTone(const std::vector<float> &t, const std::vector<float> &r)
{
	ToneFit best;
	double bestErr = 1e30;
	for (float g = 0.5f; g <= 2.001f; g += 0.02f) {
		double mz = 0, mr = 0;
		std::vector<double> z(t.size());
		for (size_t i = 0; i < t.size(); ++i) {
			z[i] = std::pow(std::max(0.0f, t[i]), g);
			mz += z[i];
			mr += r[i];
		}
		mz /= (double)t.size();
		mr /= (double)t.size();
		double cov = 0, var = 0;
		for (size_t i = 0; i < t.size(); ++i) {
			cov += (z[i] - mz) * (r[i] - mr);
			var += (z[i] - mz) * (z[i] - mz);
		}
		if (var < 1e-8)
			continue;
		ToneFit f;
		f.g = g;
		f.b = (float)(cov / var);
		f.a = (float)(mr - f.b * mz);
		const float b0 = f.b, a0 = f.a;
		f.b = clampf(f.b, 0.6f, 1.6f);
		f.a = clampf(f.a, -0.1f, 0.15f);
		f.limited = f.b != b0 || f.a != a0;
		double err = 0;
		for (size_t i = 0; i < t.size(); ++i) {
			const double d = f.at(t[i]) - r[i];
			err += d * d;
		}
		if (err < bestErr) {
			bestErr = err;
			best = f;
		}
	}
	return best;
}

// The measured mapping (target brightness -> reference brightness) at x, from percentile pairs
bool mappedAt(const std::vector<float> &t, const std::vector<float> &r, float x, float &out)
{
	if (t.empty() || x < t.front() || x > t.back())
		return false;
	for (size_t i = 1; i < t.size(); ++i) {
		if (x <= t[i]) {
			const float span = t[i] - t[i - 1];
			const float k = span > 1e-5f ? (x - t[i - 1]) / span : 0.5f;
			out = r[i - 1] + (r[i] - r[i - 1]) * k;
			return true;
		}
	}
	out = r.back();
	return true;
}

std::string fmt(const char *f, double a = 0, double b = 0, double c = 0, double d = 0)
{
	char buf[256];
	snprintf(buf, sizeof(buf), f, a, b, c, d);
	return buf;
}

} // namespace

/* ------------------------------------------------------------------ public */

Params::Params()
{
	for (int i = 0; i < kCurvePoints; ++i)
		curve[i] = (float)i / (kCurvePoints - 1);
}

bool Params::isIdentity() const
{
	const Params id;
	return difference(*this, id) < 0.5f;
}

void curveTable(const float curve[kCurvePoints], float out[256])
{
	// Monotone cubic (Fritsch-Carlson): smooth, and never overshoots or turns back down
	const int n = kCurvePoints;
	const float h = 1.0f / (n - 1);
	float d[kCurvePoints - 1], m[kCurvePoints];
	for (int i = 0; i < n - 1; ++i)
		d[i] = (curve[i + 1] - curve[i]) / h;
	m[0] = d[0];
	m[n - 1] = d[n - 2];
	for (int i = 1; i < n - 1; ++i)
		m[i] = (d[i - 1] * d[i] <= 0) ? 0.0f : (d[i - 1] + d[i]) / 2;
	for (int i = 0; i < n - 1; ++i) {
		if (d[i] == 0) {
			m[i] = m[i + 1] = 0;
			continue;
		}
		const float a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
		if (s > 9) {
			const float tau = 3.0f / std::sqrt(s);
			m[i] = tau * a * d[i];
			m[i + 1] = tau * b * d[i];
		}
	}
	for (int k = 0; k < 256; ++k) {
		const float x = k / 255.0f;
		const int i = std::min(n - 2, (int)(x / h));
		const float t = (x - i * h) / h, t2 = t * t, t3 = t2 * t;
		const float v = (2 * t3 - 3 * t2 + 1) * curve[i] + (t3 - 2 * t2 + t) * h * m[i] +
				(-2 * t3 + 3 * t2) * curve[i + 1] + (t3 - t2) * h * m[i + 1];
		out[k] = clampf(v, 0.0f, 1.0f);
	}
	for (int k = 1; k < 256; ++k)
		out[k] = std::max(out[k], out[k - 1]);
}

float difference(const Params &a, const Params &b)
{
	float ta[256], tb[256];
	curveTable(a.curve, ta);
	curveTable(b.curve, tb);
	float d = 0;
	for (int k = 0; k < 256; ++k)
		d = std::max(d, std::abs(ta[k] - tb[k]) * 255.0f);
	for (int c = 0; c < 3; ++c) // a mid grey through each gain
		d = std::max(d, std::abs(linearToSrgb(0.18f * a.gain[c]) - linearToSrgb(0.18f * b.gain[c])) * 255.0f);
	return std::max(d, std::abs(a.saturation - b.saturation) * 60.0f);
}

void applyPixel(const Params &p, const uint8_t in[3], uint8_t out[3])
{
	float table[256], lin[3];
	curveTable(p.curve, table);
	Px px;
	for (int c = 0; c < 3; ++c) {
		px.g[c] = in[c] / 255.0f;
		px.l[c] = lut().lin[in[c]];
	}
	correct(px, p, table, lin);
	for (int c = 0; c < 3; ++c)
		out[c] = (uint8_t)std::lround(clampf(linearToSrgb(lin[c]), 0.0f, 1.0f) * 255.0f);
}

Params blend(const Params &a, const Params &b, float t)
{
	Params p;
	for (int c = 0; c < 3; ++c)
		p.gain[c] = a.gain[c] + (b.gain[c] - a.gain[c]) * t;
	for (int i = 0; i < kCurvePoints; ++i)
		p.curve[i] = a.curve[i] + (b.curve[i] - a.curve[i]) * t;
	p.saturation = a.saturation + (b.saturation - a.saturation) * t;
	return p;
}

Result colorMatch(const std::vector<Frame> &reference, const std::vector<Frame> &target,
		  const std::string &referenceName, const std::string &targetName)
{
	Result res;

	/* 1. Real pictures? The same tests as the controller's Ready check. */
	const PictureStats rs = pictureStats(reference), ts = pictureStats(target);
	auto picture = [&](const PictureStats &s, const std::string &name) -> bool {
		if (s.mean < 10 && s.stddev < 6) {
			res.code = "NO_PICTURE";
			res.message = "The picture from " + name + " is black. Check that the camera is on.";
			return false;
		}
		if (s.motion < 0.3f) {
			res.code = "FROZEN";
			res.message = "The picture from " + name +
				      " isn't moving: the camera may be asleep, or the capture card is showing its "
				      "\"no signal\" screen.";
			return false;
		}
		return true;
	};
	if (!picture(rs, referenceName) || !picture(ts, targetName))
		return res;

	const std::vector<Px> rp = pool(reference, 60000), tp = pool(target, 60000);
	auto usableShare = [](const std::vector<Px> &v) {
		size_t n = 0;
		for (const Px &p : v)
			n += usable(p);
		return v.empty() ? 0.0f : (float)n / (float)v.size();
	};
	const float rUsable = usableShare(rp), tUsable = usableShare(tp);

	/* 2. White balance from each camera's neutrals */
	const Cast rc = estimateCast(rp), tc = estimateCast(tp);
	Params &P = res.params;
	P.gain[0] = clampf(rc.rg / std::max(tc.rg, 1e-3f), 0.6f, 1.6f);
	P.gain[1] = 1.0f;
	P.gain[2] = clampf(rc.bg / std::max(tc.bg, 1e-3f), 0.6f, 1.6f);
	const bool gainLimited = P.gain[0] == 0.6f || P.gain[0] == 1.6f || P.gain[2] == 0.6f || P.gain[2] == 1.6f;

	// Frame-to-frame agreement: the same estimate on each pair of frames on its own
	float spread = 0;
	const size_t pairs = std::min(reference.size(), target.size());
	if (pairs >= 2) {
		std::vector<float> lr, lb;
		for (size_t i = 0; i < pairs; ++i) {
			const Cast a = estimateCast(pool({reference[i]}, 12000)),
				   b = estimateCast(pool({target[i]}, 12000));
			lr.push_back(std::log(a.rg / std::max(b.rg, 1e-3f)));
			lb.push_back(std::log(a.bg / std::max(b.bg, 1e-3f)));
		}
		auto sd = [](const std::vector<float> &v) {
			const double m = std::accumulate(v.begin(), v.end(), 0.0) / (double)v.size();
			double s = 0;
			for (float x : v)
				s += (x - m) * (x - m);
			return (float)std::sqrt(s / (double)v.size());
		};
		spread = std::max(sd(lr), sd(lb));
	}

	/* 3. Skin tones, when both pictures clearly show some */
	auto skinRatios = [](const std::vector<Px> &v, const float gain[3], float &rg, float &bg) -> float {
		std::vector<float> a, b;
		for (const Px &p : v) {
			if (!usable(p))
				continue;
			float lin[3], g[3];
			applyGains(p, gain, lin, g);
			const float R = g[0] * 255, G = g[1] * 255, B = g[2] * 255;
			const float Y = 0.299f * R + 0.587f * G + 0.114f * B;
			const float Cb = 128 - 0.168736f * R - 0.331264f * G + 0.5f * B;
			const float Cr = 128 + 0.5f * R - 0.418688f * G - 0.081312f * B;
			if (Y < 50 || Y > 220 || Cb < 77 || Cb > 127 || Cr < 133 || Cr > 173 || lin[1] < 1e-4f)
				continue;
			a.push_back(lin[0] / lin[1]);
			b.push_back(lin[2] / lin[1]);
		}
		rg = median(a);
		bg = median(b);
		return v.empty() ? 0.0f : (float)a.size() / (float)v.size();
	};
	int skin = 0; // 0 not used, 1 agreed and used, -1 disagreed
	{
		const float one[3] = {1, 1, 1};
		float rrg, rbg, trg, tbg;
		const float rf = skinRatios(rp, one, rrg, rbg), tf = skinRatios(tp, P.gain, trg, tbg);
		if (rf > 0.01f && tf > 0.01f && trg > 0 && tbg > 0) {
			const float dr = rrg / trg, db = rbg / tbg;
			if (std::abs(std::log(dr)) < 0.2f && std::abs(std::log(db)) < 0.2f) {
				P.gain[0] = clampf(P.gain[0] * std::pow(dr, 0.3f), 0.6f, 1.6f);
				P.gain[2] = clampf(P.gain[2] * std::pow(db, 0.3f), 0.6f, 1.6f);
				skin = 1;
			} else {
				skin = -1;
			}
		}
	}
	// Brightness is the tone curve's job: keep the gains from changing it
	{
		const float k = 1.0f / (0.2126f * P.gain[0] + 0.7152f * P.gain[1] + 0.0722f * P.gain[2]);
		for (float &g : P.gain)
			g *= k;
	}

	/* 4. Tone curve */
	std::vector<float> ry, ty;
	ry.reserve(rp.size());
	ty.reserve(tp.size());
	for (const Px &p : rp)
		ry.push_back(lumaGamma(p.g));
	for (const Px &p : tp) {
		float lin[3], g[3];
		applyGains(p, P.gain, lin, g);
		ty.push_back(lumaGamma(g));
	}
	std::sort(ry.begin(), ry.end());
	std::sort(ty.begin(), ty.end());
	std::vector<float> qt, qr, ft, fr;
	for (int i = 0; i < 50; ++i) {
		const float q = 0.01f + 0.02f * (float)i;
		qt.push_back(percentile(ty, q));
		qr.push_back(percentile(ry, q));
		// The fit leaves out the darkest and brightest few percent, where clipping lives
		if (q >= 0.05f && q <= 0.95f && qt.back() > 0.02f && qt.back() < 0.97f && qr.back() < 0.97f) {
			ft.push_back(qt.back());
			fr.push_back(qr.back());
		}
	}
	ToneFit tone;
	if (ft.size() >= 8)
		tone = fitTone(ft, fr);
	for (int i = 0; i < kCurvePoints; ++i) {
		const float x = (float)i / (kCurvePoints - 1);
		float v = tone.at(x), m;
		if (ft.size() >= 8 && mappedAt(qt, qr, x, m))
			v += clampf(m - v, -0.06f, 0.06f) * 0.5f;
		P.curve[i] = clampf(v, 0.0f, 1.0f);
	}
	P.curve[0] = clampf(P.curve[0], 0.0f, 0.1f);
	for (int i = 1; i < kCurvePoints; ++i)
		P.curve[i] = std::min(1.0f, std::max(P.curve[i], P.curve[i - 1] + 0.002f));
	if (percentile(ry, 0.995f) > 0.97f && percentile(ty, 0.995f) > 0.97f)
		P.curve[kCurvePoints - 1] = 1.0f; // both clip: keep white white

	/* 5. Saturation */
	float table[256];
	curveTable(P.curve, table);
	auto chroma70 = [&](const std::vector<Px> &v, bool corrected, Hist &h) {
		std::vector<float> c;
		for (const Px &p : v) {
			float lin[3];
			if (corrected)
				correct(p, P, table, lin);
			else
				std::copy(p.l, p.l + 3, lin);
			h.add(lin);
			if (!usable(p))
				continue;
			float L, a, b;
			oklab(lin, L, a, b);
			c.push_back(std::hypot(a, b));
		}
		std::sort(c.begin(), c.end());
		return percentile(c, 0.7f);
	};
	{
		Hist hr, ht;
		const float cr = chroma70(rp, false, hr), ct = chroma70(tp, true, ht);
		if (cr > 0.01f && ct > 0.01f) {
			const float s = clampf(cr / ct, 0.7f, 1.4f);
			P.saturation = 1.0f + 0.8f * (s - 1.0f);
		}
	}

	/* 6. Confidence, with the corrected target compared to the reference */
	Hist hr, ht;
	chroma70(rp, false, hr);
	chroma70(tp, true, ht);
	const float similarity = (float)(0.5 * Hist::intersect(hr.ab, ht.ab) + 0.5 * Hist::intersect(hr.lum, ht.lum));

	struct Reason {
		int penalty;
		std::string text;
	};
	std::vector<Reason> reasons;
	const float neutral = std::min(rc.fromNeutrals ? rc.fraction : 0.0f, tc.fromNeutrals ? tc.fraction : 0.0f);
	if (std::min(rUsable, tUsable) < 0.4f)
		reasons.push_back({20, "much of the picture is blown out or too dark"});
	if (neutral < 0.01f)
		reasons.push_back({25, "there's very little white, gray or black in the pictures to judge color by"});
	else if (neutral < 0.03f)
		reasons.push_back({12, "there's only a little white, gray or black in the pictures to judge color by"});
	if (similarity < 0.2f)
		reasons.push_back({50, "the two cameras see very different things"});
	else if (similarity < 0.35f)
		reasons.push_back({35, "the two cameras see very different things"});
	else if (similarity < 0.5f)
		reasons.push_back({18, "the two cameras see fairly different things"});
	if (spread > 0.05f)
		reasons.push_back({20, "the picture kept changing while it was measured"});
	else if (spread > 0.03f)
		reasons.push_back({8, "the picture changed a little while it was measured"});
	if (gainLimited || std::max(std::abs(std::log(P.gain[0])), std::abs(std::log(P.gain[2]))) > 0.35f)
		reasons.push_back({15, "the color difference is unusually large"});
	if (tone.limited)
		reasons.push_back({10, "the brightness difference is unusually large"});
	if (skin < 0)
		reasons.push_back({5, "skin tones don't agree with the rest of the picture"});
	int conf = 100 + (skin > 0 ? 5 : 0);
	for (const Reason &r : reasons)
		conf -= r.penalty;
	res.confidence = std::clamp(conf, 0, 100);

	res.details = fmt("gains %.3f %.3f %.3f", P.gain[0], P.gain[1], P.gain[2]) +
		      fmt(", tone a=%.3f b=%.3f gamma=%.2f", tone.a, tone.b, tone.g) +
		      fmt(", saturation %.2f", P.saturation) +
		      fmt(", neutrals %.1f%%/%.1f%%", rc.fraction * 100, tc.fraction * 100) +
		      fmt(", similarity %.2f, spread %.3f", similarity, spread) +
		      (skin > 0   ? ", skin used"
		       : skin < 0 ? ", skin disagreed"
				  : "");

	res.ok = res.confidence >= kMinConfidence;
	if (res.ok) {
		res.code = "OK";
		res.message =
			fmt("Matched (confidence %.0f).", res.confidence) + (skin > 0 ? " Skin tones helped." : "");
	} else {
		const Reason *worst = nullptr;
		for (const Reason &r : reasons)
			if (!worst || r.penalty > worst->penalty)
				worst = &r;
		res.code = "LOW_CONFIDENCE";
		res.message = fmt("Not confident enough (%.0f): ", res.confidence) +
			      (worst ? worst->text : "the measurement wasn't reliable") +
			      ". The colors weren't changed.";
	}
	return res;
}

} // namespace cm
