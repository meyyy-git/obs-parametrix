// Private declaration of the editor widget. Everything is drawn with QPainter in logical
// pixels and scaled by the UI scale, so the whole window resizes and scales as one.
#pragma once

#include "editor.hpp"
#include "px_dsp.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <deque>
#include <functional>
#include <map>

namespace px {

namespace col {
const QColor bg(0x0a, 0x0c, 0x0f), win(0x13, 0x16, 0x1b), scope(0x0d, 0x10, 0x14), panel(0x1a, 0x1e, 0x25);
const QColor raised(0x23, 0x28, 0x31), line(0x26, 0x2c, 0x35), line2(0x35, 0x3c, 0x47);
const QColor fg(0xe2, 0xe6, 0xec), muted(0x8b, 0x95, 0xa4), dim(0x73, 0x7d, 0x8c);
const QColor accent(0xf0, 0xb2, 0x4a), ok(0x5f, 0xd3, 0x9a), hot(0xff, 0x6b, 0x5b), blue(0x5f, 0xb8, 0xf2);
const QColor band[8] = {QColor(0xf0, 0xb2, 0x4a), QColor(0x5f, 0xb8, 0xf2), QColor(0xb8, 0xd8, 0x6b),
			QColor(0x5f, 0xd3, 0x9a), QColor(0xff, 0x7a, 0x9c), QColor(0xe9, 0xd3, 0x6a),
			QColor(0x4f, 0xd1, 0xc5), QColor(0xff, 0x94, 0x66)};
const QColor mb[4] = {QColor(0xff, 0x94, 0x66), QColor(0xe9, 0xd3, 0x6a), QColor(0x5f, 0xd3, 0x9a),
		      QColor(0x5f, 0xb8, 0xf2)};
} // namespace col

struct ModInfo {
	std::string id;
	const ModuleType *mt = nullptr;
	bool on = true;
};

// A clickable region registered while painting. The newest region under the pointer wins.
struct Hit {
	QRectF r;
	std::string id;
	std::function<void(QPointF, Qt::KeyboardModifiers)> press, drag, dbl;
	std::function<void()> release;
	std::function<void(int, Qt::KeyboardModifiers, QPointF)> wheel;
	Qt::CursorShape cursor = Qt::ArrowCursor;
	QString tip;
};

class Editor : public QWidget {
public:
	Editor(std::unique_ptr<EditorHost> host, QWidget *parent);

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *) override;
	void keyPressEvent(QKeyEvent *) override;
	void closeEvent(QCloseEvent *) override;
	bool event(QEvent *) override;

private:
	// --- state helpers (editor.cpp)
	void refreshChain();
	const ModInfo *info(const std::string &id) const;
	const ModInfo *selected() const { return info(selId); }
	double val(const std::string &id, const std::string &key) const;
	std::string str(const std::string &id, const std::string &key) const;
	void setVal(const std::string &id, const std::string &key, double v);
	void setStr(const std::string &id, const std::string &key, const std::string &v);
	void commit(); // end of a gesture: undo point + notify host
	void pushUndo();
	void restore(const FlatMap &snap);
	void tick();
	const Hit *hitAt(QPointF p) const;
	QPointF logical(QPointF p) const { return p / scale; }

	// --- drawing primitives (editor.cpp)
	Hit &hit(QRectF r, const std::string &id);
	bool hovered(const std::string &id) const { return hoverId == id; }
	void text(QRectF r, const QString &s, const QColor &c, int flags = Qt::AlignLeft | Qt::AlignVCenter,
		  int px = 12, bool mono = false, int weight = QFont::Normal);
	double textW(const QString &s, int px = 12, bool mono = false, int weight = QFont::Normal) const;
	void box(QRectF r, const QColor &fill, const QColor &stroke, double radius = 6);
	double button(double x, double y, const QString &label, bool pressed, std::function<void()> fn,
		      const std::string &id, bool enabled = true, double w = 0, const QString &tip = QString());
	double seg(double x, double y, const QStringList &opts, int cur, std::function<void(int)> fn,
		   const std::string &id);
	void dropdown(double x, double y, double w, const QString &cur, const QStringList &opts,
		      std::function<void(int)> fn, const std::string &id);
	void knob(double cx, double y, const QString &label, double v, double mn, double mx, bool log, bool bipolar,
		  double def, const QString &valueText, const QColor &c, std::function<void(double)> set,
		  const std::string &id, double size = 46);
	double paramKnob(double x, double y, const std::string &id, const ParamDef &p, const QColor &c,
			 const QString &label = QString());
	static QString fmt(const ParamDef &p, double v);
	static QString fmtHz(double f);
	void icon(QRectF r, const char *name, const QColor &c);
	double iconButton(double x, double y, const char *name, bool pressed, std::function<void()> fn,
			  const std::string &id, const QString &tip, bool enabled = true);
	void history(QRectF r, const std::deque<float> &h, double maxDb, const QColor &c, const QString &label);
	void gridFreq(QRectF r);

	// --- layout sections (editor.cpp)
	void drawTop(double W);
	void drawRack(double W, double y);
	void drawHeader(double W, double y);
	void drawOutStrip(QRectF r);
	void drawFooter(double W, double y);
	void drawGeneric(QRectF r);
	void presetMenu(QPoint at);
	void addMenu(QPoint at);

	// --- module views (editor_views.cpp)
	void drawEq(QRectF r);
	void drawBandPanel(QRectF area);
	void drawComp(QRectF r);
	void drawMb(QRectF r);
	void drawOutput(QRectF r);
	void drawSimple(QRectF r);
	void bandParams(const std::string &id, int b, double *p) const;
	std::string bandKey(int b, const char *k) const { return "b" + std::to_string(b) + "." + k; }
	void applyMacro(const std::string &name, double v);
	std::string ensureModule(const std::string &type);
	void updateSpectrum();

	std::unique_ptr<EditorHost> host;
	QPainter *p = nullptr;
	double scale = 1.0;
	std::vector<Hit> hits;
	std::string hoverId, activeId;
	Hit active;
	bool dragging = false;
	QPointF pressPos, lastPos;
	double dragN0 = 0, dragY0 = 0;

	std::vector<ModInfo> mods;
	std::string selId;
	bool simple = false, bandMin = false, single = false;
	int selBand = -1, mbSel = 1, anaMode = 3, range = 30;
	int dragBand = -1, dragCard = -1, dragX = -1;
	double dragF0 = 0, dragG0 = 0, dropX = -1, selNodeY = 0;

	std::map<std::string, std::deque<float>> hist;
	float peakHold[2] = {-90, -90};
	std::vector<float> specPre, specPost;
	Fft fft;

	std::vector<FlatMap> undo;
	int undoPos = -1;
	FlatMap slotA, slotB, presetBase;
	bool onB = false;
	std::string presetName;
	int presetIdx = -1;

	QTimer timer;
	QElapsedTimer loopClock;
	QFont uiFont, monoFont;
};

} // namespace px
