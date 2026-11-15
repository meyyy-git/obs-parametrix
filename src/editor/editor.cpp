// Editor framework: state, input, drawing primitives, top bar, rack, header, generic module view.
#include "editor_p.hpp"

#include <QDateTime>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>

#include <cmath>

namespace px {

namespace {
const char *kScaleKey = "px_ui_scale";
bool hasMeter(const std::string &type)
{
	return type == "denoise" || type == "gate" || type == "lvl" || type == "eq" || type == "deess" ||
	       type == "comp" || type == "mb" || type == "lim";
}
QString qs(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}
} // namespace

QWidget *createEditor(std::unique_ptr<EditorHost> host, QWidget *parent)
{
	return new Editor(std::move(host), parent);
}

Editor::Editor(std::unique_ptr<EditorHost> h, QWidget *parent) : QWidget(parent, Qt::Window), host(std::move(h))
{
	setAttribute(Qt::WA_DeleteOnClose);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setWindowTitle(QStringLiteral("Parametrix · ") + qs(host->sourceName()));
	single = host->singleModule();

	uiFont = QFont(QStringList{"IBM Plex Sans Condensed", "Segoe UI", "Helvetica Neue", "Arial"});
	monoFont = QFont(QStringList{"JetBrains Mono", "Cascadia Mono", "Consolas", "Menlo", "monospace"});
	monoFont.setStyleHint(QFont::Monospace);

	double s = 1, w = 1180, hh = 720;
	host->store().getNum(kScaleKey, s);
	host->store().getNum("px_win_w", w);
	host->store().getNum("px_win_h", hh);
	scale = clampd(s, 1.0, 2.0);
	setMinimumSize((int)(900 * scale), (int)(560 * scale));
	resize((int)std::max(w, 900 * scale), (int)std::max(hh, 560 * scale));

	fft.init(2048);
	specPre.assign(1025, -120.0f);
	specPost.assign(1025, -120.0f);

	refreshChain();
	for (size_t i = mods.size(); i-- > 0;)
		if (mods[i].mt->type == "eq")
			selId = mods[i].id;
	{ // hooks for the standalone preview, to open on a given module, band or mode
		std::string want;
		double v = 0;
		if (host->store().getStr("px_debug_select", want))
			for (const ModInfo &m : mods)
				if (m.mt->type == want)
					selId = m.id;
		if (host->store().getNum("px_debug_simple", v))
			simple = v != 0 && !single;
		if (host->store().getNum("px_debug_band", v))
			selBand = (int)v;
	}
	host->store().getStr("px_current", presetName);
	presetBase = captureChain(host->store());
	slotA = slotB = presetBase;
	pushUndo();

	connect(&timer, &QTimer::timeout, this, [this] { tick(); });
	timer.start(33);
}

// ------------------------------------------------------------------ state
void Editor::refreshChain()
{
	mods.clear();
	Store &st = host->store();
	for (const std::string &id : chainIds(st)) {
		std::string type;
		st.getStr(id + ".type", type);
		const ModuleType *mt = findType(type);
		if (!mt)
			continue;
		ModInfo m;
		m.id = id;
		m.mt = mt;
		double on = 1;
		st.getNum(id + ".on", on);
		m.on = on != 0;
		mods.push_back(m);
	}
	if (!info(selId) && !mods.empty())
		selId = mods[std::min<size_t>(1, mods.size() - 1)].id;
}

const ModInfo *Editor::info(const std::string &id) const
{
	for (const ModInfo &m : mods)
		if (m.id == id)
			return &m;
	return nullptr;
}

double Editor::val(const std::string &id, const std::string &key) const
{
	double v = 0;
	if (const ModInfo *m = info(id)) {
		const int i = m->mt->index(key);
		if (i >= 0)
			v = m->mt->params[(size_t)i].def;
	}
	host->store().getNum(id + "." + key, v);
	return v;
}

std::string Editor::str(const std::string &id, const std::string &key) const
{
	std::string s;
	host->store().getStr(id + "." + key, s);
	return s;
}

void Editor::setVal(const std::string &id, const std::string &key, double v)
{
	bool isBool = key == "on";
	if (const ModInfo *m = info(id)) {
		const int i = m->mt->index(key);
		if (i >= 0) {
			const ParamDef &pd = m->mt->params[(size_t)i];
			isBool = pd.kind == Kind::Bool;
			v = clampd(v, pd.min, pd.max);
		}
	}
	host->store().set(id + "." + key, isBool ? Val::boolean(v != 0) : Val::num(v));
	host->changed();
}

void Editor::setStr(const std::string &id, const std::string &key, const std::string &v)
{
	host->store().set(id + "." + key, Val::str(v));
	host->changed();
}

void Editor::pushUndo()
{
	FlatMap snap = captureChain(host->store());
	if (undoPos >= 0 && toFlatJson(undo[(size_t)undoPos]) == toFlatJson(snap))
		return;
	undo.resize((size_t)(undoPos + 1));
	undo.push_back(std::move(snap));
	if (undo.size() > 100)
		undo.erase(undo.begin());
	undoPos = (int)undo.size() - 1;
}

void Editor::commit()
{
	pushUndo();
	host->gestureEnded();
	update();
}

void Editor::restore(const FlatMap &snap)
{
	applyChain(host->store(), snap);
	host->changed();
	host->gestureEnded();
	refreshChain();
	selBand = -1;
	update();
}

void Editor::tick()
{
	if (!host->alive()) {
		timer.stop();
		close();
		return;
	}
	if (!isVisible())
		return;
	for (const ModInfo &m : mods)
		if (auto mod = host->module(m.id)) {
			std::deque<float> &h = hist[m.id];
			const float g = host->enabled() && m.on ? mod->gr.load() : 0.0f;
			h.push_back(g > -0.05f && g < 0.05f ? 0.0f : g);
			if (h.size() > 300)
				h.pop_front();
			if (m.mt->last)
				for (int c = 0; c < 2; c++)
					peakHold[c] = std::max(peakHold[c] - 0.4f, mod->aux[c].load());
		}
	if (!simple && selected() && selected()->mt->type == "eq")
		updateSpectrum();
	update();
}

// ------------------------------------------------------------------ input
const Hit *Editor::hitAt(QPointF pt) const
{
	for (size_t i = hits.size(); i-- > 0;)
		if (hits[i].r.contains(pt))
			return &hits[i];
	return nullptr;
}

void Editor::mousePressEvent(QMouseEvent *e)
{
	setFocus();
	if (e->button() != Qt::LeftButton)
		return;
	const QPointF pt = logical(e->position());
	const Hit *h = hitAt(pt);
	if (!h)
		return;
	active = *h; // a copy: painting rebuilds `hits` while the drag is in progress
	activeId = active.id;
	pressPos = lastPos = pt;
	dragging = (bool)active.drag || (bool)active.release;
	if (active.press)
		active.press(pt, e->modifiers());
	update();
}

void Editor::mouseMoveEvent(QMouseEvent *e)
{
	const QPointF pt = logical(e->position());
	lastPos = pt;
	if (dragging && (e->buttons() & Qt::LeftButton)) {
		if (active.drag)
			active.drag(pt, e->modifiers());
		update();
		return;
	}
	const Hit *h = hitAt(pt);
	const std::string id = h ? h->id : "";
	setCursor(h ? h->cursor : Qt::ArrowCursor);
	if (id != hoverId) {
		hoverId = id;
		setToolTip(h ? h->tip : QString());
		update();
	}
}

void Editor::mouseReleaseEvent(QMouseEvent *)
{
	if (dragging && active.release)
		active.release();
	dragging = false;
	activeId.clear();
	update();
}

void Editor::mouseDoubleClickEvent(QMouseEvent *e)
{
	const QPointF pt = logical(e->position());
	if (const Hit *h = hitAt(pt)) {
		if (h->dbl) {
			const Hit copy = *h;
			copy.dbl(pt, e->modifiers());
			update();
			return;
		}
	}
	mousePressEvent(e);
}

void Editor::wheelEvent(QWheelEvent *e)
{
	const QPointF pt = logical(e->position());
	if (const Hit *h = hitAt(pt)) {
		if (h->wheel) {
			const Hit copy = *h;
			copy.wheel(e->angleDelta().y() > 0 ? 1 : -1, e->modifiers(), pt);
			update();
			e->accept();
		}
	}
}

void Editor::keyPressEvent(QKeyEvent *e)
{
	const ModInfo *m = selected();
	if (e->matches(QKeySequence::Undo)) {
		if (undoPos > 0)
			restore(undo[(size_t)--undoPos]);
	} else if (e->matches(QKeySequence::Redo)) {
		if (undoPos + 1 < (int)undo.size())
			restore(undo[(size_t)++undoPos]);
	} else if (e->key() == Qt::Key_Escape) {
		selBand = -1;
	} else if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && m && m->mt->type == "eq" &&
		   selBand >= 0 && !simple) {
		if (removeEqBand(host->store(), m->id, selBand)) {
			selBand = -1;
			host->changed();
			commit();
		}
	} else {
		QWidget::keyPressEvent(e);
		return;
	}
	update();
}

