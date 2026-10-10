#ifndef CLIENT_RENDERCVARS_HPP
#define CLIENT_RENDERCVARS_HPP

// The render options the client registers at startup (FUN_1405dcf20, FUN_14023caf0,
// FUN_1412a2bf0) and the ambient occlusion tuning the world registers (FUN_142557ae0).
void FullScreenEffectsRegisterCVars();
void FrameRateRegisterCVars();
void ResampleRegisterCVars();
void AmbientOcclusionRegisterCVars();

#endif
