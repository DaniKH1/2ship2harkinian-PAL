#include "Anchor.h"
#include "2s2h/NameTag/NameTag.h"
#include "2s2h/Enhancements/FrameInterpolation/FrameInterpolation.h"
#include <ship/Context.h>
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "z64malloc.h"
extern PlayState* gPlayState;

#include "objects/object_test3/object_test3.h"
#include "objects/gameplay_keep/gameplay_keep.h"
extern PlayerAgeProperties sPlayerAgeProperties[PLAYER_FORM_MAX];
extern TexturePtr sPlayerEyesTextures[PLAYER_FORM_MAX][PLAYER_EYES_MAX];
extern EffectBlureInit2 D_8085D30C;
extern EffectTireMarkInit D_8085D330;
void Player_DrawGameplay(PlayState* play, Player* player, s32 lod, Gfx* cullDList,
                         OverrideLimbDrawFlex overrideLimbDraw);
void Player_Anim_PlayOnceMorph(PlayState* play, Player* player, PlayerAnimationHeader* anim);
PlayerAnimationHeader* Player_GetIdleAnim(Player* player);
}

// Hijacking player->zTargetActiveTimer (unused s32 for the dummy) to store the clientId for convenience
#define DUMMY_CLIENT_ID player->zTargetActiveTimer