void Editor::closeEvent(QCloseEvent *e)
{
	if (host->alive()) {
		host->store().set("px_win_w", Val::num(width()));
		host->store().set("px_win_h", Val::num(height()));
		if (host->loopState() != 0)
			host->setLoop(0); // never leave a loop playing to the stream behind a closed editor
	}
	QWidget::closeEvent(e);
}

bool Editor::event(QEvent *e)
{
	return QWidget::event(e);
}

// ------------------------------------------------------------------ primitives
Hit &Editor::hit(QRectF r, const std::string &id)
{
	hits.emplace_back();
	hits.back().r = r;
	hits.back().id = id;
	return hits.back();
}

void Editor::text(QRectF r, const QString &s, const QColor &c, int flags, int px, bool mono, int weight)
{
	QFont f = mono ? monoFont : uiFont;
	f.setPixelSize(px);
	f.setWeight((QFont::Weight)weight);
	p->setFont(f);
	p->setPen(c);
	p->drawText(r, flags | Qt::TextSingleLine, s);
}

double Editor::textW(const QString &s, int px, bool mono, int weight) const
{
	QFont f = mono ? monoFont : uiFont;
	f.setPixelSize(px);
	f.setWeight((QFont::Weight)weight);
	return QFontMetricsF(f).horizontalAdvance(s);
}

