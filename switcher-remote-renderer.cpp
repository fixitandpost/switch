#include "switcher-remote-renderer.hpp"

#include <QBuffer>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>

#include <graphics/graphics.h>
#include <obs-frontend-api.h>

namespace {
constexpr int kRemoteMargin = 18;
constexpr int kRemoteSpacing = 14;
constexpr int kRemoteHeaderHeight = 34;

QColor TileBorderColor(bool program, bool preview, bool selected)
{
	if (program && preview)
		return QColor(QStringLiteral("#f97316"));
	if (program)
		return QColor(QStringLiteral("#ef4444"));
	if (preview)
		return QColor(QStringLiteral("#22c55e"));
	if (selected)
		return QColor(QStringLiteral("#f8fafc"));
	return QColor(QStringLiteral("#334155"));
}

QColor TileLabelBackground(bool hasSource, bool program, bool preview)
{
	if (!hasSource)
		return QColor(15, 23, 42, 225);
	if (program && preview)
		return QColor(249, 115, 22, 220);
	if (program)
		return QColor(220, 38, 38, 220);
	if (preview)
		return QColor(22, 163, 74, 220);
	return QColor(15, 23, 42, 210);
}

static inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	int newCX;
	int newCY;

	const double windowAspect = double(windowCX) / double(windowCY);
	const double baseAspect = double(baseCX) / double(baseCY);

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

bool SourceInActiveTree(obs_source_t *root, obs_source_t *target)
{
	if (!root || !target)
		return false;
	if (root == target)
		return true;

	struct SearchContext {
		obs_source_t *target = nullptr;
		bool found = false;
	} context{target, false};

	obs_source_enum_active_tree(
		root,
		[](obs_source_t *parent, obs_source_t *child, void *data) {
			UNUSED_PARAMETER(parent);
			auto *ctx = static_cast<SearchContext *>(data);
			if (ctx->target == child)
				ctx->found = true;
		},
		&context);

	return context.found;
}
} // namespace

std::vector<SwitcherRemoteTileLayout> BuildSwitcherRemoteTileLayout(int slotCount, const QSize &size)
{
	std::vector<SwitcherRemoteTileLayout> layout;
	if (slotCount <= 0 || size.width() <= 0 || size.height() <= 0)
		return layout;

	const int dimension = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(slotCount)))));
	const int availableWidth = std::max(1, size.width() - (kRemoteMargin * 2) - (kRemoteSpacing * (dimension - 1)));
	const int availableHeight = std::max(1, size.height() - (kRemoteMargin * 2) - (kRemoteSpacing * (dimension - 1)));
	const int cellWidth = std::max(1, availableWidth / dimension);
	const int cellHeight = std::max(1, availableHeight / dimension);

	layout.reserve(static_cast<size_t>(slotCount));
	for (int index = 0; index < slotCount; index++) {
		const int row = index / dimension;
		const int column = index % dimension;
		const int x = kRemoteMargin + column * (cellWidth + kRemoteSpacing);
		const int y = kRemoteMargin + row * (cellHeight + kRemoteSpacing);
		QRect rect(x, y, cellWidth, cellHeight);
		layout.push_back({index,
				  rect,
				  QRectF(double(rect.x()) / double(size.width()), double(rect.y()) / double(size.height()),
					 double(rect.width()) / double(size.width()), double(rect.height()) / double(size.height()))});
	}

	return layout;
}

SwitcherRemoteRenderer::~SwitcherRemoteRenderer()
{
	DestroyResources();
}

void SwitcherRemoteRenderer::Reset()
{
	DestroyResources();
	renderSize = {};
}

bool SwitcherRemoteRenderer::EnsureResources(const QSize &size)
{
	if (size == renderSize && texrender && stagesurface)
		return true;

	if (stagesurface) {
		gs_stagesurface_destroy(stagesurface);
		stagesurface = nullptr;
	}
	if (texrender) {
		gs_texrender_destroy(texrender);
		texrender = nullptr;
	}
	renderSize = size;

	texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	stagesurface = gs_stagesurface_create(static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()), GS_BGRA);
	return texrender && stagesurface;
}

void SwitcherRemoteRenderer::DestroyResources()
{
	if (!texrender && !stagesurface)
		return;

	obs_enter_graphics();
	if (stagesurface) {
		gs_stagesurface_destroy(stagesurface);
		stagesurface = nullptr;
	}
	if (texrender) {
		gs_texrender_destroy(texrender);
		texrender = nullptr;
	}
	obs_leave_graphics();
}

