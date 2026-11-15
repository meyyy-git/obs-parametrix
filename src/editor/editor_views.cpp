// Module views: EQ curve and band panel, compressor, multiband, output, Simple mode.
#include "editor_p.hpp"

#include <QLineF>
#include <QPainterPath>

#include <array>
#include <complex>

#include <cmath>

namespace px {

namespace {
QString qs(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}
bool gainType(int t)
{
	return t == EQT_BELL || t == EQT_LOWSHELF || t == EQT_HIGHSHELF || t == EQT_TILT || t == EQT_FLATTILT;
}
const char *kTypeName[] = {"Bell",  "Low shelf", "High shelf", "Low cut",  "High cut",
			   "Notch", "Band pass", "Tilt shelf", "Flat tilt"};
const char *kTypeShort[] = {"BL", "LS", "HS", "LC", "HC", "NT", "BP", "TL", "FT"};
const double kFMin = 20, kFMax = 20000;
} // namespace

void Editor::bandParams(const std::string &id, int b, double *out) const
{
	const ModInfo *m = info(id);
	for (int k = 0; k < kEqBandParams; k++) {
		const ParamDef &pd = m->mt->params[(size_t)(kEqFirstBand + b * kEqBandParams + k)];
		out[k] = pd.kind == Kind::Source ? -1 : val(id, pd.key);
	}
}

void Editor::updateSpectrum()
{
	auto mod = host->module(selId);
	if (!mod)
		return;
	static thread_local std::vector<std::complex<float>> x(2048);
	float buf[2048];
	for (int which = 0; which < 2; which++) {
		if (mod->tap(which, buf, 2048) != 2048)
			return;
		for (int i = 0; i < 2048; i++)
			x[(size_t)i] = buf[i] * (float)(0.5 - 0.5 * std::cos(2 * kPi * i / 2048));
		fft.run(x.data(), false);
		std::vector<float> &s = which ? specPost : specPre;
		for (int k = 0; k <= 1024; k++) {
			// a full-scale sine reads 0 dB through a Hann window
			const float db = (float)(20.0 * std::log10(std::abs(x[(size_t)k]) / 512.0 + 1e-9));
			s[(size_t)k] += (db - s[(size_t)k]) * (db > s[(size_t)k] ? 0.6f : 0.12f);
		}
	}
}

// ------------------------------------------------------------------ EQ
void Editor::drawEq(QRectF r)
{
	const ModInfo *m = selected();
	const std::string id = m->id;
	const double sr = host->sampleRate();
	const QRectF canvas(r.left(), r.top(), r.width(), r.height() - 40);
	const QRectF plot = canvas.adjusted(34, 14, -40, -22);
	auto fx = [plot](double f) {
		return plot.left() + plot.width() * std::log(f / kFMin) / std::log(kFMax / kFMin);
	};
	auto xf = [plot](double x) {
		return kFMin * std::pow(kFMax / kFMin, (x - plot.left()) / plot.width());
	};
	const int rng = range;
	auto ey = [plot, rng](double d) {
		return plot.top() + (1 - (d + rng) / (2.0 * rng)) * plot.height();
	};
	auto ye = [plot, rng](double y) {
		return (1 - (y - plot.top()) / plot.height()) * 2.0 * rng - rng;
	};

	gridFreq(plot);
	const int stepDb = range == 30 ? 10 : range == 12 ? 3 : 2;
	for (int d = -range; d <= range; d += stepDb) {
		p->setPen(QPen(d == 0 ? QColor(0x2c, 0x33, 0x3d) : QColor(0x1a, 0x1f, 0x26), 1));
		p->drawLine(QPointF(plot.left(), ey(d)), QPointF(plot.right(), ey(d)));
		text(QRectF(plot.right() + 6, ey(d) - 7, 32, 14), (d > 0 ? "+" : "") + QString::number(d),
		     d == 0 ? col::muted : col::dim, Qt::AlignLeft | Qt::AlignVCenter, 10, true);
	}
	for (int d = -80; d <= -10; d += 20)
		text(QRectF(plot.left() - 34, plot.top() + (-d / 90.0) * plot.height() - 7, 28, 14), QString::number(d),
		     QColor(0x3e, 0x46, 0x51), Qt::AlignRight | Qt::AlignVCenter, 10, true);

	// analyzer
	auto spectrum = [&](const std::vector<float> &s, const QColor &fill, const QColor &stroke) {
		QPainterPath path;
		path.moveTo(plot.left(), plot.bottom());
		QPainterPath lineP;
		bool first = true;
		for (int k = 1; k <= 1024; k++) {
			const double f = k * sr / 2048.0;
			if (f < kFMin || f > kFMax)
				continue;
			const QPointF pt(fx(f), clampd(plot.top() + (-s[(size_t)k] / 90.0) * plot.height(), plot.top(),
						       plot.bottom()));
			path.lineTo(pt);
			first ? lineP.moveTo(pt) : lineP.lineTo(pt);
			first = false;
		}
		path.lineTo(plot.right(), plot.bottom());
		p->fillPath(path, fill);
		if (stroke.alpha()) {
			p->setPen(QPen(stroke, 1));
			p->setBrush(Qt::NoBrush);
			p->drawPath(lineP);
		}
	};
	if (anaMode == 1 || anaMode == 3)
		spectrum(specPre, QColor(139, 149, 164, 36), QColor(0, 0, 0, 0));
	if (anaMode == 2 || anaMode == 3)
		spectrum(specPost, QColor(95, 184, 242, 26), QColor(95, 184, 242, 140));

	// bands
	const int n = (int)val(id, "nbands");
	if (selBand >= n)
		selBand = -1;
	std::vector<std::array<double, kEqBandParams>> bp((size_t)n);
	std::vector<QPointF> nodes((size_t)n);
	for (int b = 0; b < n; b++) {
		bandParams(id, b, bp[(size_t)b].data());
		const double *q = bp[(size_t)b].data();
		nodes[(size_t)b] = QPointF(fx(clampd(q[EQ_FREQ], kFMin, kFMax)),
					   ey(gainType((int)q[EQ_TYPE]) ? clampd(q[EQ_GAIN], -range, range) : 0));
	}
	int hoverBand = -1;
	if (hovered("eq.canvas") && !dragging)
		for (int b = 0; b < n; b++)
			if (QLineF(nodes[(size_t)b], lastPos).length() < 12)
				hoverBand = b;

	auto bandPath = [&](int b, double gain, bool closed) {
		QPainterPath path;
		const double *q = bp[(size_t)b].data();
		if (closed)
			path.moveTo(plot.left(), ey(0));
		for (double x = plot.left(); x <= plot.right(); x += 2) {
			const QPointF pt(x, ey(clampd(eqBandDb(q, gain, sr, xf(x)), -range, range)));
			(closed || x > plot.left()) ? path.lineTo(pt) : path.moveTo(pt);
		}
		if (closed)
			path.lineTo(plot.right(), ey(0));
		return path;
	};
	for (int b = 0; b < n; b++) {
		const double *q = bp[(size_t)b].data();
		if (q[EQ_ON] < 0.5)
			continue;
		const QColor c = col::band[b % 8];
		if (q[EQ_DYN_ON] > 0.5 && gainType((int)q[EQ_TYPE])) { // where the band moves to at full range
			QColor dc = c;
			dc.setAlpha(150);
			p->setPen(QPen(dc, 1, Qt::DashLine));
			p->setBrush(Qt::NoBrush);
			p->drawPath(bandPath(b, q[EQ_GAIN] + q[EQ_DYN_RANGE], false));
		}
		if (b == selBand || b == hoverBand) {
			QColor fc = c;
			fc.setAlpha(46);
			p->fillPath(bandPath(b, q[EQ_GAIN], true), fc);
			fc.setAlpha(200);
			p->setPen(QPen(fc, 1.25));
			p->setBrush(Qt::NoBrush);
			p->drawPath(bandPath(b, q[EQ_GAIN], false));
		}
	}
	// the sum
	{
		QPainterPath path;
		for (double x = plot.left(); x <= plot.right(); x += 1.5) {
			double d = 0;
			for (int b = 0; b < n; b++)
				if (bp[(size_t)b][EQ_ON] > 0.5)
					d += eqBandDb(bp[(size_t)b].data(), bp[(size_t)b][EQ_GAIN], sr, xf(x));
			const QPointF pt(x, ey(clampd(d, -range, range)));
			x > plot.left() ? path.lineTo(pt) : path.moveTo(pt);
		}
		p->setPen(QPen(col::fg, 2));
		p->setBrush(Qt::NoBrush);
		p->drawPath(path);
	}
	// nodes
	for (int b = 0; b < n; b++) {
		const bool on = bp[(size_t)b][EQ_ON] > 0.5;
		const QColor c = col::band[b % 8];
		const double rad = b == selBand ? 10 : 8;
		p->setPen(on ? Qt::NoPen : QPen(c, 1.5));
		p->setBrush(on ? c : col::scope);
		p->drawEllipse(nodes[(size_t)b], rad, rad);
		if (b == selBand) {
			p->setPen(QPen(col::fg, 2));
			p->setBrush(Qt::NoBrush);
			p->drawEllipse(nodes[(size_t)b], rad + 3, rad + 3);
		}
		text(QRectF(nodes[(size_t)b].x() - 10, nodes[(size_t)b].y() - 8, 20, 16), QString::number(b + 1),
		     on ? col::scope : c, Qt::AlignCenter, 10, true, QFont::Medium);
	}
	text(QRectF(plot.left(), canvas.top() + 2, plot.width(), 14),
	     "Double-click to add · drag to move · scroll for Q · Alt-click to bypass · Shift for fine · Esc closes the panel",
	     col::muted, Qt::AlignCenter, 11);

	// canvas interaction
	Hit &h = hit(canvas, "eq.canvas");
	h.cursor = hoverBand >= 0 ? Qt::OpenHandCursor : Qt::CrossCursor;
	auto nearest = [nodes](QPointF pt) {
		int best = -1;
		double bd = 14;
		for (size_t b = 0; b < nodes.size(); b++) {
			const double d = QLineF(nodes[b], pt).length();
			if (d < bd) {
				bd = d;
				best = (int)b;
			}
		}
		return best;
	};
	h.press = [this, id, nearest, bp](QPointF pt, Qt::KeyboardModifiers mods) {
		const int b = nearest(pt);
		dragBand = -1;
		selBand = b;
		if (b < 0)
			return;
		if (mods & Qt::AltModifier) {
			setVal(id, bandKey(b, "on"), bp[(size_t)b][EQ_ON] > 0.5 ? 0 : 1);
			commit();
			return;
		}
		dragBand = b;
		dragF0 = bp[(size_t)b][EQ_FREQ];
		dragG0 = bp[(size_t)b][EQ_GAIN];
	};
	h.drag = [this, id, fx, xf, ey, ye, bp](QPointF pt, Qt::KeyboardModifiers mods) {
		if (dragBand < 0)
			return;
		const double k = mods & Qt::ShiftModifier ? 0.15 : 1.0;
		setVal(id, bandKey(dragBand, "freq_hz"),
		       clampd(xf(fx(dragF0) + (pt.x() - pressPos.x()) * k), kFMin, kFMax));
		if (gainType((int)bp[(size_t)dragBand][EQ_TYPE]))
			setVal(id, bandKey(dragBand, "gain_db"),
			       std::round(clampd(ye(ey(clampd(dragG0, -range, range)) + (pt.y() - pressPos.y()) * k),
						 -30, 30) *
					  10) /
				       10);
	};
	h.release = [this] {
		if (dragBand >= 0)
			commit();
		dragBand = -1;
	};
	h.wheel = [this, id, nearest, bp](int d, Qt::KeyboardModifiers mods, QPointF pt) {
		int b = nearest(pt);
		if (b < 0)
			b = selBand;
		if (b < 0)
			return;
		selBand = b;
		setVal(id, bandKey(b, "q"), bp[(size_t)b][EQ_Q] * std::pow(mods & Qt::ShiftModifier ? 1.02 : 1.12, d));
		commit();
	};
	h.dbl = [this, id, nearest, xf, ye, n, plot](QPointF pt, Qt::KeyboardModifiers) {
		if (nearest(pt) >= 0 || !plot.contains(pt) || !addEqBand(host->store(), id))
			return;
		const double f = clampd(xf(pt.x()), kFMin, kFMax);
		const int type = f < 40 ? EQT_LOWCUT : f > 15000 ? EQT_HIGHCUT : EQT_BELL;
		setVal(id, bandKey(n, "type"), type);
		setVal(id, bandKey(n, "freq_hz"), f);
		setVal(id, bandKey(n, "gain_db"),
		       type == EQT_BELL ? std::round(clampd(ye(pt.y()), -30, 30) * 10) / 10 : 0);
		setVal(id, bandKey(n, "q"), type == EQT_BELL ? 1.0 : 0.7071);
		setVal(id, bandKey(n, "slope"), type == EQT_BELL ? 1 : 3);
		selBand = n;
		commit();
	};

	// readout under the pointer
	if (hovered("eq.canvas") && plot.contains(lastPos) && !dragging) {
		const double f = xf(lastPos.x());
		double d = 0;
		for (int b = 0; b < n; b++)
			if (bp[(size_t)b][EQ_ON] > 0.5)
				d += eqBandDb(bp[(size_t)b].data(), bp[(size_t)b][EQ_GAIN], sr, f);
		const QString s = QString("%1   %2%3 dB").arg(fmtHz(f)).arg(d > 0 ? "+" : "").arg(d, 0, 'f', 1);
		const double w = textW(s, 11, true) + 14;
		const QRectF tr(std::min(lastPos.x() + 14, plot.right() - w),
				std::max(lastPos.y() - 30, plot.top() + 16), w, 22);
		box(tr, col::panel, col::line2, 4);
		text(tr, s, col::fg, Qt::AlignCenter, 11, true);
	}

	if (selBand >= 0) {
		selNodeY = nodes[(size_t)selBand].y();
		drawBandPanel(canvas);
	}

	// band chips
	const double cy = r.bottom() - 34;
	p->setPen(col::line);
	p->drawLine(QPointF(r.left(), r.bottom() - 39.5), QPointF(r.right(), r.bottom() - 39.5));
	p->fillRect(QRectF(r.left(), r.bottom() - 39, r.width(), 39), col::win);
	double x = r.left() + 12;
	for (int b = 0; b < n && x < r.right() - 190; b++) {
		const double *q = bp[(size_t)b].data();
		const bool on = q[EQ_ON] > 0.5, dyn = q[EQ_DYN_ON] > 0.5;
		const QString label = QString("%1 %2 %3")
					      .arg(b + 1)
					      .arg(kTypeShort[(int)q[EQ_TYPE]])
					      .arg(fmtHz(q[EQ_FREQ]).remove(' '));
		const double w = textW(label, 12, true) + 30 + (dyn ? 32 : 0);
		const std::string hid = "chip." + std::to_string(b);
		const QColor c = col::band[b % 8];
		box(QRectF(x, cy, w, 28), b == selBand ? col::raised : col::scope,
		    b == selBand ? c : (hovered(hid) ? col::muted : col::line2));
		p->setPen(on ? Qt::NoPen : QPen(c, 1.5));
		p->setBrush(on ? c : QColor(0, 0, 0, 0));
		p->drawEllipse(QPointF(x + 13, cy + 14), 5, 5);
		text(QRectF(x + 24, cy, w - 26, 28), label, on ? col::fg : col::dim, Qt::AlignLeft | Qt::AlignVCenter,
		     12, true);
		if (dyn)
			text(QRectF(x + w - 36, cy, 30, 28), "DYN", col::accent, Qt::AlignRight | Qt::AlignVCenter, 11,
			     true);
		Hit &hc = hit(QRectF(x, cy, w, 28), hid);
		hc.cursor = Qt::PointingHandCursor;
		hc.press = [this, b](QPointF, Qt::KeyboardModifiers) {
			selBand = selBand == b ? -1 : b;
		};
		x += w + 6;
	}
	iconButton(
		x, cy, "plus", false,
		[this, id, n, bp] {
			if (!addEqBand(host->store(), id))
				return;
			double f = 1000;
			for (double c : {1000.0, 500.0, 2000.0, 250.0, 4000.0, 125.0, 8000.0}) {
				bool used = false;
				for (const auto &q : bp)
					used = used || std::fabs(std::log2(q[EQ_FREQ] / c)) < 0.3;
				if (!used) {
					f = c;
					break;
				}
			}
			setVal(id, bandKey(n, "freq_hz"), f);
			selBand = n;
			commit();
		},
		"band.add", "Add band", n < kEqBands);
	text(QRectF(r.right() - 150, cy, 138, 28), QString("%1/%2 bands").arg(n).arg(kEqBands), col::muted,
	     Qt::AlignRight | Qt::AlignVCenter, 12, true);
}

void Editor::drawBandPanel(QRectF area)
{
	const ModInfo *m = selected();
	const std::string id = m->id;
	const int b = selBand;
	double q[kEqBandParams];
	bandParams(id, b, q);
	const int type = (int)q[EQ_TYPE];
	const bool gain = gainType(type), cut = type == EQT_LOWCUT || type == EQT_HIGHCUT,
		   dyn = q[EQ_DYN_ON] > 0.5 && gain;
	const QColor c = col::band[b % 8];
	const double w = std::min(area.width() - 24, 690.0);
	const double hgt = bandMin ? 48 : 48 + 96 + (dyn ? 100 : 0);
	// The panel sits at the bottom, and moves to the top when it would cover the band it edits.
	const double bottomY = area.bottom() - 30 - hgt;
	const bool covers = selNodeY > bottomY - 14 && dragBand < 0;
	static bool top = false;
	if (dragBand < 0)
		top = covers;
	const QRectF r(area.center().x() - w / 2, top ? area.top() + 22 : bottomY, w, hgt);
	p->fillRect(r.adjusted(0, 6, 0, 10), QColor(0, 0, 0, 70)); // soft drop shadow
	box(r, col::panel, col::line2, 10);
	hit(r, "bp.bg").press = [](QPointF, Qt::KeyboardModifiers) {
	}; // clicks on the panel don't reach the curve

	auto def = [&](const char *k) -> const ParamDef & {
		return m->mt->params[(size_t)m->mt->index(bandKey(b, k))];
	};
	const double y = r.top() + 10;
	double x = r.left() + 12;
	p->setPen(Qt::NoPen);
	p->setBrush(c);
	p->drawEllipse(QPointF(x + 11, y + 14), 11, 11);
	text(QRectF(x, y + 3, 22, 22), QString::number(b + 1), col::scope, Qt::AlignCenter, 11, true, QFont::Medium);
	x += 30;
	text(QRectF(x, y, 70, 28), kTypeName[type], col::fg, Qt::AlignLeft | Qt::AlignVCenter, 12, false,
	     QFont::Medium);
	x += 68;

	// right-hand buttons, laid out from the right edge
	double rx = r.right() - 12 - 28;
	iconButton(rx, y, "x", false, [this] { selBand = -1; }, "bp.close", "Close panel (Esc)");
	rx -= 32;
	iconButton(
		rx, y, bandMin ? "up" : "down", false, [this] { bandMin = !bandMin; }, "bp.min",
		bandMin ? "Expand panel" : "Minimize panel");
	rx -= 10;
	p->setPen(col::line2);
	p->drawLine(QPointF(rx + 3, y + 4), QPointF(rx + 3, y + 24));
	rx -= 30;
	iconButton(
		rx, y, "trash", false,
		[this, id, b] {
			if (removeEqBand(host->store(), id, b)) {
				selBand = -1;
				host->changed();
				commit();
			}
		},
		"bp.del", "Delete band (Del)");
	const bool on = q[EQ_ON] > 0.5, solo = q[EQ_SOLO] > 0.5;
	const double bw = textW("Bypass", 12, false, QFont::Medium) + 20,
		     sw = textW("Solo", 12, false, QFont::Medium) + 20;
	rx -= bw + 4;
	button(
		rx, y, "Bypass", !on,
		[this, id, b, on] {
			setVal(id, bandKey(b, "on"), on ? 0 : 1);
			commit();
		},
		"bp.byp");
	rx -= sw + 4;
	button(
		rx, y, "Solo", solo,
		[this, id, b, solo] {
			const int n = (int)val(id, "nbands");
			for (int k = 0; k < n; k++)
				setVal(id, bandKey(k, "solo"), k == b && !solo ? 1 : 0);
			commit();
		},
		"bp.solo");

	if (bandMin) {
		QString s = fmtHz(q[EQ_FREQ]);
		if (gain)
			s += QString(" · %1%2 dB").arg(q[EQ_GAIN] > 0 ? "+" : "").arg(q[EQ_GAIN], 0, 'f', 1);
		s += cut ? QString(" · %1 dB/oct").arg(kEqSlopes[(int)q[EQ_SLOPE]])
			 : (type == EQT_FLATTILT ? QString() : QString(" · Q %1").arg(q[EQ_Q], 0, 'f', 2));
		if (dyn)
			s += " · DYN";
		text(QRectF(x, y, rx - x - 8, 28), s, col::muted, Qt::AlignLeft | Qt::AlignVCenter, 12, true);
		return;
	}
	if (gain) {
		const double dw = textW("Dynamic", 12, false, QFont::Medium) + 20;
		rx -= dw + 4;
		button(
			rx, y, "Dynamic", dyn,
			[this, id, b, dyn] {
				setVal(id, bandKey(b, "dyn_on"), dyn ? 0 : 1);
				commit();
			},
			"bp.dyn");
	}
	// filter shapes, drawn from the real response of each type
	if (rx - x > 9 * 30 + 12) {
		box(QRectF(x, y, 9 * 30 + 4, 28), col::scope, col::line);
		for (int t = 0; t < 9; t++) {
			const std::string hid = "shape." + std::to_string(t);
			const QRectF sr(x + 2 + t * 30, y + 2, 30, 24);
			if (t == type)
				box(sr, col::raised, QColor(0, 0, 0, 0), 4);
			double sp[kEqBandParams] = {};
			sp[EQ_TYPE] = t;
			sp[EQ_FREQ] = t == EQT_LOWSHELF ? 250 : t == EQT_HIGHSHELF ? 4000 : 1000;
			sp[EQ_Q] = t == EQT_NOTCH ? 2 : 1;
			sp[EQ_SLOPE] = 3;
			QPainterPath path;
			for (int i = 0; i <= 24; i++) {
				const double f = kFMin * std::pow(kFMax / kFMin, i / 24.0);
				const QPointF pt(sr.left() + 3 + i,
						 sr.center().y() - clampd(eqBandDb(sp, 9, 48000, f), -12, 12) * 0.55);
				i ? path.lineTo(pt) : path.moveTo(pt);
			}
			p->setPen(QPen(t == type ? c : (hovered(hid) ? col::fg : col::muted), 1.5));
			p->setBrush(Qt::NoBrush);
			p->drawPath(path);
			Hit &hs = hit(sr, hid);
			hs.cursor = Qt::PointingHandCursor;
			hs.tip = kTypeName[t];
			hs.press = [this, id, b, t](QPointF, Qt::KeyboardModifiers) {
				setVal(id, bandKey(b, "type"), t);
				if (!gainType(t))
					setVal(id, bandKey(b, "dyn_on"), 0);
				if ((t == EQT_LOWCUT || t == EQT_HIGHCUT) && val(id, bandKey(b, "q")) > 2)
					setVal(id, bandKey(b, "q"), 0.7071);
				commit();
			};
		}
	}

	// knobs
	const double ky = r.top() + 50;
	x = r.left() + 14;
	x = paramKnob(x, ky, id, def("freq_hz"), c, "Freq");
	if (gain)
		x = paramKnob(x, ky, id, def("gain_db"), c, "Gain");
	if (type != EQT_FLATTILT)
		x = paramKnob(x, ky, id, def("q"), c, "Q");
	x += 10;
	if (cut) {
		text(QRectF(x, ky, 120, 16), "Slope", col::muted, Qt::AlignLeft | Qt::AlignVCenter, 12);
		QStringList opts;
		for (int s : kEqSlopes)
			opts << QString("%1 dB/oct").arg(s);
		dropdown(
			x, ky + 26, 112, opts[(int)q[EQ_SLOPE]], opts,
			[this, id, b](int i) {
				setVal(id, bandKey(b, "slope"), i);
				commit();
			},
			"bp.slope");
		x += 124;
	}
	if (host->channels() == 2) {
		text(QRectF(x, ky, 120, 16), "Placement", col::muted, Qt::AlignLeft | Qt::AlignVCenter, 12);
		seg(
			x, ky + 26, {"Stereo", "L", "R", "M", "S"}, (int)q[EQ_PLACE],
			[this, id, b](int i) {
				setVal(id, bandKey(b, "placement"), i);
				commit();
			},
			"bp.place");
	}
	if (!dyn)
		return;
	const double dy = ky + 96;
	p->setPen(col::line);
	p->drawLine(QPointF(r.left() + 12, dy - 6), QPointF(r.right() - 12, dy - 6));
	x = r.left() + 14;
	x = paramKnob(x, dy, id, def("dyn_threshold_db"), c, "Threshold");
	x = paramKnob(x, dy, id, def("dyn_range_db"), c, "Range");
	x = paramKnob(x, dy, id, def("dyn_attack_ms"), c, "Attack");
	x = paramKnob(x, dy, id, def("dyn_release_ms"), c, "Release");
	x += 10;
	text(QRectF(x, dy, 120, 16), "Sidechain", col::muted, Qt::AlignLeft | Qt::AlignVCenter, 12);
	QStringList sources{"Internal"};
	for (const std::string &s : host->audioSources())
		sources << qs(s);
	const std::string cur = str(id, bandKey(b, "dyn_sidechain"));
	dropdown(
		x, dy + 26, 150, cur.empty() ? QStringLiteral("Internal") : qs(cur), sources,
		[this, id, b, sources](int i) {
			setStr(id, bandKey(b, "dyn_sidechain"), i == 0 ? "" : sources[i].toStdString());
			commit();
		},
		"bp.sc");
}

// ------------------------------------------------------------------ compressor
void Editor::drawComp(QRectF r)
{
	const ModInfo *m = selected();
	const std::string id = m->id;
	const double s = std::min(230.0, r.height() - 150);
	const QRectF g(r.left() + 14, r.top() + 14, s, s);
	p->setPen(QPen(QColor(0x1a, 0x1f, 0x26), 1));
	for (int i = 0; i <= 6; i++) {
		p->drawLine(QPointF(g.left(), g.top() + s * i / 6), QPointF(g.right(), g.top() + s * i / 6));
		p->drawLine(QPointF(g.left() + s * i / 6, g.top()), QPointF(g.left() + s * i / 6, g.bottom()));
	}
	auto tx = [g, s](double d) {
		return g.left() + (d + 60) / 60 * s;
	};
	auto ty = [g, s](double d) {
		return g.top() + (-d / 60) * s;
	};
	p->setPen(QPen(col::line2, 1));
	p->drawLine(QPointF(tx(-60), ty(-60)), QPointF(tx(0), ty(0)));
	const double th = val(id, "threshold_db"), ratio = val(id, "ratio"), knee = val(id, "knee_db");
	const double mk = val(id, "auto_makeup") != 0 ? -compGainDb(0, th, ratio, knee) * 0.5 : val(id, "makeup_db");
	QPainterPath path;
	for (double d = -60; d <= 0; d += 0.5) {
		const QPointF pt(tx(d), ty(clampd(d + compGainDb(d, th, ratio, knee) + mk, -60, 0)));
		d > -60 ? path.lineTo(pt) : path.moveTo(pt);
	}
	p->setPen(QPen(col::fg, 2));
	p->setBrush(Qt::NoBrush);
	p->drawPath(path);
	QColor ac = col::accent;
	ac.setAlpha(140);
	p->setPen(QPen(ac, 1, Qt::DashLine));
	p->drawLine(QPointF(tx(th), g.top()), QPointF(tx(th), g.bottom()));
	text(QRectF(g.left() + 8, g.top() + 6, s, 14), "Input → output", col::muted, Qt::AlignLeft | Qt::AlignVCenter,
	     11, true);

	const std::deque<float> &h = hist[id];
	history(QRectF(g.right() + 16, g.top(), r.right() - g.right() - 30, s), h, 24, col::accent,
		QString("Gain reduction · %1 dB").arg(h.empty() ? 0.0 : h.back(), 0, 'f', 1));

	double x = r.left() + 16, y = g.bottom() + 20;
	for (const ParamDef &pd : m->mt->params) {
		if (pd.kind != Kind::Float)
			continue;
		if (x + 74 > r.right()) {
			x = r.left() + 16;
			y += 92;
		}
		x = paramKnob(x, y, id, pd, col::accent);
	}
}

// ------------------------------------------------------------------ multiband
void Editor::drawMb(QRectF r)
{
	static const char *names[kMbBands] = {"Low", "Low-mid", "High-mid", "High"};
	static const char *xkeys[3] = {"x1_hz", "x2_hz", "x3_hz"};
	const ModInfo *m = selected();
	const std::string id = m->id;
	auto mod = host->module(id);
	const QRectF canvas(r.left(), r.top(), r.width(), std::min(260.0, r.height() - 190));
	const QRectF plot = canvas.adjusted(34, 14, -34, -22);
	auto fx = [plot](double f) {
		return plot.left() + plot.width() * std::log(f / kFMin) / std::log(kFMax / kFMin);
	};
	auto xf = [plot](double x) {
		return kFMin * std::pow(kFMax / kFMin, (x - plot.left()) / plot.width());
	};
	gridFreq(plot);
	const double xs[5] = {kFMin, val(id, "x1_hz"), val(id, "x2_hz"), val(id, "x3_hz"), kFMax};
	bool anySolo = false, solo[kMbBands];
	double gr[kMbBands];
	for (int b = 0; b < kMbBands; b++) {
		solo[b] = val(id, "b" + std::to_string(b) + ".solo") != 0;
		anySolo = anySolo || solo[b];
		gr[b] = mod && m->on && host->enabled() ? mod->aux[b].load() : 0;
	}
	for (int b = 0; b < kMbBands; b++) {
		const double x0 = fx(xs[b]), x1 = fx(xs[b + 1]);
		QColor c = col::mb[b];
		p->setOpacity(anySolo && !solo[b] ? 0.25 : 1.0);
		c.setAlpha(b == mbSel ? 38 : 16);
		p->fillRect(QRectF(x0, plot.top(), x1 - x0, plot.height()), c);
		if (b == mbSel)
			p->fillRect(QRectF(x0, plot.top(), x1 - x0, 2), col::mb[b]);
		const double gh = clampd(-gr[b] / 24, 0, 1) * plot.height() * 0.7;
		c.setAlpha(150);
		p->fillRect(QRectF(x0 + 6, plot.top() + 2, std::max(0.0, x1 - x0 - 12), gh), c);
		if (x1 - x0 > 60)
			text(QRectF(x0 + 8, plot.bottom() - 20, x1 - x0 - 10, 16),
			     QString(names[b]) + (solo[b] ? " · solo" : ""), col::mb[b],
			     Qt::AlignLeft | Qt::AlignVCenter, 12, false, QFont::Medium);
		if (x1 - x0 > 50)
			text(QRectF(x0, plot.top() + std::max(gh + 4, 22.0), x1 - x0, 14),
			     QString("%1 dB").arg(gr[b] > -0.05 ? 0.0 : gr[b], 0, 'f', 1), col::fg, Qt::AlignCenter, 10,
			     true);
		p->setOpacity(1.0);
	}
	int hoverX = -1;
	for (int k = 0; k < 3; k++)
		if (hovered("mb.canvas") && std::fabs(fx(xs[k + 1]) - lastPos.x()) < 7)
			hoverX = k;
	for (int k = 0; k < 3; k++) {
		const double x = fx(xs[k + 1]);
		const bool hot = k == hoverX || (dragging && activeId == "mb.canvas" && dragX == k);
		p->setPen(QPen(hot ? col::fg : QColor(226, 230, 236, 140), hot ? 2 : 1.25));
		p->drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
		box(QRectF(x - 4, plot.center().y() - 14, 8, 28), hot ? col::fg : col::muted, QColor(0, 0, 0, 0), 4);
		const bool flip = x > plot.right() - 70;
		text(QRectF(flip ? x - 76 : x + 6, plot.top() + 4, 70, 14), fmtHz(xs[k + 1]), col::fg,
		     (flip ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, 10, true);
	}
	Hit &h = hit(canvas, "mb.canvas");
	h.cursor = hoverX >= 0 ? Qt::SizeHorCursor : Qt::PointingHandCursor;
	const std::array<double, 5> xa = {xs[0], xs[1], xs[2], xs[3], xs[4]};
	h.press = [this, fx, xf, xa](QPointF pt, Qt::KeyboardModifiers) {
		dragX = -1;
		for (int k = 0; k < 3; k++)
			if (std::fabs(fx(xa[(size_t)k + 1]) - pt.x()) < 7)
				dragX = k;
		if (dragX < 0)
			for (int b = 0; b < kMbBands; b++)
				if (xf(pt.x()) >= xa[(size_t)b] && xf(pt.x()) < xa[(size_t)b + 1])
					mbSel = b;
	};
	h.drag = [this, id, xf, xa](QPointF pt, Qt::KeyboardModifiers) {
		if (dragX < 0)
			return;
		// crossovers stay at least a third of an octave apart
		const double lo = dragX == 0 ? 30 : xa[(size_t)dragX] * 1.3,
			     hi = dragX == 2 ? 16000 : xa[(size_t)dragX + 2] / 1.3;
		setVal(id, xkeys[dragX], clampd(xf(pt.x()), lo, hi));
	};
	h.release = [this] {
		if (dragX >= 0)
			commit();
		dragX = -1;
	};

	// band cards
	const double cw = (r.width() - 32 - 3 * 8) / 4, cy = canvas.bottom() + 12;
	for (int b = 0; b < kMbBands; b++) {
		const double x = r.left() + 16 + b * (cw + 8);
		const std::string hid = "mb.card." + std::to_string(b);
		box(QRectF(x, cy, cw, 58), b == mbSel ? col::raised : col::scope,
		    b == mbSel ? col::mb[b] : (hovered(hid) ? col::muted : col::line2), 8);
		Hit &hc = hit(QRectF(x, cy, cw, 58), hid);
		hc.cursor = Qt::PointingHandCursor;
		hc.press = [this, b](QPointF, Qt::KeyboardModifiers) {
			mbSel = b;
		};
		p->setPen(Qt::NoPen);
		p->setBrush(col::mb[b]);
		p->drawEllipse(QPointF(x + 14, cy + 18), 4, 4);
		const double nw = textW(names[b], 13, false, QFont::Medium);
		text(QRectF(x + 24, cy + 8, nw + 4, 20), names[b], col::fg, Qt::AlignLeft | Qt::AlignVCenter, 13, false,
		     QFont::Medium);
		const double sw = textW("Solo", 12, false, QFont::Medium) + 20;
		const QString span = fmtHz(xs[b]) + " – " + fmtHz(xs[b + 1]);
		if (cw > nw + sw + textW(span, 12, true) + 52) // show the range only when all of it fits
			text(QRectF(x + 32 + nw, cy + 8, cw - nw - sw - 48, 20), span, col::muted,
			     Qt::AlignLeft | Qt::AlignVCenter, 12, true);
		const bool so = solo[b];
		button(
			x + cw - sw - 8, cy + 5, "Solo", so,
			[this, id, b, so] {
				setVal(id, "b" + std::to_string(b) + ".solo", so ? 0 : 1);
				commit();
			},
			"mb.solo." + std::to_string(b));
		const QRectF bar(x + 10, cy + 43, cw - 86, 4);
		box(bar, col::line, QColor(0, 0, 0, 0), 2);
		const double gw = clampd(-gr[b] / 24, 0, 1) * bar.width();
		if (gw > 0.5)
			box(QRectF(bar.left(), bar.top(), gw, 4), col::mb[b], QColor(0, 0, 0, 0), 2);
		text(QRectF(x + cw - 72, cy + 36, 62, 18), QString("%1 dB").arg(gr[b] > -0.05 ? 0.0 : gr[b], 0, 'f', 1),
		     col::muted, Qt::AlignRight | Qt::AlignVCenter, 12, true);
	}

	// knobs of the selected band
	const double ky = cy + 74;
	double x = r.left() + 16;
	p->setPen(Qt::NoPen);
	p->setBrush(col::mb[mbSel]);
	p->drawEllipse(QPointF(x + 4, ky + 20), 4, 4);
	text(QRectF(x, ky + 28, 130, 20), names[mbSel], col::fg, Qt::AlignLeft | Qt::AlignVCenter, 14, false,
	     QFont::DemiBold);
	text(QRectF(x, ky + 48, 150, 18), fmtHz(xs[mbSel]) + " – " + fmtHz(xs[mbSel + 1]), col::muted,
	     Qt::AlignLeft | Qt::AlignVCenter, 12, true);
	x += 150;
	for (int k = 0; k < kMbBandParams - 1; k++)
		x = paramKnob(x, ky, id, m->mt->params[(size_t)(kMbFirstBand + mbSel * kMbBandParams + k)],
			      col::mb[mbSel]);
}

// ------------------------------------------------------------------ output
void Editor::drawOutput(QRectF r)
{
	const ModInfo *m = selected();
	const std::string id = m->id;
	auto mod = host->module(id);
	static const double targets[3] = {-14, -16, -23};
	const double target = targets[(int)clampd(val(id, "lufs_target"), 0, 2)];
	const bool live = mod && host->enabled();
	const double mom = live ? mod->aux[2].load() : -70, integ = live ? mod->aux[3].load() : -70;
	const double pkL = live ? peakHold[0] : -90, pkR = live ? peakHold[1] : -90;
	const QString na = QStringLiteral("—");
	struct Tile {
		QString label, value, unit;
	};
	const Tile tiles[4] = {{"Momentary", mom <= -69.9 ? na : QString::number(mom, 'f', 1), "LUFS"},
			       {"Integrated", integ <= -69.9 ? na : QString::number(integ, 'f', 1),
				integ <= -69.9 ? QString("LUFS")
					       : QString("%1%2 LU from %3")
							 .arg(integ - target > 0 ? "+" : "")
							 .arg(integ - target, 0, 'f', 1)
							 .arg(target)},
			       {"Peak left", pkL <= -89 ? na : QString::number(pkL, 'f', 1), "dBFS"},
			       {"Peak right", pkR <= -89 ? na : QString::number(pkR, 'f', 1), "dBFS"}};
	const double tw = (r.width() - 32 - 3 * 10) / 4;
	for (int i = 0; i < 4; i++) {
		const QRectF t(r.left() + 16 + i * (tw + 10), r.top() + 16, tw, 84);
		box(t, col::win, col::line, 8);
		text(QRectF(t.left() + 12, t.top() + 8, tw - 24, 16), tiles[i].label, col::muted,
		     Qt::AlignLeft | Qt::AlignVCenter, 12, false, QFont::Medium);
		text(QRectF(t.left() + 12, t.top() + 26, tw - 24, 32), tiles[i].value, col::fg,
		     Qt::AlignLeft | Qt::AlignVCenter, 24, true, QFont::Medium);
		text(QRectF(t.left() + 12, t.top() + 60, tw - 24, 16), tiles[i].unit, col::muted,
		     Qt::AlignLeft | Qt::AlignVCenter, 11, true);
	}
	// integrated loudness against the target: the green mark is the target
	const QRectF gauge(r.left() + 16, r.top() + 118, r.width() - 32, 8);
	box(gauge, col::win, col::line, 4);
	p->fillRect(QRectF(gauge.center().x() - 1, gauge.top() - 3, 2, 14), col::ok);
	if (integ > -69.9)
		p->fillRect(QRectF(gauge.left() + clampd(0.5 + (integ - target) / 20, 0, 1) * gauge.width() - 1,
				   gauge.top() - 3, 2, 14),
			    col::fg);

	double x = r.left() + 16;
	const bool stereo = host->channels() == 2;
	for (const ParamDef &pd : m->mt->params) {
		if (pd.kind != Kind::Float || (!stereo && (pd.key == "width" || pd.key == "balance")))
			continue;
		x = paramKnob(x, r.top() + 150, id, pd, col::accent);
	}
}

// ------------------------------------------------------------------ Simple mode
std::string Editor::ensureModule(const std::string &type)
{
	for (const ModInfo &m : mods)
		if (m.mt->type == type)
			return m.id;
	const std::string id = addModule(host->store(), type);
	refreshChain();
	return id;
}

void Editor::applyMacro(const std::string &name, double v)
{
	auto first = [this](const std::string &type) {
		for (const ModInfo &m : mods)
			if (m.mt->type == type)
				return m.id;
		return std::string();
	};
	// first EQ band that satisfies `pred`, as its index or -1
	auto eqBand = [this](const std::string &eq, std::function<bool(const double *)> pred) {
		if (eq.empty())
			return -1;
		const int n = (int)val(eq, "nbands");
		for (int b = 0; b < n; b++) {
			double q[kEqBandParams];
			bandParams(eq, b, q);
			if (pred(q))
				return b;
		}
		return -1;
	};
	const std::string eq = first("eq");
	if (name == "clean") {
		if (!first("gate").empty())
			setVal(first("gate"), "threshold_db", -70 + v * 0.35);
		if (v > 55)
			setVal(ensureModule("denoise"), "reduction_db", (v - 55) * 0.6);
		else if (!first("denoise").empty())
			setVal(first("denoise"), "reduction_db", 0);
	} else if (name == "warmth") {
		const int b = eqBand(eq, [](const double *q) { return (int)q[EQ_TYPE] == EQT_LOWSHELF; });
		if (b >= 0)
			setVal(eq, bandKey(b, "gain_db"), std::round((v - 50) / 50 * 6 * 10) / 10);
		if (!first("sat").empty())
			setVal(first("sat"), "drive_db", v * 0.15);
	} else if (name == "presence") {
		const int b = eqBand(eq, [](const double *q) {
			return (int)q[EQ_TYPE] == EQT_BELL && q[EQ_FREQ] > 1500 && q[EQ_FREQ] < 6000;
		});
		if (b >= 0)
			setVal(eq, bandKey(b, "gain_db"), std::round((v - 50) / 50 * 6 * 10) / 10);
	} else if (name == "deess") {
		const std::string d = first("deess");
		if (!d.empty()) {
			setVal(d, "range_db", -v * 0.16);
			setVal(d, "threshold_db", -14 - v * 0.3);
		}
	} else if (name == "punch") {
		const std::string c = first("comp");
		if (!c.empty()) {
			setVal(c, "ratio", 1.2 + v * 0.07);
			setVal(c, "threshold_db", -8 - v * 0.24);
			setVal(c, "makeup_db", v * 0.08);
		}
	} else if (name == "space") {
		if (v > 0)
			setVal(ensureModule("verb"), "mix", v * 0.4);
		else if (!first("verb").empty())
			setVal(first("verb"), "mix", 0);
	}
}

void Editor::drawSimple(QRectF r)
{
	struct Macro {
		const char *key, *label, *what;
		double def;
	};
	static const Macro macros[6] = {{"clean", "Clean", "Gate · Denoise", 40},
					{"warmth", "Warmth", "EQ low shelf · Saturation", 50},
					{"presence", "Presence", "EQ bell around 3 kHz", 50},
					{"deess", "De-ess", "De-esser", 50},
					{"punch", "Punch", "Compressor", 50},
					{"space", "Space", "Reverb mix", 0}};
	const int cols = r.width() > 860 ? 6 : 3, rows = 6 / cols;
	const double cw = std::min(150.0, (r.width() - 32 - (cols - 1) * 12) / cols), chh = 152;
	const double totalW = cols * cw + (cols - 1) * 12, totalH = rows * chh + (rows - 1) * 12;
	const double x0 = r.center().x() - totalW / 2, y0 = r.center().y() - totalH / 2 - 16;
	for (int i = 0; i < 6; i++) {
		const Macro &mc = macros[i];
		const QRectF c(x0 + (i % cols) * (cw + 12), y0 + (i / cols) * (chh + 12), cw, chh);
		box(c, col::win, col::line, 10);
		const std::string key = std::string("px_macro_") + mc.key, name = mc.key;
		double v = mc.def;
		host->store().getNum(key, v);
		knob(
			c.center().x(), c.top() + 10, mc.label, v, 0, 100, false, false, mc.def,
			QString("%1%").arg(v, 0, 'f', 0), col::accent,
			[this, key, name](double nv) {
				host->store().set(key, Val::num(nv));
				applyMacro(name, nv);
				host->changed();
			},
			"macro." + name, 72);
		text(QRectF(c.left() + 4, c.bottom() - 26, cw - 8, 18), mc.what, col::muted, Qt::AlignCenter, 12);
	}
	text(QRectF(r.left(), y0 + totalH + 14, r.width(), 20),
	     "Each knob moves several modules at once. Clean and Space add the module they need.", col::muted,
	     Qt::AlignCenter, 12);
}

} // namespace px