void Editor::box(QRectF r, const QColor &fill, const QColor &stroke, double radius)
{
	p->setPen(stroke.alpha() ? QPen(stroke, 1) : Qt::NoPen);
	p->setBrush(fill);
	p->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

double Editor::button(double x, double y, const QString &label, bool pressed, std::function<void()> fn,
		      const std::string &id, bool enabled, double w, const QString &tip)
{
	if (w <= 0)
		w = textW(label, 12, false, QFont::Medium) + 20;
	// hover steps up 1px, press sinks 1px
	const double dy = !enabled ? 0 : (activeId == id ? 1 : (hovered(id) ? -1 : 0));
	const QRectF r(x, y + dy, w, 28);
	box(r, col::raised, pressed ? col::accent : (hovered(id) && enabled ? col::muted : col::line2));
	text(r, label, !enabled ? col::dim : (pressed ? col::accent : col::fg), Qt::AlignCenter, 12, false,
	     QFont::Medium);
	Hit &h = hit(QRectF(x, y, w, 28), id);
	h.tip = tip;
	if (enabled) {
		h.cursor = Qt::PointingHandCursor;
		h.press = [fn](QPointF, Qt::KeyboardModifiers) {
			fn();
		};
	}
	return x + w;
}

double Editor::iconButton(double x, double y, const char *name, bool pressed, std::function<void()> fn,
			  const std::string &id, const QString &tip, bool enabled)
{
	const double dy = !enabled ? 0 : (activeId == id ? 1 : (hovered(id) ? -1 : 0));
	const QRectF r(x, y + dy, 28, 28);
	box(r, col::raised, pressed ? col::accent : (hovered(id) && enabled ? col::muted : col::line2));
	icon(r, name, !enabled ? col::dim : (pressed ? col::accent : col::fg));
	Hit &h = hit(QRectF(x, y, 28, 28), id);
	h.tip = tip;
	if (enabled) {
		h.cursor = Qt::PointingHandCursor;
		h.press = [fn](QPointF, Qt::KeyboardModifiers) {
			fn();
		};
	}
	return x + 28;
}

void Editor::icon(QRectF r, const char *name, const QColor &c)
{
	const QPointF o = r.center();
	p->setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p->setBrush(Qt::NoBrush);
	const std::string n = name;
	QPainterPath path;
	if (n == "left") {
		path.moveTo(o + QPointF(2, -4.5));
		path.lineTo(o + QPointF(-2.5, 0));
		path.lineTo(o + QPointF(2, 4.5));
	} else if (n == "right") {
		path.moveTo(o + QPointF(-2, -4.5));
		path.lineTo(o + QPointF(2.5, 0));
		path.lineTo(o + QPointF(-2, 4.5));
	} else if (n == "down" || n == "up") {
		const double s = n == "down" ? 1 : -1;
		path.moveTo(o + QPointF(-4.5, -2 * s));
		path.lineTo(o + QPointF(0, 2.5 * s));
		path.lineTo(o + QPointF(4.5, -2 * s));
	} else if (n == "plus") {
		path.moveTo(o + QPointF(0, -5));
		path.lineTo(o + QPointF(0, 5));
		path.moveTo(o + QPointF(-5, 0));
		path.lineTo(o + QPointF(5, 0));
	} else if (n == "x") {
		path.moveTo(o + QPointF(-4, -4));
		path.lineTo(o + QPointF(4, 4));
		path.moveTo(o + QPointF(4, -4));
		path.lineTo(o + QPointF(-4, 4));
	} else if (n == "trash") {
		path.moveTo(o + QPointF(-5.5, -4));
		path.lineTo(o + QPointF(5.5, -4));
		path.moveTo(o + QPointF(-2, -4));
		path.lineTo(o + QPointF(-2, -5.5));
		path.lineTo(o + QPointF(2, -5.5));
		path.lineTo(o + QPointF(2, -4));
		path.moveTo(o + QPointF(-4, -4));
		path.lineTo(o + QPointF(-3.2, 5.5));
		path.lineTo(o + QPointF(3.2, 5.5));
		path.lineTo(o + QPointF(4, -4));
	} else if (n == "power") {
		path.moveTo(o + QPointF(0, -6.5));
		path.lineTo(o + QPointF(0, -0.5));
		p->drawArc(QRectF(o.x() - 5.5, o.y() - 5, 11, 11), 130 * 16, 280 * 16);
	}
	p->drawPath(path);
}

double Editor::seg(double x, double y, const QStringList &opts, int cur, std::function<void(int)> fn,
		   const std::string &id)
{
	double w = 0;
	for (const QString &o : opts)
		w += textW(o, 12, false, QFont::Medium) + 18;
	box(QRectF(x, y, w, 28), col::scope, col::line2);
	double cx = x;
	for (int i = 0; i < opts.size(); i++) {
		const double cw = textW(opts[i], 12, false, QFont::Medium) + 18;
		const std::string hid = id + "." + std::to_string(i);
		const QRectF r(cx, y, cw, 28);
		if (i == cur)
			box(r.adjusted(1, 1, -1, -1), col::raised, QColor(0, 0, 0, 0), 5);
		text(r, opts[i], i == cur || hovered(hid) ? col::fg : col::muted, Qt::AlignCenter, 12, false,
		     QFont::Medium);
		Hit &h = hit(r, hid);
		h.cursor = Qt::PointingHandCursor;
		h.press = [fn, i](QPointF, Qt::KeyboardModifiers) {
			fn(i);
		};
		cx += cw;
	}
	return x + w;
}

void Editor::dropdown(double x, double y, double w, const QString &cur, const QStringList &opts,
		      std::function<void(int)> fn, const std::string &id)
{
	const QRectF r(x, y, w, 28);
	box(r, col::raised, hovered(id) ? col::muted : col::line2);
	text(r.adjusted(8, 0, -22, 0), cur, col::fg, Qt::AlignLeft | Qt::AlignVCenter, 12);
	icon(QRectF(x + w - 24, y, 24, 28), "down", col::muted);
	Hit &h = hit(r, id);
	h.cursor = Qt::PointingHandCursor;
	const QPoint at = mapToGlobal((QPointF(x, y + 30) * scale).toPoint());
	h.press = [this, opts, fn, at](QPointF, Qt::KeyboardModifiers) {
		QMenu menu(this);
		for (int i = 0; i < opts.size(); i++)
			menu.addAction(opts[i])->setData(i);
		if (QAction *a = menu.exec(at))
			fn(a->data().toInt());
	};
}

void Editor::knob(double cx, double y, const QString &label, double v, double mn, double mx, bool log, bool bipolar,
		  double def, const QString &valueText, const QColor &c, std::function<void(double)> set,
		  const std::string &id, double size)
{
	auto toN = [=](double x) {
		return clampd(log ? std::log(x / mn) / std::log(mx / mn) : (x - mn) / (mx - mn), 0, 1);
	};
	auto fromN = [=](double n) {
		n = clampd(n, 0, 1);
		return log ? mn * std::pow(mx / mn, n) : mn + n * (mx - mn);
	};
	const double n = toN(v), n0 = bipolar ? toN(0) : 0;
	const bool big = size > 60;
	text(QRectF(cx - 60, y, 120, 16), label, big ? col::fg : col::muted, Qt::AlignCenter, big ? 13 : 12, false,
	     big ? QFont::Medium : QFont::Normal);
	const QRectF arc(cx - size * 0.39, y + 18 + size * 0.11, size * 0.78, size * 0.78);
	p->setBrush(Qt::NoBrush);
	p->setPen(QPen(col::line2, size / 11.5, Qt::SolidLine, Qt::RoundCap));
	p->drawArc(arc, 225 * 16, -270 * 16);
	if (std::fabs(n - n0) > 0.002) {
		p->setPen(QPen(c, size / 11.5, Qt::SolidLine, Qt::RoundCap));
		p->drawArc(arc, (int)((225 - 270 * std::min(n, n0)) * 16), (int)(-270 * std::fabs(n - n0) * 16));
	}
	const QPointF o = arc.center();
	p->setPen(QPen(hovered(id) || activeId == id ? col::muted : col::line2, 1));
	p->setBrush(col::raised);
	p->drawEllipse(o, size * 0.26, size * 0.26);
	const double a = (-135 + 270 * n) * kPi / 180;
	p->setPen(QPen(col::fg, 2, Qt::SolidLine, Qt::RoundCap));
	p->drawLine(o, o + QPointF(std::sin(a), -std::cos(a)) * size * 0.21);
	text(QRectF(cx - 60, y + 18 + size, 120, 16), valueText, col::fg, Qt::AlignCenter, 12, true);

	Hit &h = hit(QRectF(cx - size / 2, y + 16, size, size + 2), id);
	h.cursor = Qt::SizeVerCursor;
	h.press = [this, n](QPointF pt, Qt::KeyboardModifiers) {
		dragN0 = n;
		dragY0 = pt.y();
	};
	h.drag = [this, set, fromN](QPointF pt, Qt::KeyboardModifiers m) {
		set(fromN(dragN0 + (dragY0 - pt.y()) * (m & Qt::ShiftModifier ? 0.001 : 0.005)));
	};
	h.release = [this] {
		commit();
	};
	h.wheel = [this, set, fromN, n](int d, Qt::KeyboardModifiers m, QPointF) {
		set(fromN(n + d * (m & Qt::ShiftModifier ? 0.004 : 0.02)));
		commit();
	};
	h.dbl = [this, set, def](QPointF, Qt::KeyboardModifiers) {
		set(def);
		commit();
	};
}

QString Editor::fmtHz(double f)
{
	return f < 1000 ? QString::number(f, 'f', f < 100 ? 1 : 0) + " Hz"
			: QString::number(f / 1000, 'f', f < 10000 ? 2 : 1) + " kHz";
}

QString Editor::fmt(const ParamDef &pd, double v)
{
	if (pd.unit == "dB")
		return (v > 0 && pd.min < 0 ? "+" : "") + QString::number(v, 'f', 1) + " dB";
	if (pd.unit == "Hz")
		return fmtHz(v);
	if (pd.unit == "ms")
		return v < 10     ? QString::number(v, 'f', 1) + " ms"
		       : v < 1000 ? QString::number(v, 'f', 0) + " ms"
				  : QString::number(v / 1000, 'f', 2) + " s";
	if (pd.unit == "s")
		return QString::number(v, 'f', 2) + " s";
	if (pd.unit == "%")
		return QString::number(v, 'f', 0) + "%";
	if (pd.unit == ":1")
		return v >= 50 ? QStringLiteral("∞:1") : QString::number(v, 'f', 1) + ":1";
	return QString::number(v, 'f', v < 1 ? 3 : 2);
}

double Editor::paramKnob(double x, double y, const std::string &id, const ParamDef &pd, const QColor &c,
			 const QString &label)
{
	QString l = label;
	if (l.isEmpty()) {
		l = qs(pd.label);
		const int sep = l.lastIndexOf(QStringLiteral(" · "));
		if (sep >= 0)
			l = l.mid(sep + 3);
	}
	const std::string key = pd.key;
	const double pitch = std::max(74.0, textW(l, 12) + 14);
	knob(
		x + pitch / 2 - 4, y, l, val(id, key), pd.min, pd.max, pd.log, pd.min < 0 && pd.max > 0, pd.def,
		fmt(pd, val(id, key)), c, [this, id, key](double v) { setVal(id, key, v); }, id + "." + key);
	return x + pitch;
}

void Editor::gridFreq(QRectF r)
{
	auto fx = [&](double f) {
		return r.left() + r.width() * std::log(f / 20.0) / std::log(1000.0);
	};
	for (int dec = 10; dec <= 10000; dec *= 10)
		for (int m = 1; m < 10; m++) {
			const double f = dec * m;
			if (f < 20 || f > 20000)
				continue;
			const bool major = m == 1 || m == 2 || m == 5;
			p->setPen(QPen(major ? QColor(0x20, 0x26, 0x2e) : QColor(0x17, 0x1b, 0x21), 1));
			p->drawLine(QPointF(fx(f), r.top()), QPointF(fx(f), r.bottom()));
			if (major)
				text(QRectF(fx(f) - 20, r.bottom() + 2, 40, 14),
				     f >= 1000 ? QString::number(f / 1000) + "k" : QString::number(f), col::dim,
				     Qt::AlignCenter, 10, true);
		}
}

void Editor::history(QRectF r, const std::deque<float> &h, double maxDb, const QColor &c, const QString &label)
{
	p->setPen(QPen(QColor(0x1a, 0x1f, 0x26), 1));
	for (int i = 0; i <= 4; i++) {
		const double y = r.top() + r.height() * i / 4;
		p->drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
		text(QRectF(r.right() - 44, y + (i == 4 ? -14 : 1), 40, 13), QString::number(-maxDb * i / 4, 'f', 0),
		     col::dim, Qt::AlignRight | Qt::AlignVCenter, 10, true);
	}
	text(QRectF(r.left() + 8, r.top() + 6, r.width(), 14), label, col::muted, Qt::AlignLeft | Qt::AlignVCenter, 11,
	     true);
	if (h.empty())
		return;
	QPainterPath fill, lineP;
	const double dx = r.width() / 300.0;
	fill.moveTo(r.right(), r.top());
	for (size_t k = 0; k < h.size(); k++) {
		const double x = r.right() - k * dx,
			     y = r.top() + clampd(-h[h.size() - 1 - k] / maxDb, 0, 1) * r.height();
		fill.lineTo(x, y);
		k ? lineP.lineTo(x, y) : lineP.moveTo(x, y);
	}
	fill.lineTo(r.right() - (h.size() - 1) * dx, r.top());
	QColor f = c;
	f.setAlpha(64);
	p->fillPath(fill, f);
	p->setPen(QPen(c, 1.5));
	p->setBrush(Qt::NoBrush);
	p->drawPath(lineP);
}

// ------------------------------------------------------------------ paint
void Editor::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::TextAntialiasing);
	p = &painter;
	hits.clear();
	p->fillRect(rect(), col::win);
	if (!host->alive()) {
		p = nullptr;
		return;
	}
	p->scale(scale, scale);
	const double W = width() / scale, H = height() / scale;
	refreshChain();

	drawTop(W);
	double y = 44;
	if (!single) {
		drawRack(W, y);
		y += 74;
	}
	auto banner = [&](const QString &msg) {
		p->fillRect(QRectF(0, y, W, 28), col::scope);
		text(QRectF(12, y, W - 24, 28), msg, col::accent, Qt::AlignLeft | Qt::AlignVCenter, 12);
		p->setPen(col::line);
		p->drawLine(QPointF(0, y + 27.5), QPointF(W, y + 27.5));
		y += 28;
	};
	if (limiterMisplaced(host->store()))
		banner(QStringLiteral(
			"Limiter isn't the last module before Output, so later modules can push peaks over the ceiling."));
	if (host->loopState() == 2)
		banner(QStringLiteral(
			"The loop is playing in place of your live mic. If you're live, the audience hears the loop."));
	if (!simple) {
		drawHeader(W, y);
		y += 44;
	}
	const double footer = 30, strip = single ? 0 : 96;
	const QRectF view(0, y, W - strip, H - y - footer);
	p->fillRect(view, col::scope);
	const ModInfo *m = selected();
	p->save();
	p->setClipRect(view);
	if (simple)
		drawSimple(view);
	else if (m && m->mt->type == "eq")
		drawEq(view);
	else if (m && m->mt->type == "comp")
		drawComp(view);
	else if (m && m->mt->type == "mb")
		drawMb(view);
	else if (m && m->mt->type == "out")
		drawOutput(view);
	else if (m)
		drawGeneric(view);
	p->restore();
	if (!host->enabled()) // bypassed: grey out the rack and the module area
		p->fillRect(QRectF(0, 44, W - strip, H - 44 - footer), QColor(0x13, 0x16, 0x1b, 150));
	if (!single) {
		p->setPen(col::line);
		p->drawLine(QPointF(W - strip + 0.5, y - (simple ? 0 : 44)), QPointF(W - strip + 0.5, H - footer));
		drawOutStrip(QRectF(W - strip, y - (simple ? 0 : 44), strip, H - footer - y + (simple ? 0 : 44)));
	}
	drawFooter(W, H - footer);
	p = nullptr;
}

