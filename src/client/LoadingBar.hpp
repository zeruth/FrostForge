#ifndef CLIENT_LOADING_BAR_HPP
#define CLIENT_LOADING_BAR_HPP

// The loading screen's progress bar (the UILoadingScreen effect): its frame, its fill and its
// border, drawn over everything at the bottom of the screen while it is shown.

#include <cstdint>

class GxRenderContext;

// ref: FUN_14022ea60
void LoadingBarRender();
// The bar shown (its three textures loaded the first time), at half; at `progress`; hidden, its
// textures let go. A movie that runs short of data shows it while it waits.
void LoadingBarShow();
void LoadingBarSetProgress(float progress);
void LoadingBarHide();

// The context's 2D state for the loading screen: the UILoadingScreen pipeline for a topology,
// blend and raster state.
// ref: FUN_140232110
void LoadingScreenSetPipeline(GxRenderContext* context, uint8_t topology, int32_t blend, int32_t raster);
// The constants of a 2D pass through `projection` (transposed), with the window's size.
// ref: FUN_1402321b0
void LoadingScreenSetConstants(GxRenderContext* context, const float* projection);
// The viewport (clamped to the unit square in x and y), flagged when it changed.
// ref: FUN_140232c60
void LoadingScreenSetViewport(GxRenderContext* context, float minX, float maxX, float minY, float maxY, float minZ,
    float maxZ);

#endif
