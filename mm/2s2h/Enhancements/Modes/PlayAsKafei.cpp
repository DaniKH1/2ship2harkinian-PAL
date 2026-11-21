#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"
#include "2s2h/Network/Anchor/Anchor.h"

extern "C" {
#include "variables.h"
#include "objects/object_link_child/object_link_child.h"
#include "objects/object_test3/object_test3.h"

void ResourceMgr_PatchGfxByName(const char* path, const char* patchName, int index, Gfx instruction);
void ResourceMgr_UnpatchGfxByName(const char* path, const char* patchName);
extern TexturePtr sPlayerEyesTextures[PLAYER_FORM_MAX][PLAYER_EYES_MAX];
extern TexturePtr sPlayerMouthTextures[PLAYER_FORM_MAX][PLAYER_MOUTH_MAX];
extern Gfx* gPlayerLeftHandClosedDLs[2 * PLAYER_FORM_MAX];
extern Gfx* gPlayerRightHandClosedDLs[2 * PLAYER_FORM_MAX];
extern Gfx* gPlayerLeftHandOpenDLs[2 * PLAYER_FORM_MAX];
extern Gfx* gPlayerRightHandOpenDLs[2 * PLAYER_FORM_MAX];
}

static FlexSkeletonHeader gLinkHumanSkelBackup;
static FlexSkeletonHeader gKafeiSkelBackup;

#define CVAR_NAME "gModes.PlayAsKafei"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

static Gfx* sLeftHandClosedBackup[2] = { nullptr, nullptr };
static Gfx* sRightHandClosedBackup[2] = { nullptr, nullptr };
static Gfx* sLeftHandOpenBackup[2] = { nullptr, nullptr };
static Gfx* sRightHandOpenBackup[2] = { nullptr, nullptr };
static bool sHandBackupsTaken = false;

