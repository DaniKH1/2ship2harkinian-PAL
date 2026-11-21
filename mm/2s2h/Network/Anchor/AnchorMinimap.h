#ifndef NETWORK_ANCHOR_ANCHOR_MINIMAP_H
#define NETWORK_ANCHOR_ANCHOR_MINIMAP_H

#include "z64.h"

#define ANCHOR_MINIMAP_MAX_PLAYERS 16

typedef struct {
    f32 worldX;
    f32 worldZ;
    u8 colorR;
    u8 colorG;
    u8 colorB;
    s16 sceneId;
    s16 rotY;
} AnchorMinimapRemotePlayer;

#ifdef __cplusplus
extern "C" {
#endif
s32 Ship_GetMinimapRemotePlayers(AnchorMinimapRemotePlayer* out, s32 maxCount);
#ifdef __cplusplus
}
#endif

#endif