QByteArray SwitcherRemoteRenderer::RenderJpeg(const std::vector<SwitcherRemoteRenderSlot> &renderSlots,
					      int selectedSlotIndex, const QSize &size, bool previewProgramMode,
					      obs_source_t *previewSource, obs_source_t *programSource, QString *error)
{
	if (size.width() <= 0 || size.height() <= 0) {
		if (error)
			*error = QStringLiteral("Invalid render size");
		return {};
	}

	QImage image(size, QImage::Format::Format_RGBX8888);
	image.fill(QColor(QStringLiteral("#020617")));

	obs_enter_graphics();

	if (!EnsureResources(size)) {
		obs_leave_graphics();
		if (error)
			*error = QStringLiteral("Unable to allocate remote render resources");
		return {};
	}

	if (gs_texrender_begin(texrender, static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()))) {
		vec4 clearColor;
		vec4_zero(&clearColor);

		gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
		gs_ortho(0.0f, float(size.width()), 0.0f, float(size.height()), -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		const auto layout = BuildSwitcherRemoteTileLayout(static_cast<int>(renderSlots.size()), size);
		for (size_t slotIndex = 0; slotIndex < renderSlots.size() && slotIndex < layout.size(); slotIndex++) {
			const auto &slot = renderSlots[slotIndex];
			if (!slot.source)
				continue;

			const auto &tile = layout[slotIndex].rect;
			const QRect contentRect =
				tile.adjusted(6, 6, -6, -(kRemoteHeaderHeight + 6));

			uint32_t sourceCX = obs_source_get_width(slot.source);
			if (sourceCX == 0)
				sourceCX = 1;
			uint32_t sourceCY = obs_source_get_height(slot.source);
			if (sourceCY == 0)
				sourceCY = 1;

			int x = 0;
			int y = 0;
			float scale = 1.0f;
			GetScaleAndCenterPos(static_cast<int>(sourceCX), static_cast<int>(sourceCY), contentRect.width(),
					     contentRect.height(), x, y, scale);

			const int viewportX = contentRect.x() + x;
			const int viewportY = contentRect.y() + y;
			const int viewportW = std::max(1, static_cast<int>(std::round(scale * float(sourceCX))));
			const int viewportH = std::max(1, static_cast<int>(std::round(scale * float(sourceCY))));

			gs_viewport_push();
			gs_projection_push();
			const bool previousLinear = gs_set_linear_srgb(true);
			gs_set_viewport(viewportX, viewportY, viewportW, viewportH);
			obs_source_inc_showing(slot.source);
			obs_source_video_render(slot.source);
			obs_source_dec_showing(slot.source);
			gs_set_linear_srgb(previousLinear);
			gs_projection_pop();
			gs_viewport_pop();
		}

		gs_blend_state_pop();
		gs_texrender_end(texrender);
	}

	gs_stage_texture(stagesurface, gs_texrender_get_texture(texrender));

	uint8_t *videoData = nullptr;
	uint32_t videoLinesize = 0;
	if (gs_stagesurface_map(stagesurface, &videoData, &videoLinesize)) {
		for (int y = 0; y < size.height(); y++) {
			memcpy(image.scanLine(y), videoData + (y * static_cast<int>(videoLinesize)),
			       static_cast<size_t>(image.bytesPerLine()));
		}
		gs_stagesurface_unmap(stagesurface);
	}

	obs_leave_graphics();

	const auto layout = BuildSwitcherRemoteTileLayout(static_cast<int>(renderSlots.size()), size);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);

	QFont titleFont = painter.font();
	titleFont.setPointSize(11);
	titleFont.setBold(true);
	painter.setFont(titleFont);

	for (size_t slotIndex = 0; slotIndex < renderSlots.size() && slotIndex < layout.size(); slotIndex++) {
		const auto &slot = renderSlots[slotIndex];
		const auto &tile = layout[slotIndex].rect;
		const bool hasSource = slot.source != nullptr;
		const bool preview = hasSource &&
				     ((previewProgramMode && SourceInActiveTree(previewSource, slot.source)) ||
				      (!previewProgramMode && static_cast<int>(slotIndex) == selectedSlotIndex));
		const bool program = hasSource && SourceInActiveTree(programSource, slot.source);
		const bool selected = static_cast<int>(slotIndex) == selectedSlotIndex;

		const QColor borderColor = TileBorderColor(program, preview, selected);
		painter.setPen(QPen(borderColor, selected ? 4.0 : 3.0));
		painter.drawRoundedRect(tile.adjusted(1, 1, -1, -1), 14, 14);

		const QRect headerRect(tile.left() + 6, tile.bottom() - (kRemoteHeaderHeight + 6), tile.width() - 12, kRemoteHeaderHeight);
		painter.setPen(Qt::NoPen);
		painter.setBrush(TileLabelBackground(hasSource, program, preview));
		painter.drawRoundedRect(headerRect, 10, 10);

		if (!hasSource) {
			painter.setPen(QColor(QStringLiteral("#94a3b8")));
			painter.drawText(tile.adjusted(18, 18, -18, -(kRemoteHeaderHeight + 24)), Qt::AlignCenter | Qt::TextWordWrap,
					 QStringLiteral("Empty slot"));
		}

		painter.setPen(QColor(QStringLiteral("#f8fafc")));
		painter.drawText(headerRect.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::TextSingleLine, slot.title);

		QString badge;
		if (program && preview)
			badge = QStringLiteral("PROGRAM + PREVIEW");
		else if (program)
			badge = previewProgramMode ? QStringLiteral("PROGRAM") : QStringLiteral("LIVE");
		else if (preview)
			badge = QStringLiteral("PREVIEW");
		else if (selected)
			badge = QStringLiteral("SELECTED");

		if (!badge.isEmpty()) {
			QFont badgeFont = titleFont;
			badgeFont.setPointSize(9);
			badgeFont.setBold(true);
			painter.setFont(badgeFont);
			QRect badgeRect = headerRect.adjusted(12, 6, -12, -6);
			painter.drawText(badgeRect, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, badge);
			painter.setFont(titleFont);
		}
	}

	painter.end();

	QByteArray jpg;
	QBuffer buffer(&jpg);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "JPEG", 80);
	return jpg;
}
