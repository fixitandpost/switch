#include "switcher-ui.hpp"

#include <algorithm>

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QPen>

namespace {
void DrawSwitchModeGlyph(QPainter *painter, const QRect &rect, const QString &glyph, const QColor &color)
{
	if (!painter)
		return;

	QPen pen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter->setPen(pen);
	painter->setBrush(Qt::NoBrush);

	const QRectF r = QRectF(rect).adjusted(3.0, 3.0, -3.0, -3.0);
	if (glyph == QStringLiteral("workspace")) {
		const qreal dot = 2.4;
		for (int y = 0; y < 3; ++y) {
			for (int x = 0; x < 3; ++x) {
				const QPointF center(r.left() + r.width() * (0.2 + x * 0.3),
						     r.top() + r.height() * (0.2 + y * 0.3));
				painter->setBrush(color);
				painter->drawEllipse(center, dot, dot);
			}
		}
		painter->setBrush(Qt::NoBrush);
		return;
	}
	if (glyph == QStringLiteral("vertical")) {
		painter->drawRoundedRect(r.adjusted(3.0, 0.0, -3.0, 0.0), 2.0, 2.0);
		painter->drawLine(QPointF(r.center().x() - 2.5, r.bottom() - 2.0),
				  QPointF(r.center().x() + 2.5, r.bottom() - 2.0));
		return;
	}
	if (glyph == QStringLiteral("motion")) {
		painter->drawEllipse(r.adjusted(2.0, 2.0, -2.0, -2.0));
		painter->drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.top() + 5.0));
		painter->drawLine(QPointF(r.center().x(), r.bottom() - 5.0), QPointF(r.center().x(), r.bottom()));
		painter->drawLine(QPointF(r.left(), r.center().y()), QPointF(r.left() + 5.0, r.center().y()));
		painter->drawLine(QPointF(r.right() - 5.0, r.center().y()), QPointF(r.right(), r.center().y()));
		painter->setBrush(color);
		painter->drawEllipse(r.center(), 2.2, 2.2);
		painter->setBrush(Qt::NoBrush);
		return;
	}

	painter->drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
	painter->drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
	painter->drawLine(QPointF(r.left() + 2.0, r.top() + 2.0), QPointF(r.right() - 2.0, r.bottom() - 2.0));
	painter->drawLine(QPointF(r.right() - 2.0, r.top() + 2.0), QPointF(r.left() + 2.0, r.bottom() - 2.0));
}

} // namespace

QColor SwitchUi::WithAlpha(QColor color, int alpha)
{
	color.setAlpha(std::clamp(alpha, 0, 255));
	return color;
}

QColor SwitchUi::Blend(const QColor &first, const QColor &second, qreal ratio)
{
	ratio = std::clamp(ratio, 0.0, 1.0);
	const auto inverse = 1.0 - ratio;
	return QColor::fromRgbF(first.redF() * inverse + second.redF() * ratio,
				 first.greenF() * inverse + second.greenF() * ratio,
				 first.blueF() * inverse + second.blueF() * ratio,
				 first.alphaF() * inverse + second.alphaF() * ratio);
}

QString SwitchUi::CssColor(const QColor &color)
{
	return QStringLiteral("rgba(%1, %2, %3, %4)")
		.arg(color.red())
		.arg(color.green())
		.arg(color.blue())
		.arg(color.alpha());
}

SwitchModeItemDelegate::SwitchModeItemDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize SwitchModeItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	(void)option;
	(void)index;
	return QSize(0, 52);
}

void SwitchModeItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
				   const QModelIndex &index) const
{
	if (!painter)
		return;

	const QRect rowRect = option.rect.adjusted(0, 4, 0, -4);
	const bool selected = option.state.testFlag(QStyle::State_Selected);
	const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
	const QColor highlight = option.palette.color(QPalette::Highlight);
	const QColor textColor =
		selected ? option.palette.color(QPalette::HighlightedText) : option.palette.color(QPalette::WindowText);
	const int rowCenterY = rowRect.center().y();

	painter->save();
	painter->setRenderHint(QPainter::Antialiasing, true);
	if (selected || hovered) {
		const QColor fill = selected ? highlight : SwitchUi::WithAlpha(highlight, 34);
		const QColor border = selected ? highlight : SwitchUi::WithAlpha(highlight, 128);
		painter->setPen(QPen(border, 1.0));
		painter->setBrush(fill);
		painter->drawRoundedRect(rowRect.adjusted(0, 0, -1, -1), 4.0, 4.0);
	}

	const QRect content = rowRect.adjusted(18, 0, 14, 0);
	const QSize iconSize(24, 24);
	const QRect iconRect(content.left(), rowCenterY - iconSize.height() / 2, iconSize.width(),
			     iconSize.height());

	QFont font = option.font;
	font.setWeight(QFont::DemiBold);
	const QFontMetrics metrics(font);
	const int textLeft = iconRect.right() + 14;
	const QRect textRect(textLeft, rowRect.top(), content.right() - textLeft, rowRect.height());
	const QString title = metrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
						 textRect.width());

	DrawSwitchModeGlyph(painter, iconRect, index.data(kSwitchModeGlyphRole).toString(), textColor);
	painter->setFont(font);
	painter->setPen(textColor);
	painter->setClipRect(rowRect);
	painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, title);
	painter->restore();
}