static DamageTable DummyPlayerDamageTable = {
    /* Deku Nut       */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Deku Stick     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Horse trample  */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Explosives     */ DMG_ENTRY(3, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Zora boomerang */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Normal arrow   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* UNK_DMG_0x06   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Hookshot       */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Goron punch    */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_KNOCKBACK_SMALL),
    /* Sword          */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Goron pound    */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Fire arrow     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice arrow      */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light arrow    */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Goron spikes   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Deku spin      */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Deku bubble    */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Deku launch    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_KNOCKBACK_SMALL),
    /* UNK_DMG_0x12   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Zora barrier   */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Normal shield  */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_NONE),
    /* Light ray      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_NONE),
    /* Thrown object  */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Zora punch     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Spin attack    */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Sword beam     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Normal Roll    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_NONE),
    /* UNK_DMG_0x1B   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* UNK_DMG_0x1C   */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_NONE),
    /* Unblockable    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_NONE),
    /* UNK_DMG_0x1E   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Powder Keg     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
};

// Returns the instrument animation for the given form, matching what the real Player would be playing.
// Goron drums / Zora guitar / Deku pipes / human-form ocarina are only drawn while skelAnime.animation is one
// of these.
static void* DummyPlayer_GetInstrumentAnim(uint8_t form, uint8_t anim) {
    switch (form) {
        case PLAYER_FORM_GORON:
            return anim == 1   ? (void*)&gPlayerAnim_pg_gakkistart
                   : anim == 2 ? (void*)&gPlayerAnim_pg_gakkiplay
                   : anim == 3 ? (void*)&gPlayerAnim_pg_gakkiwait
                               : nullptr;
        case PLAYER_FORM_ZORA:
            return anim == 1   ? (void*)&gPlayerAnim_pz_gakkistart
                   : anim == 2 ? (void*)&gPlayerAnim_pz_gakkiplay
                               : nullptr;
        case PLAYER_FORM_DEKU:
            return anim == 1   ? (void*)&gPlayerAnim_pn_gakkistart
                   : anim == 2 ? (void*)&gPlayerAnim_pn_gakkiplay
                               : nullptr;
        case PLAYER_FORM_HUMAN:
            return anim == 4 ? (void*)&gPlayerAnim_link_normal_okarina_swing : nullptr;
        default:
            return nullptr;
    }
}

// Modeled after EnTest3_Init and Player_Init
void DummyPlayer_Init(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->actorIndexToClientId[actor->params];
    DUMMY_CLIENT_ID = clientId;

    if (!Anchor::Instance->clients.contains(DUMMY_CLIENT_ID)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[DUMMY_CLIENT_ID];

    player->actor.room = -1;
    player->csId = CS_ID_NONE;
    player->transformation = client.transformation;
    player->ageProperties = &sPlayerAgeProperties[player->transformation];
    player->heldItemAction = PLAYER_IA_NONE;
    player->heldItemId = ITEM_OCARINA_OF_TIME;

    Player_SetModelGroup(player, PLAYER_MODELGROUP_DEFAULT);
    play->playerInit(player, play, gPlayerSkeletons[player->transformation]);

    // Skipping Effect_Add(...), the dummy doesn't need weapon effects

    player->maskObjectSegment = ZeldaArena_Malloc(0x3800);
    // Skipping part of play->func_18780, specifically Player_SetAction
    Player_Anim_PlayOnceMorph(play, player, Player_GetIdleAnim(player));
    player->yaw = player->actor.shape.rot.y;

    // Ensures the actor is always updating/drawing even when culled/out of distance
    actor->flags =
        ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED | ACTOR_FLAG_INSIDE_CULLING_VOLUME;
    player->cylinder.base.acFlags = AC_ON | AC_TYPE_PLAYER;
    player->cylinder.base.ocFlags2 = OC2_TYPE_1;
    player->cylinder.elem.acElemFlags = ACELEM_ON | ACELEM_HOOKABLE | ACELEM_NO_HITMARK;
    player->actor.flags |= ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER;
    player->cylinder.dim.radius = 30;
    player->actor.colChkInfo.damageTable = &DummyPlayerDamageTable;

    bool isGlobalRoom = (std::string("2ship-global") == CVarGetString("gNetwork.Anchor.RoomId", ""));
    if (!isGlobalRoom) {
        NameTag_RegisterForActorWithOptions(actor, client.name.c_str(), { .yOffset = 30 });
    }
}

// Update the actor with new data from the client
void DummyPlayer_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    if (!Anchor::Instance->clients.contains(DUMMY_CLIENT_ID)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[DUMMY_CLIENT_ID];

    if (client.sceneId != gPlayState->sceneId || !client.online || !client.isSaveLoaded) {
        actor->world.pos.x = -9999.0f;
        actor->world.pos.y = -9999.0f;
        actor->world.pos.z = -9999.0f;
        actor->shape.shadowAlpha = 0;
        return;
    }

    actor->shape.shadowAlpha = 255;
    Math_Vec3s_Copy(&actor->shape.rot, &client.posRot.rot);
    Math_Vec3f_Copy(&actor->world.pos, &client.posRot.pos);
    // Write the joints where the skeleton reads them (skelAnime.jointTable is the ALIGN16 of the
    // buffer), so the dummy draws the same pose as the remote.
    memcpy(player->skelAnime.jointTable, client.jointTable, sizeof(client.jointTable));
    memcpy(player->skelAnimeUpper.jointTable, client.upperJointTable, sizeof(client.upperJointTable));
    player->maskObjectLoadState = 0;
    player->maskId = player->currentMask;
    player->currentMask = client.currentMask;
    player->rightHandType = client.rightHandType;
    player->leftHandType = client.leftHandType;
    player->currentShield = client.currentShield;
    player->sheathType = client.sheathType;
    player->heldItemAction = client.heldItemAction;
    player->heldItemId = client.heldItemId;
    player->itemAction = client.itemAction;
    player->stateFlags1 = client.stateFlags1;
    player->stateFlags2 = client.stateFlags2;
    player->stateFlags3 = client.stateFlags3;
    player->skelAnime.movementFlags = client.skelAnimeMovementFlags;
    player->unk_AB8 = client.unk_AB8;

    // The Kamaro/Bremen songs for remote users are synced one-way through PLAYER_SFX
    // packets, so when the remote player stops dancing/marching nothing tells the local to
    // stop. Detect the end of the dance/march and stop the song on the local.
    bool isKamaroDancing = (player->stateFlags2 & PLAYER_STATE2_2000000) != 0;
    bool isBremenMarching = (player->stateFlags3 & PLAYER_STATE3_20000000) != 0;
    if (client.wasKamaroDancing && !isKamaroDancing) {
        SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_FANFARE, 0);
    }
    if (client.wasBremenMarching && !isBremenMarching) {
        SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_FANFARE, 0);
    }
    client.wasKamaroDancing = isKamaroDancing;
    client.wasBremenMarching = isBremenMarching;

    // While the remote is using the mask, the song fades out as the local moves away from it
    if (isKamaroDancing || isBremenMarching) {
        Player* localPlayer = GET_PLAYER(gPlayState);
        if (localPlayer != nullptr) {
            f32 distance = Math_Vec3f_DistXYZ(&localPlayer->actor.world.pos, &player->actor.world.pos);
            f32 fadeDistance = 150.0f * CVarGetFloat("gNetwork.Anchor.SfxDistanceMultiplier", 1.0f);
            f32 volume = fadeDistance > 0.0f ? 1.0f - distance / fadeDistance : (distance <= 0.0f ? 1.0f : 0.0f);
            if (volume < 0.05f) {
                volume = 0.05f;
            }
            u8 channelVolume = (u8)(volume * 0x7F);
            for (int channelIndex = 0; channelIndex < 16; channelIndex++) {
                SEQCMD_SET_CHANNEL_VOLUME(SEQ_PLAYER_FANFARE, channelIndex, 0, channelVolume);
            }
        }
    }
    player->unk_B0C = client.unk_B0C;
    player->unk_B28 = client.unk_B28;
    player->unk_ACC = client.unk_ACC;
    player->invincibilityTimer = client.invincibilityTimer;

    // Instruments (Goron drums, Zora guitar, Deku pipes)
    void* instrumentAnim = DummyPlayer_GetInstrumentAnim(client.transformation, client.instrumentAnim);
    if (instrumentAnim != nullptr) {
        player->skelAnime.animation = instrumentAnim;
        player->skelAnime.curFrame = client.curFrame;
        player->unk_B8A = client.unk_B8A;
        player->unk_B86[0] = client.unk_B86[0];
        player->unk_B86[1] = client.unk_B86[1];
        memcpy(player->unk_B10, client.unk_B10, sizeof(client.unk_B10));
    } else {
        player->skelAnime.animation = Player_GetIdleAnim(player);
    }

    // Skin: apply the remote player's skin (best effort - skeletons are per-form)
    if (client.skinId != client.dummySkinApplied) {
        client.dummySkinApplied = client.skinId;
        FlexSkeletonHeader* skinSkel = gPlayerSkeletons[player->transformation];
        if (client.skinId == 1) {
            // Kafei: load the Kafei skeleton (cached) so the dummy uses it instead of Link's
            static FlexSkeletonHeader* sKafeiSkelPtr = nullptr;
            if (sKafeiSkelPtr == nullptr) {
                auto gKafeiSkelResource =
                    Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(gKafeiSkel);
                if (gKafeiSkelResource != nullptr) {
                    sKafeiSkelPtr = (FlexSkeletonHeader*)gKafeiSkelResource->GetRawPointer();
                }
            }
            if (sKafeiSkelPtr != nullptr) {
                skinSkel = sKafeiSkelPtr;
            }
        }
        play->playerInit(player, play, skinSkel);
    }

    // Generate a horse for the remote player while they "call" it: mounted on the
    // horse, or playing the ocarina (the Song of Epona teleports the horse to the
    // player). The local player cannot interact with it (ENHORSE_UNRIDEABLE blocks mounting).
    if ((client.stateFlags1 & PLAYER_STATE1_800000) || (client.stateFlags2 & PLAYER_STATE2_USING_OCARINA)) {
        if (client.remoteHorse == nullptr || client.remoteHorse->update == nullptr) {
            client.remoteHorse = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_HORSE, actor->world.pos.x,
                                             actor->world.pos.y, actor->world.pos.z, 0, actor->shape.rot.y, 0,
                                             ENHORSE_PARAMS(ENHORSE_PARAM_4000, ENHORSE_2));
            if (client.remoteHorse != nullptr) {
                EnHorse* remoteHorse = (EnHorse*)client.remoteHorse;
                remoteHorse->stateFlags |= ENHORSE_UNRIDEABLE;
            }
        }
        if (client.remoteHorse != nullptr) {
            client.remoteHorse->world.pos = actor->world.pos;
        }
    } else if (client.remoteHorse != nullptr) {
        Actor_Kill(client.remoteHorse);
        client.remoteHorse = nullptr;
    }

    PlayerItemAction modelItemAction =
        (PlayerItemAction)(player->itemAction > PLAYER_IA_MINUS1 ? player->itemAction : player->heldItemAction);
    if (modelItemAction < PLAYER_IA_NONE || modelItemAction >= PLAYER_IA_MAX) {
        modelItemAction = PLAYER_IA_NONE;
    }
    Player_SetModels(player, Player_ActionToModelGroup(player, modelItemAction));

    if (Anchor::Instance->roomState.pvpMode == 0 ||
        (Anchor::Instance->roomState.pvpMode == 1 &&
         client.teamId == CVarGetString("gNetwork.Anchor.TeamId", "default"))) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    actor->flags &= ~ACTOR_FLAG_LOCK_ON_DISABLED;

    if (player->cylinder.base.acFlags & AC_HIT && player->invincibilityTimer == 0) {
        Anchor::Instance->SendPacket_DamagePlayer(client.clientId, player->actor.colChkInfo.damageEffect,
                                                  player->actor.colChkInfo.damage);
        player->actor.colChkInfo.damage = 0;
        if (player->actor.colChkInfo.damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
            Actor_SetColorFilter(&player->actor, 0, 0xFF, 0, 24);
        } else {
            player->invincibilityTimer = 20;
        }
    }

    Collider_UpdateCylinder(&player->actor, &player->cylinder);

    if (!(player->stateFlags2 & PLAYER_STATE2_4000)) {
        if (!(player->stateFlags1 & (PLAYER_STATE1_4 | PLAYER_STATE1_DEAD | PLAYER_STATE1_2000 | PLAYER_STATE1_4000 |
                                     PLAYER_STATE1_800000))) {
            CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);
        }

        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_4000000)) &&
            (player->invincibilityTimer <= 0)) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &player->cylinder.base);

            if (player->invincibilityTimer < 0) {
                CollisionCheck_SetAT(play, &play->colChkCtx, &player->cylinder.base);
            }
        }
    }

    if (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_10000000 | PLAYER_STATE1_20000000)) {
        player->actor.colChkInfo.mass = MASS_IMMOVABLE;
    } else {
        player->actor.colChkInfo.mass = 50;
    }

    Collider_ResetCylinderAC(play, &player->cylinder.base);
}

void DummyPlayer_Draw(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    if (!Anchor::Instance->clients.contains(DUMMY_CLIENT_ID)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[DUMMY_CLIENT_ID];

    if (client.sceneId != gPlayState->sceneId || !client.online || !client.isSaveLoaded) {
        return;
    }

    Player_DrawGameplay(play, player, 1, gCullBackDList, Player_OverrideLimbDrawGameplayDefault);
}

void DummyPlayer_Destroy(Actor* actor, PlayState* play) {
}