// ------------------------------------------------------------------ top bar
void Editor::drawTop(double W)
{
	double x = 12;
	const double y = 8;
	p->setPen(Qt::NoPen);
	p->setBrush(col::accent);
	p->drawEllipse(QPointF(x + 4, 22), 4, 4);
	const double bw = textW("Parametrix", 15, false, QFont::DemiBold);
	text(QRectF(x + 16, 0, bw + 4, 44), "Parametrix", col::fg, Qt::AlignLeft | Qt::AlignVCenter, 15, false,
	     QFont::DemiBold);
	{
		Hit &h = hit(QRectF(x, 6, bw + 20, 32), "brand");
		h.tip = "UI scale";
		const QPoint at = mapToGlobal((QPointF(x, 40) * scale).toPoint());
		h.press = [this, at](QPointF, Qt::KeyboardModifiers) {
			QMenu menu(this);
			for (int pct : {100, 125, 150, 200}) {
				QAction *a = menu.addAction(QString("UI scale %1%").arg(pct));
				a->setCheckable(true);
				a->setChecked(std::fabs(scale * 100 - pct) < 1);
				a->setData(pct);
			}
			if (QAction *a = menu.exec(at)) {
				const double old = scale;
				scale = a->data().toInt() / 100.0;
				host->store().set(kScaleKey, Val::num(scale));
				setMinimumSize((int)(900 * scale), (int)(560 * scale));
				resize((int)(width() * scale / old), (int)(height() * scale / old));
			}
		};
	}
	x += bw + 32;

	if (!single) {
		// preset browser: previous, name (opens the list), next
		const std::vector<PresetEntry> list = host->presets();
		auto step = [this, list](int d) {
			if (list.empty())
				return;
			int idx = -1;
			for (size_t i = 0; i < list.size(); i++)
				if (list[i].name == presetName)
					idx = (int)i;
			idx = ((idx < 0 ? 0 : idx + d) + (int)list.size()) % (int)list.size();
			if (host->loadPreset(list[(size_t)idx].id)) {
				presetName = list[(size_t)idx].name;
				host->changed();
				refreshChain();
				selBand = -1;
				presetBase = captureChain(host->store());
				commit();
			}
		};
		const double pw = clampd(W - 950, 150, 230);
		box(QRectF(x, y, pw, 28), col::scope, col::line2);
		icon(QRectF(x, y, 26, 28), "left", hovered("p.prev") ? col::fg : col::muted);
		icon(QRectF(x + pw - 26, y, 26, 28), "right", hovered("p.next") ? col::fg : col::muted);
		Hit &hp = hit(QRectF(x, y, 26, 28), "p.prev");
		hp.cursor = Qt::PointingHandCursor;
		hp.tip = "Previous preset";
		hp.press = [step](QPointF, Qt::KeyboardModifiers) {
			step(-1);
		};
		Hit &hn = hit(QRectF(x + pw - 26, y, 26, 28), "p.next");
		hn.cursor = Qt::PointingHandCursor;
		hn.tip = "Next preset";
		hn.press = [step](QPointF, Qt::KeyboardModifiers) {
			step(1);
		};
		const bool dirty = toFlatJson(captureChain(host->store())) != toFlatJson(presetBase);
		text(QRectF(x + 26, y, pw - 52, 28),
		     (presetName.empty() ? QStringLiteral("Custom") : qs(presetName)) + (dirty ? " *" : ""),
		     hovered("p.name") ? col::accent : col::fg, Qt::AlignCenter, 12, false, QFont::Medium);
		Hit &hm = hit(QRectF(x + 26, y, pw - 52, 28), "p.name");
		hm.cursor = Qt::PointingHandCursor;
		hm.tip = "Browse presets";
		const QPoint at = mapToGlobal((QPointF(x, y + 30) * scale).toPoint());
		hm.press = [this, at](QPointF, Qt::KeyboardModifiers) {
			presetMenu(at);
		};
		x += pw + 6;

		x = button(
			    x, y, "Save preset", false,
			    [this] {
				    bool ok = false;
				    const QString name = QInputDialog::getText(this, "Save preset", "Preset name",
									       QLineEdit::Normal, qs(presetName), &ok);
				    if (ok && !name.trimmed().isEmpty() &&
					host->savePreset(name.trimmed().toStdString())) {
					    presetName = name.trimmed().toStdString();
					    presetBase = captureChain(host->store());
				    }
			    },
			    "save") +
		    8;
	}

	x = seg(
		    x, y, {"A", "B"}, onB ? 1 : 0,
		    [this](int i) {
			    if ((i == 1) == onB)
				    return;
			    (onB ? slotB : slotA) = captureChain(host->store());
			    onB = i == 1;
			    restore(onB ? slotB : slotA);
			    pushUndo();
		    },
		    "ab") +
	    8;
	x = button(x, y, "Undo", false, [this] { restore(undo[(size_t)--undoPos]); }, "undo", undoPos > 0) + 4;
	x = button(
		    x, y, "Redo", false, [this] { restore(undo[(size_t)++undoPos]); }, "redo",
		    undoPos + 1 < (int)undo.size()) +
	    8;

	const int loop = host->loopState();
	QString loopLabel = "Capture loop";
	if (loop == 1)
		loopLabel = QString("Recording %1 s").arg(std::max(0, 10 - (int)(loopClock.elapsed() / 1000)));
	else if (loop == 2)
		loopLabel = "Stop loop";
	x = button(
		x, y, loopLabel, loop == 2,
		[this, loop] {
			host->setLoop(loop == 0 ? 1 : 0);
			loopClock.restart();
		},
		"loop", true, 0, "Record 10 s of your voice and loop it while you adjust settings");

	// right side
	double rx = W - 12 - 32;
	const bool on = host->enabled();
	{
		const QRectF r(rx, 6, 32, 32);
		QColor ring = on ? col::ok : col::line2;
		if (on) { // breathing glow
			const double ph = 0.5 + 0.5 * std::sin(QDateTime::currentMSecsSinceEpoch() / 2400.0 * 2 * kPi);
			QColor glow = col::ok;
			glow.setAlphaF(0.35 * ph);
			p->setPen(QPen(glow, 4));
			p->setBrush(Qt::NoBrush);
			p->drawEllipse(r.adjusted(-1, -1, 1, 1));
		}
		p->setPen(QPen(ring, 1));
		p->setBrush(col::scope);
		p->drawEllipse(r.adjusted(0.5, 0.5, -0.5, -0.5));
		icon(r, "power", on ? col::ok : col::dim);
		Hit &h = hit(r, "power");
		h.cursor = Qt::PointingHandCursor;
		h.tip = on ? "Turn Parametrix off" : "Turn Parametrix on";
		h.press = [this, on](QPointF, Qt::KeyboardModifiers) {
			host->setEnabled(!on);
		};
	}
	if (!on) {
		const double tw = textW("Bypassed", 12, true) + 14;
		rx -= tw + 8;
		box(QRectF(rx, 11, tw, 22), col::scope, col::line2);
		text(QRectF(rx, 11, tw, 22), "Bypassed", col::muted, Qt::AlignCenter, 12, true);
	}
	const double ms = host->latencySamples() * 1000.0 / host->sampleRate();
	const QString stat = QString("Latency %1 ms   CPU %2%").arg(ms, 0, 'f', 1).arg(host->cpuPercent(), 0, 'f', 1);
	const double sw = textW(stat, 12, true);
	const double mw = textW("Simple", 12, false, QFont::Medium) + textW("Advanced", 12, false, QFont::Medium) + 36;
	const double room = rx - x - 12;
	if (room >= mw + sw + 24) {
		rx -= sw + 12;
		text(QRectF(rx, 0, sw + 4, 44), stat, ms > 5 ? col::accent : col::muted,
		     Qt::AlignLeft | Qt::AlignVCenter, 12, true);
	}
	if (!single && room >= mw + 12)
		seg(rx - mw - 12, y, {"Simple", "Advanced"}, simple ? 0 : 1, [this](int i) { simple = i == 0; }, "mode");
	p->setPen(col::line);
	p->drawLine(QPointF(0, 43.5), QPointF(W, 43.5));
}

