#pragma once

#include <QByteArray>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <vector>

#include <graphics/graphics.h>

#include "obs.hpp"

struct SwitcherRemoteRenderSlot {
	int index = 0;
	OBSSource source = nullptr;
	QString title;
};

struct SwitcherRemoteTileLayout {
	int index = 0;
	QRect rect;
	QRectF normalizedRect;
};

std::vector<SwitcherRemoteTileLayout> BuildSwitcherRemoteTileLayout(int slotCount, const QSize &size);

class SwitcherRemoteRenderer {
public:
	SwitcherRemoteRenderer() = default;
	~SwitcherRemoteRenderer();

	QByteArray RenderJpeg(const std::vector<SwitcherRemoteRenderSlot> &renderSlots, int selectedSlotIndex,
			      const QSize &size, bool previewProgramMode, obs_source_t *previewSource,
			      obs_source_t *programSource,
			      QString *error = nullptr);

	void Reset();

private:
	bool EnsureResources(const QSize &size);
	void DestroyResources();

	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stagesurface = nullptr;
	QSize renderSize;
};