void ApplyKafeiSkinParts(bool enabled) {
    if (enabled) {
        // Patch the waist DL to call Kafei's waist DL (ensures pelvis mesh loads)
        ResourceMgr_PatchGfxByName(gLinkHumanWaistDL, "gLinkHumanWaistDL0", 0,
                                    gsSPDisplayListOTRFilePath(gKafeiWaistDL));
        ResourceMgr_PatchGfxByName(gLinkHumanWaistDL, "gLinkHumanWaistDL1", 1, gsSPEndDisplayList());

        // Patch hand DLs to use Kafei's hands
        if (!sHandBackupsTaken) {
            sLeftHandClosedBackup[0] = gPlayerLeftHandClosedDLs[8];
            sLeftHandClosedBackup[1] = gPlayerLeftHandClosedDLs[9];
            sRightHandClosedBackup[0] = gPlayerRightHandClosedDLs[8];
            sRightHandClosedBackup[1] = gPlayerRightHandClosedDLs[9];
            sLeftHandOpenBackup[0] = gPlayerLeftHandOpenDLs[8];
            sLeftHandOpenBackup[1] = gPlayerLeftHandOpenDLs[9];
            sRightHandOpenBackup[0] = gPlayerRightHandOpenDLs[8];
            sRightHandOpenBackup[1] = gPlayerRightHandOpenDLs[9];
            sHandBackupsTaken = true;
        }
        gPlayerLeftHandClosedDLs[8] = (Gfx*)gKafeiLeftHandDL;
        gPlayerLeftHandClosedDLs[9] = (Gfx*)gKafeiLeftHandDL;
        gPlayerRightHandClosedDLs[8] = (Gfx*)gKafeiRightHandDL;
        gPlayerRightHandClosedDLs[9] = (Gfx*)gKafeiRightHandDL;
        gPlayerLeftHandOpenDLs[8] = (Gfx*)gKafeiLeftHandDL;
        gPlayerLeftHandOpenDLs[9] = (Gfx*)gKafeiLeftHandDL;
        gPlayerRightHandOpenDLs[8] = (Gfx*)gKafeiRightHandDL;
        gPlayerRightHandOpenDLs[9] = (Gfx*)gKafeiRightHandDL;

        sPlayerEyesTextures[PLAYER_FORM_HUMAN][0] = (TexturePtr)gKafeiEyesOpenTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][1] = (TexturePtr)gKafeiEyesHalfTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][2] = (TexturePtr)gKafeiEyesClosedTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][3] = (TexturePtr)gKafeiEyesRightTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][4] = (TexturePtr)gKafeiEyesLeftTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][5] = (TexturePtr)gKafeiEyesUpTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][6] = (TexturePtr)gKafeiEyesDownTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][7] = (TexturePtr)gKafeiEyesWincingTex;

        sPlayerMouthTextures[PLAYER_FORM_HUMAN][0] = (TexturePtr)gKafeiMouthClosedTex;
        sPlayerMouthTextures[PLAYER_FORM_HUMAN][1] = (TexturePtr)gKafeiMouthHalfTex;
        sPlayerMouthTextures[PLAYER_FORM_HUMAN][2] = (TexturePtr)gKafeiMouthOpenTex;
        sPlayerMouthTextures[PLAYER_FORM_HUMAN][3] = (TexturePtr)gKafeiMouthSmileTex;
    } else {
        ResourceMgr_UnpatchGfxByName(gLinkHumanWaistDL, "gLinkHumanWaistDL0");
        ResourceMgr_UnpatchGfxByName(gLinkHumanWaistDL, "gLinkHumanWaistDL1");

        // Restore hand DLs
        if (sHandBackupsTaken) {
            gPlayerLeftHandClosedDLs[8] = sLeftHandClosedBackup[0];
            gPlayerLeftHandClosedDLs[9] = sLeftHandClosedBackup[1];
            gPlayerRightHandClosedDLs[8] = sRightHandClosedBackup[0];
            gPlayerRightHandClosedDLs[9] = sRightHandClosedBackup[1];
            gPlayerLeftHandOpenDLs[8] = sLeftHandOpenBackup[0];
            gPlayerLeftHandOpenDLs[9] = sLeftHandOpenBackup[1];
            gPlayerRightHandOpenDLs[8] = sRightHandOpenBackup[0];
            gPlayerRightHandOpenDLs[9] = sRightHandOpenBackup[1];
        }

        sPlayerEyesTextures[PLAYER_FORM_HUMAN][0] = (TexturePtr)gLinkHumanEyesOpenTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][1] = (TexturePtr)gLinkHumanEyesHalfTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][2] = (TexturePtr)gLinkHumanEyesClosedTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][3] = (TexturePtr)gLinkHumanEyesRightTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][4] = (TexturePtr)gLinkHumanEyesLeftTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][5] = (TexturePtr)gLinkHumanEyesUpTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][6] = (TexturePtr)gLinkHumanEyesDownTex;
        sPlayerEyesTextures[PLAYER_FORM_HUMAN][7] = (TexturePtr)gLinkHumanEyesWincingTex;

        sPlayerMouthTextures[PLAYER_FORM_HUMAN][0] = (TexturePtr)gLinkHumanMouthClosedTex;
        sPlayerMouthTextures[PLAYER_FORM_HUMAN][1] = (TexturePtr)gLinkHumanMouthHalfTex;
        sPlayerMouthTextures[PLAYER_FORM_HUMAN][2] = (TexturePtr)gLinkHumanMouthOpenTex;
        sPlayerMouthTextures[PLAYER_FORM_HUMAN][3] = (TexturePtr)gLinkHumanMouthSmileTex;
    }
}

void UpdatePlayAsKafei() {
    auto gLinkHumanSkelResource = Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(gLinkHumanSkel);
    if (gLinkHumanSkelResource == nullptr || gLinkHumanSkelResource->GetRawPointer() == nullptr) {
        return;
    }
    FlexSkeletonHeader* gLinkHumanSkelPtr = (FlexSkeletonHeader*)gLinkHumanSkelResource->GetRawPointer();
    if (CVAR) {
        memcpy(gLinkHumanSkelPtr, &gKafeiSkelBackup, sizeof(FlexSkeletonHeader));
    } else {
        memcpy(gLinkHumanSkelPtr, &gLinkHumanSkelBackup, sizeof(FlexSkeletonHeader));
    }
    ApplyKafeiSkinParts(CVAR);
}