void Editor::presetMenu(QPoint at)
{
	QMenu menu(this);
	const std::vector<PresetEntry> list = host->presets();
	std::map<std::string, QMenu *> subs;
	for (size_t i = 0; i < list.size(); i++) {
		QMenu *&sub = subs[list[i].category];
		if (!sub)
			sub = menu.addMenu(qs(list[i].category));
		QAction *a = sub->addAction(qs(list[i].name));
		a->setData((int)i);
		a->setCheckable(true);
		a->setChecked(list[i].name == presetName);
	}
	menu.addSeparator();
	QMenu *slotMenu = menu.addMenu("Assign current preset to hotkey slot");
	for (int i = 1; i <= 8; i++) {
		std::string cur;
		host->store().getStr("px_slot" + std::to_string(i), cur);
		slotMenu->addAction(QString("Slot %1%2").arg(i).arg(cur.empty() ? "" : " · " + qs(cur)))->setData(-i);
	}
	QAction *a = menu.exec(at);
	if (!a)
		return;
	const int d = a->data().toInt();
	if (d < 0) {
		host->store().set("px_slot" + std::to_string(-d), Val::str(presetName));
		host->gestureEnded();
	} else if (host->loadPreset(list[(size_t)d].id)) {
		presetName = list[(size_t)d].name;
		host->changed();
		refreshChain();
		selBand = -1;
		presetBase = captureChain(host->store());
		commit();
	}
}

