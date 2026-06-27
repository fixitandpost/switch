#pragma once

#include <QStyledItemDelegate>

constexpr int kSwitchModeGlyphRole = Qt::UserRole + 10;

class SwitchModeItemDelegate : public QStyledItemDelegate {
public:
	explicit SwitchModeItemDelegate(QObject *parent = nullptr);

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