// Applies the skin selector's choice to the gLinkHumanSkel resource so it persists across zone changes.
// skinId: 0 = vanilla, 1 = Kafei; mod skins (skinId >= 2) are disabled until further notice and fall back to vanilla.
void ApplySkinToResource(int skinId) {
    auto resourceManager = Ship::Context::GetRawInstance()->GetResourceManager();
    auto gLinkHumanSkelResource = resourceManager->LoadResource(gLinkHumanSkel);
    if (gLinkHumanSkelResource == nullptr) {
        return;
    }
    FlexSkeletonHeader* gLinkHumanSkelPtr = (FlexSkeletonHeader*)gLinkHumanSkelResource->GetRawPointer();
    if (gLinkHumanSkelPtr == nullptr) {
        return;
    }

    if (skinId == 1) { // Kafei
        auto gKafeiSkelResource = resourceManager->LoadResource(gKafeiSkel);
        if (gKafeiSkelResource != nullptr) {
            FlexSkeletonHeader* gKafeiSkelPtr = (FlexSkeletonHeader*)gKafeiSkelResource->GetRawPointer();
            if (gKafeiSkelPtr != nullptr) {
                memcpy(gLinkHumanSkelPtr, gKafeiSkelPtr, sizeof(FlexSkeletonHeader));
            }
        }
        ApplyKafeiSkinParts(true);
    } else {
        // Mod skins are disabled until further notice: the Skin Selector only offers
        // "Child Link (vanilla)" and "Kafei", so any stale mod skin selection (skinId >= 2)
        // falls back to vanilla.
        // Vanilla: restore Link's original skeleton so that mod-provided skeletons
        // (if any) take over via the resource manager.
        memcpy(gLinkHumanSkelPtr, &gLinkHumanSkelBackup, sizeof(FlexSkeletonHeader));
        ApplyKafeiSkinParts(false);
        // Restore gPlayerSkeletons[HUMAN] to Link's skeleton resource.
        gPlayerSkeletons[PLAYER_FORM_HUMAN] = gLinkHumanSkelPtr;
    }
}

void RegisterSkinSelectorHooks() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>([]() {
        // Re-apply the selected skin's skeleton to the resource so it persists across zone changes
        if (Anchor::Instance != nullptr) {
            ApplySkinToResource(Anchor::Instance->localSkinId);
        }
    });
}

void RegisterPlayAsKafei() {
    // Even though this isn't run when a cvar is changed, it can still run if ShipInit::InitAll(); is called again,
    // likely in the case of setting a preset or something. So we need to make sure this only runs once.
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    auto gLinkHumanSkelResource = Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(gLinkHumanSkel);
    auto gKafeiSkelResource = Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(gKafeiSkel);
    if (gLinkHumanSkelResource == nullptr || gKafeiSkelResource == nullptr) {
        return;
    }
    FlexSkeletonHeader* gLinkHumanSkelPtr = (FlexSkeletonHeader*)gLinkHumanSkelResource->GetRawPointer();
    FlexSkeletonHeader* gKafeiSkelPtr = (FlexSkeletonHeader*)gKafeiSkelResource->GetRawPointer();
    if (gLinkHumanSkelPtr == nullptr || gKafeiSkelPtr == nullptr) {
        return;
    }

    memcpy(&gLinkHumanSkelBackup, gLinkHumanSkelPtr, sizeof(FlexSkeletonHeader));
    memcpy(&gKafeiSkelBackup, gKafeiSkelPtr, sizeof(FlexSkeletonHeader));

    UpdatePlayAsKafei();

    // Restore the persisted skin selection so it survives restarts. If the
    // PlayAsKafei mode is active, the mode's Kafei model stays in place.
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->localSkinId = static_cast<uint8_t>(CVarGetInteger("gModes.SkinSelectorId", 0));
        if (!CVAR) {
            ApplySkinToResource(Anchor::Instance->localSkinId);
        }
    }

    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>([]() { UpdatePlayAsKafei(); });

    // Register skin selector hooks (re-apply skin on zone change)
    RegisterSkinSelectorHooks();
}

// We only want this running at boot, we don't want this running when the cvar is changed, only on scene destroy
static RegisterShipInitFunc initFunc(RegisterPlayAsKafei, {});