// ------------------------------------------------------------------ rack
void Editor::drawRack(double W, double y)
{
	const int n = (int)mods.size();
	const double cw = std::min(104.0, (W - 24 - 60) / std::max(n, 1) - 6), pitch = cw + 6;
	double x = 12;
	for (int i = 0; i < n; i++) {
		const ModInfo &m = mods[(size_t)i];
		const bool sel = !simple && m.id == selId;
		const std::string hid = "card." + m.id;
		const double dy = dragCard == i ? 0 : (hovered(hid) ? -1 : 0);
		const QRectF r(x, y + 10 + dy, cw, 54);
		box(r, sel ? col::raised : col::scope, sel ? col::accent : (hovered(hid) ? col::muted : col::line2), 8);
		const bool locked = m.mt->first || m.mt->last;

		Hit &h = hit(QRectF(x, y + 10, cw, 54), hid);
		h.cursor = Qt::PointingHandCursor;
		h.tip = qs(m.mt->name) + (locked ? " (fixed position)" : " — drag to reorder");
		const std::string id = m.id;
		h.press = [this, id, i, locked](QPointF pt, Qt::KeyboardModifiers) {
			selId = id;
			simple = false;
			selBand = -1;
			dragCard = locked ? -1 : i;
			dropX = pt.x();
		};
		h.drag = [this](QPointF pt, Qt::KeyboardModifiers) {
			dropX = pt.x();
		};
		h.release = [this, id, i, pitch] {
			if (dragCard >= 0 && std::fabs(dropX - pressPos.x()) > pitch / 2) {
				const int target = (int)std::floor((dropX - 12) / pitch);
				for (int k = 0; k < std::abs(target - i); k++)
					if (!moveModule(host->store(), id, target < i ? -1 : 1))
						break;
				host->changed();
				commit();
			}
			dragCard = -1;
		};

		// bypass dot
		const QPointF dot(x + 13, y + 26 + dy);
		p->setPen(QPen(m.on ? col::ok : col::dim, 1.5));
		p->setBrush(m.on ? col::ok : QColor(0, 0, 0, 0));
		p->drawEllipse(dot, 5, 5);
		Hit &hd = hit(QRectF(x + 4, y + 16, 20, 20), "pw." + m.id);
		hd.cursor = Qt::PointingHandCursor;
		hd.tip = m.on ? "Bypass module" : "Enable module";
		const bool on = m.on;
		hd.press = [this, id, on](QPointF, Qt::KeyboardModifiers) {
			setVal(id, "on", on ? 0 : 1);
			commit();
		};

		// name, numbered when a type appears more than once
		int same = 0, idx = 0;
		for (const ModInfo &o : mods)
			if (o.mt == m.mt) {
				same++;
				if (&o <= &m)
					idx = same;
			}
		QString name = qs(m.mt->name) + (same > 1 ? QString(" %1").arg(idx) : QString());
		QFont f = uiFont;
		f.setPixelSize(12);
		f.setWeight(QFont::Medium);
		f.setStrikeOut(!m.on);
		p->setFont(f);
		p->setPen(m.on ? col::fg : col::dim);
		p->drawText(QRectF(x + 24, y + 16 + dy, cw - 28, 20),
			    Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
			    QFontMetricsF(f).elidedText(name, Qt::ElideRight, cw - 28));

		if (hasMeter(m.mt->type)) {
			const QRectF bar(x + 8, y + 50 + dy, cw - 16, 4);
			box(bar, col::line, QColor(0, 0, 0, 0), 2);
			const std::deque<float> &hh = hist[m.id];
			const double g = hh.empty() ? 0 : clampd(std::fabs(hh.back()) / 20.0, 0, 1);
			if (g > 0.01)
				box(QRectF(bar.right() - bar.width() * g, bar.top(), bar.width() * g, 4), col::accent,
				    QColor(0, 0, 0, 0), 2);
		}
		x += pitch;
	}
	if (dragCard >= 0 && dragging && std::fabs(dropX - pressPos.x()) > pitch / 2) {
		const int target = (int)clampd(std::floor((dropX - 12) / pitch), 1, n - 2);
		const double mx = 12 + (target + (target > dragCard ? 1 : 0)) * pitch - 3;
		p->setPen(QPen(col::accent, 3, Qt::SolidLine, Qt::RoundCap));
		p->drawLine(QPointF(mx, y + 12), QPointF(mx, y + 62));
	}
	// add module
	const QRectF add(x, y + 10, 54, 54);
	p->setPen(QPen(hovered("add") ? col::muted : col::line2, 1, Qt::DashLine));
	p->setBrush(Qt::NoBrush);
	p->drawRoundedRect(add.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
	icon(add, "plus", hovered("add") ? col::fg : col::muted);
	Hit &ha = hit(add, "add");
	ha.cursor = Qt::PointingHandCursor;
	ha.tip = n >= kMaxModules ? "The rack is full (16 modules)" : "Add module";
	const QPoint at = mapToGlobal((QPointF(add.left(), add.bottom() + 4) * scale).toPoint());
	if (n < kMaxModules)
		ha.press = [this, at](QPointF, Qt::KeyboardModifiers) {
			addMenu(at);
		};
	p->setPen(col::line);
	p->drawLine(QPointF(0, y + 73.5), QPointF(W, y + 73.5));
}

void Editor::addMenu(QPoint at)
{
	QMenu menu(this);
	for (const ModuleType &t : registry())
		if (!t.first && !t.last)
			menu.addAction(qs(t.name))->setData(qs(t.type));
	if (QAction *a = menu.exec(at)) {
		const std::string id = addModule(host->store(), a->data().toString().toStdString());
		if (!id.empty()) {
			selId = id;
			simple = false;
			selBand = -1;
			host->changed();
			commit();
		}
	}
}

// ------------------------------------------------------------------ module header
void Editor::drawHeader(double W, double y)
{
	const ModInfo *m = selected();
	if (!m)
		return;
	const std::string id = m->id;
	const double by = y + 8;
	double x = 12;
	const double tw = textW(qs(m->mt->name), 14, false, QFont::DemiBold);
	text(QRectF(x, y, tw + 4, 44), qs(m->mt->name), col::fg, Qt::AlignLeft | Qt::AlignVCenter, 14, false,
	     QFont::DemiBold);
	x += tw + 12;
	const bool on = m->on;
	x = button(
		    x, by, on ? "On" : "Bypassed", on,
		    [this, id, on] {
			    setVal(id, "on", on ? 0 : 1);
			    commit();
		    },
		    "h.on") +
	    8;
	{
		const std::string type = m->mt->type;
		const QPoint at = mapToGlobal((QPointF(x, by + 30) * scale).toPoint());
		x = button(
			    x, by, "Presets", false,
			    [this, id, type, at] {
				    QMenu menu(this);
				    const std::vector<PresetEntry> list = host->modulePresets(type);
				    for (size_t i = 0; i < list.size(); i++)
					    menu.addAction(qs(list[i].category) + QStringLiteral(" · ") +
							   qs(list[i].name))
						    ->setData((int)i);
				    if (!list.empty())
					    menu.addSeparator();
				    menu.addAction("Save module preset…")->setData(-1);
				    QAction *a = menu.exec(at);
				    if (!a)
					    return;
				    const int d = a->data().toInt();
				    if (d < 0) {
					    bool ok = false;
					    const QString name = QInputDialog::getText(this, "Save module preset",
										       "Preset name", QLineEdit::Normal,
										       QString(), &ok);
					    if (ok && !name.trimmed().isEmpty())
						    host->saveModulePreset(id, name.trimmed().toStdString());
				    } else if (host->loadModulePreset(list[(size_t)d].id, id)) {
					    host->changed();
					    refreshChain();
					    selBand = -1;
					    commit();
				    }
			    },
			    "h.presets", true, 0, "Presets for this module only") +
		    8;
	}

	const QStringList sources = [this] {
		QStringList l{"Internal"};
		for (const std::string &s : host->audioSources())
			l << qs(s);
		return l;
	}();
	for (const ParamDef &pd : m->mt->params) {
		if (pd.hidden || pd.key.find('.') != std::string::npos)
			continue; // per-band parameters are edited in the module view
		const std::string key = pd.key;
		if (pd.kind == Kind::Bool) {
			const bool v = val(id, key) != 0;
			x = button(
				    x, by, qs(pd.label), v,
				    [this, id, key, v] {
					    setVal(id, key, v ? 0 : 1);
					    commit();
				    },
				    "h." + key) +
			    6;
		} else if (pd.kind == Kind::Choice) {
			QStringList opts;
			for (const std::string &o : pd.options)
				opts << qs(o);
			const int cur = (int)val(id, key);
			auto set = [this, id, key](int i) {
				setVal(id, key, i);
				commit();
			};
			if (opts.size() <= 3) {
				x = seg(x, by, opts, cur, set, "h." + key) + 8;
			} else { // a dropdown shows only its value, so it needs its name beside it
				const double lw = textW(qs(pd.label), 12) + 8;
				text(QRectF(x, y, lw, 44), qs(pd.label), col::muted, Qt::AlignLeft | Qt::AlignVCenter,
				     12);
				dropdown(x + lw, by, 130, opts.value(cur), opts, set, "h." + key);
				x += lw + 138;
			}
		} else if (pd.kind == Kind::Source) {
			const double lw = textW("Sidechain", 12) + 8;
			text(QRectF(x, y, lw, 44), "Sidechain", col::muted, Qt::AlignLeft | Qt::AlignVCenter, 12);
			const std::string cur = str(id, key);
			dropdown(
				x + lw, by, 140, cur.empty() ? QStringLiteral("Internal") : qs(cur), sources,
				[this, id, key, sources](int i) {
					setStr(id, key, i == 0 ? "" : sources[i].toStdString());
					commit();
				},
				"h." + key);
			x += lw + 148;
		}
	}
	if (m->mt->type == "denoise")
		x = button(
			    x, by, "Learn noise", false,
			    [this, id] {
				    setVal(id, "learn", val(id, "learn") + 1);
				    host->gestureEnded();
			    },
			    "h.learn", true, 0, "Stay quiet for 2 seconds while the noise profile is captured") +
		    8;
	if (m->mt->type == "eq") {
		x = seg(x, by, {"Off", "Pre", "Post", "Both"}, anaMode, [this](int i) { anaMode = i; }, "h.ana") + 8;
		x = seg(
			    x, by, {"±6", "±12", "±30"},
			    range == 6    ? 0
			    : range == 12 ? 1
					  : 2,
			    [this](int i) { range = i == 0   ? 6
						    : i == 1 ? 12
							     : 30; }, "h.rng") +
		    8;
	}

	const double strip = single ? 0 : 96;
	if (!m->mt->first && !m->mt->last && !single) {
		int i = 0;
		while (i < (int)mods.size() && mods[(size_t)i].id != id)
			i++;
		double rx = W - strip - 12;
		const double rmw = textW("Remove module", 12, false, QFont::Medium) + 20,
			     dw = textW("Duplicate module", 12, false, QFont::Medium) + 20;
		rx -= rmw;
		if (rx - dw - 70 < x)
			return; // not enough room: the rack and its menu still cover these actions
		button(
			rx, by, "Remove module", false,
			[this, id] {
				removeModule(host->store(), id);
				host->changed();
				refreshChain();
				selBand = -1;
				commit();
			},
			"h.rm");
		rx -= dw + 6;
		const std::string type = m->mt->type;
		button(
			rx, by, "Duplicate module", false,
			[this, id, type] {
				const std::string nid = addModule(host->store(), type);
				if (nid.empty())
					return;
				const ModuleType *mt = findType(type);
				for (const ParamDef &pd : mt->params) {
					double v;
					std::string s;
					if (host->store().getNum(id + "." + pd.key, v))
						host->store().set(nid + "." + pd.key, pd.kind == Kind::Bool
											      ? Val::boolean(v != 0)
											      : Val::num(v));
					else if (host->store().getStr(id + "." + pd.key, s))
						host->store().set(nid + "." + pd.key, Val::str(s));
				}
				// bring the copy next to the original
				for (int k = 0; k < kMaxModules; k++) {
					const std::vector<std::string> ids = chainIds(host->store());
					const auto a = std::find(ids.begin(), ids.end(), id),
						   b = std::find(ids.begin(), ids.end(), nid);
					if (b - a == 1 || !moveModule(host->store(), nid, b > a ? -1 : 1))
						break;
				}
				selId = nid;
				host->changed();
				commit();
			},
			"h.dup", (int)mods.size() < kMaxModules);
		rx -= 34;
		iconButton(
			rx, by, "right", false,
			[this, id] {
				moveModule(host->store(), id, 1);
				host->changed();
				commit();
			},
			"h.right", "Move later in the chain", i < (int)mods.size() - 2);
		rx -= 32;
		iconButton(
			rx, by, "left", false,
			[this, id] {
				moveModule(host->store(), id, -1);
				host->changed();
				commit();
			},
			"h.left", "Move earlier in the chain", i > 1);
	}
	p->setPen(col::line);
	p->drawLine(QPointF(0, y + 43.5), QPointF(W - strip, y + 43.5));
}

// ------------------------------------------------------------------ generic module view
void Editor::drawGeneric(QRectF r)
{
	const ModInfo *m = selected();
	const std::string type = m->mt->type;
	double y = r.top() + 14;
	if (hasMeter(type)) {
		const QRectF g(r.left() + 14, y, r.width() - 28, std::min(220.0, r.height() - 150));
		static const std::map<std::string, QColor> colors = {{"gate", col::ok},
								     {"deess", col::band[4]},
								     {"lim", col::hot},
								     {"denoise", col::band[6]},
								     {"lvl", col::band[5]}};
		const auto c = colors.find(type);
		const std::deque<float> &h = hist[m->id];
		const QString what = type == "denoise" ? "Noise reduction"
				     : type == "lvl"   ? "Level change"
						       : "Gain reduction";
		history(g, h, type == "gate" || type == "denoise" ? 40 : 24,
			c == colors.end() ? col::accent : c->second,
			QString("%1 · %2 dB").arg(what).arg(h.empty() ? 0.0 : h.back(), 0, 'f', 1));
		y = g.bottom() + 20;
	} else if (type == "sat") {
		// transfer curve of the chosen shaper at the current drive
		const double s = std::min(220.0, r.height() - 150);
		const QRectF g(r.left() + 14, y, s, s);
		p->setPen(QPen(QColor(0x1a, 0x1f, 0x26), 1));
		for (int i = 0; i <= 4; i++) {
			p->drawLine(QPointF(g.left(), g.top() + s * i / 4), QPointF(g.right(), g.top() + s * i / 4));
			p->drawLine(QPointF(g.left() + s * i / 4, g.top()), QPointF(g.left() + s * i / 4, g.bottom()));
		}
		p->setPen(QPen(col::line2, 1));
		p->drawLine(g.bottomLeft(), g.topRight());
		const double d = dbToLin(val(m->id, "drive_db")), mix = val(m->id, "mix") / 100;
		const int curve = (int)val(m->id, "curve");
		auto shape = [curve](double x) {
			if (curve == 1)
				return x >= 0 ? std::tanh(x) : std::tanh(0.7 * x) / 0.7 * 0.85;
			if (curve == 2) {
				const double u = clampd(x, -1, 1);
				return 1.5 * (u - u * u * u / 3);
			}
			return std::tanh(x);
		};
		QPainterPath path;
		for (int i = 0; i <= 100; i++) {
			const double v = -1 + 2 * i / 100.0,
				     o = clampd(shape(v * d) / std::sqrt(d) * mix + v * (1 - mix), -1, 1);
			const QPointF pt(g.left() + (v + 1) / 2 * s, g.top() + (1 - (o + 1) / 2) * s);
			i ? path.lineTo(pt) : path.moveTo(pt);
		}
		p->setPen(QPen(col::fg, 2));
		p->setBrush(Qt::NoBrush);
		p->drawPath(path);
		text(QRectF(g.left() + 8, g.top() + 6, s, 14), "Input → output", col::muted,
		     Qt::AlignLeft | Qt::AlignVCenter, 11, true);
		y = g.bottom() + 20;
	} else if (type == "pitch") {
		static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		auto mod = host->module(m->id);
		const float note = mod && host->enabled() && m->on ? mod->aux[0].load() : -1.0f;
		const QRectF t(r.left() + 16, y, 260, 84);
		box(t, col::win, col::line, 8);
		text(QRectF(t.left() + 12, t.top() + 8, 236, 16), "Detected note", col::muted,
		     Qt::AlignLeft | Qt::AlignVCenter, 12, false, QFont::Medium);
		QString big = QStringLiteral("—"), small = "No pitch found";
		if (note >= 0) {
			const int n = (int)std::floor(note + 0.5);
			const double off = (note - n) * 100, corr = mod->aux[1].load();
			big = QString("%1%2").arg(names[((n % 12) + 12) % 12]).arg(n / 12 - 1);
			small = QString("%1%2 cents off · correcting %3%4 cents")
					.arg(off > 0 ? "+" : "")
					.arg(off, 0, 'f', 0)
					.arg(corr > 0 ? "+" : "")
					.arg(corr, 0, 'f', 0);
		}
		text(QRectF(t.left() + 12, t.top() + 26, 236, 32), big, col::fg, Qt::AlignLeft | Qt::AlignVCenter, 24,
		     true, QFont::Medium);
		text(QRectF(t.left() + 12, t.top() + 60, 236, 16), small, col::muted, Qt::AlignLeft | Qt::AlignVCenter,
		     11, true);
		text(QRectF(t.right() + 16, y, r.width() - 310, 84),
		     "For one voice at a time. Retune speed 0 ms gives the hard, robotic effect; 40 ms and up sounds natural.",
		     col::muted, Qt::AlignLeft | Qt::AlignVCenter, 12);
		y += 104;
	} else {
		static const std::map<std::string, QString> hints = {
			{"trim",
			 "Align delay holds this source back without any echo. Put it on a music source to line a backing track up with a late vocal."},
			{"verb", "The tail keeps ringing when you bypass or remove the reverb."},
			{"delay",
			 "Time is in milliseconds; OBS has no tempo to sync to. The echoes ring out when you bypass the delay."}};
		const auto h = hints.find(type);
		if (h != hints.end()) {
			text(QRectF(r.left() + 16, y, r.width() - 32, 20), h->second, col::muted,
			     Qt::AlignLeft | Qt::AlignVCenter, 12);
			y += 36;
		}
	}
	double x = r.left() + 16;
	for (const ParamDef &pd : m->mt->params) {
		if (pd.kind != Kind::Float || pd.hidden || pd.key.find('.') != std::string::npos)
			continue;
		if (x + 74 > r.right()) {
			x = r.left() + 16;
			y += 92;
		}
		x = paramKnob(x, y, m->id, pd, col::accent);
	}
}

// ------------------------------------------------------------------ output strip and footer
void Editor::drawOutStrip(QRectF r)
{
	p->fillRect(r.adjusted(1, 0, 0, 0), col::win);
	text(QRectF(r.left(), r.top() + 8, r.width(), 16), "Output", col::muted, Qt::AlignCenter, 12, false,
	     QFont::Medium);
	const ModInfo *out = mods.empty() ? nullptr : &mods.back();
	const QRectF m(r.center().x() - 17, r.top() + 32, 34, r.height() - 32 - 112);
	box(m, col::scope, QColor(0, 0, 0, 0), 3);
	if (out && m.height() > 30 && host->enabled()) { // a bypassed filter gets no audio, so its meters rest
		auto mod = host->module(out->id);
		const int segs = 30;
		for (int c = 0; c < 2; c++) {
			const double lvl = mod ? clampd((mod->aux[c].load() + 60) / 60, 0, 1) : 0,
				     pk = clampd((peakHold[c] + 60) / 60, 0, 1);
			const double bx = m.left() + 2 + c * 16, bw = 14, sh = (m.height() - 4) / segs;
			for (int s = 0; s < segs && (double)s / segs < lvl; s++) {
				const double t = (double)s / segs;
				p->fillRect(QRectF(bx, m.bottom() - 2 - (s + 1) * sh + 1, bw, sh - 1.5),
					    t > 0.92   ? col::hot
					    : t > 0.75 ? col::accent
						       : col::ok);
			}
			p->fillRect(QRectF(bx, m.bottom() - 2 - pk * (m.height() - 4), bw, 1.5), col::fg);
		}
	}
	if (out)
		for (const ParamDef &pd : out->mt->params)
			if (pd.key == "gain_db")
				paramKnob(r.center().x() - 33, r.bottom() - 100, out->id, pd, col::fg);
}

void Editor::drawFooter(double W, double y)
{
	p->fillRect(QRectF(0, y, W, 30), col::win);
	p->setPen(col::line);
	p->drawLine(QPointF(0, y + 0.5), QPointF(W, y + 0.5));
	const int ch = host->channels();
	const QString fmtS = ch == 1 ? "Mono" : ch == 2 ? "Stereo" : QString("%1 ch").arg(ch);
	text(QRectF(12, y, W / 2, 30),
	     QString("%1 · %2 kHz · %3").arg(fmtS).arg(host->sampleRate() / 1000.0).arg(qs(host->sourceName())),
	     col::muted, Qt::AlignLeft | Qt::AlignVCenter, 12, true);
	if (!single) // a standalone filter has no rack to count
		text(QRectF(W / 2, y, W / 2 - 12, 30), QString("%1/%2 modules").arg(mods.size()).arg(kMaxModules),
		     col::muted, Qt::AlignRight | Qt::AlignVCenter, 12, true);
}

} // namespace px
