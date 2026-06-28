#pragma once

#include <QColor>
#include <QString>
#include <QStyledItemDelegate>

constexpr int kSwitchModeGlyphRole = Qt::UserRole + 10;

namespace SwitchUi {
QColor WithAlpha(QColor color, int alpha);
QColor Blend(const QColor &first, const QColor &second, qreal ratio);
QString CssColor(const QColor &color);
} // namespace SwitchUi

class SwitchModeItemDelegate : public QStyledItemDelegate {
public:
	explicit SwitchModeItemDelegate(QObject *parent = nullptr);

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
