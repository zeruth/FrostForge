#ifndef WORLD_WORLD_CLIP_HPP
#define WORLD_WORLD_CLIP_HPP

// The world's clip distances as the CVars set them (DAT_142d00c30..DAT_142d00c40), each held
// to its range by its setter, and the clamps the scenes apply to what they derive from them.
// The CVars are registered by WorldInitialize (FUN_14134ed50, not ported), so until then the
// values are the static data's: 1, 0.1, 1, 200 and 1.

// ref: FUN_141365000
float WorldGetFarClipCVar();
// ref: FUN_141365010
float WorldGetFarClip2CVar();
// ref: FUN_141365020
float WorldGetFarClip3CVar();
// ref: FUN_141365030
float WorldGetNearClipCVar();

// ref: FUN_141365b60
void WorldSetFarClipCVar(float value);
// ref: FUN_141365b80
void WorldSetFarClip2CVar(float value);
// ref: FUN_141365ba0
void WorldSetFarClip3CVar(float value);
// ref: FUN_141365bc0
void WorldSetC30CVar(float value);
// ref: FUN_141365be0
void WorldSetNearClipCVar();

// The far clip to [705, 5180].
// ref: FUN_141364d70
float WorldClampFarClip(float farClip);
// The second far clip to [1000, 50000].
// ref: FUN_141364d90
float WorldClampFarClip2(float farClip);
// The third to [400, 6000].
// ref: FUN_141364db0
float WorldClampFarClip3(float farClip);
// The near clip: 0.2 always.
// ref: FUN_141364dd0
float WorldGetNearClip();

#endif
