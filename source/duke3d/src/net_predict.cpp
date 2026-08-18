#define NET_PREDICT_CPP_

#include "duke3d.h"
#include "oldnet.h"
#include "net_predict.h"
#ifdef __EMSCRIPTEN__
# include <emscripten.h>
#endif

// Without initializing value as done here, menu sounds might be missing.
int32_t oldnet_predictcontext = NOT_PREDICTABLE;

CREATE_PREDICTED_LINKED_LIST(spritesect);
CREATE_PREDICTED_LINKED_LIST(spritestat);
int32_t predicted_Numsprites;
int16_t predicted_tailspritefree;

actor_t  backupActor;
int32_t  backupSeed;
// [NetDuke32 port] netduke32's separate g_random stream does not exist in this
// tree; randomseed (backupSeed) is the sole RNG state to save/restore.

// ── P2: per-tic sound watermark ─────────────────────────────────────────────
// netduke32's model ("play during PREDICTSTATE_PROCESS, mute CORRECT + the
// authoritative echo") assumes each tic is predicted exactly once before it
// is consumed. Our guest RE-ENTERS a range of tics on every correction and
// can meet a tic first on any of three passes (PROCESS, a CORRECT replay, or
// the free-run authoritative consume), so the exactly-once property must key
// on the TIC, not the pass: an own-action sound for tic T emits only while
// T >= s_psndDonePlc, and every completed sim pass raises the mark past its
// tic. First pass of T plays (instant), every later entry of T is silent.
static int32_t s_psndDonePlc;       // tics below this already emitted own-action sounds
static int32_t s_psndDupPlc = -1;   // in-tic (plc,snd) dedup: tic the ring describes
static int16_t s_psndDupSnd[8];
static int     s_psndDupCnt;

void Net_LocalSoundResetWatermark(void)
{
    s_psndDonePlc = 0;
    s_psndDupPlc  = -1;
    s_psndDupCnt  = 0;
}

void Net_LocalSoundMarkTicDone(int32_t tic)
{
    // plc restarted (level change) without passing Net_ClearFIFO: legal
    // re-entries lag the mark by at most the pending sample window (~100
    // tics, see oldnet.cpp's `capped` gate), so a gap beyond the whole ring
    // is a restart (s_botInvLogPlc idiom). Smaller backward jumps -- heal
    // rewinds re-running already-heard tics -- deliberately keep the mark:
    // a re-entry stays silent no matter which flow re-enters it.
    if (tic + MOVEFIFOSIZ < s_psndDonePlc)
        s_psndDonePlc = tic;
    if (tic >= s_psndDonePlc)
        s_psndDonePlc = tic + 1;
}

int Net_LocalSoundGate(int soundNum, int spriteNum)
{
    if (numplayers < 2)
        return 1;   // solo/demo: prediction never runs, nothing to suppress

    extern int32_t g_netPredictMode, g_netStreamMode, g_netForensics;
    auto const pOwn = g_player[myconnectindex].ps;

    // Claimed = a stream-mode GUEST's own action sound: fired from inside its
    // own seat's P_ProcessInput/P_HandleSharedKeys context (P1 arming) on its
    // own sprite (or as a 2D sound). The host and legacy lockstep keep the
    // blanket rule bit-for-bit: their own tics are played by the
    // authoritative pass exactly as before.
    bool const claimed = (g_netPredictMode & 8) && g_netStreamMode
        && myconnectindex != connecthead
        && oldnet_predictcontext == myconnectindex
        && pOwn != NULL && (spriteNum == -1 || spriteNum == pOwn->i);

    if (!claimed)
        return !oldnet_predicting;   // blanket: replays silent, authoritative plays

    int32_t const tic = oldnet_predicting ? predictfifoplc : movefifoplc - 1;

    int         emit = 0;
    const char *why;
    if (tic < s_psndDonePlc)
        why = oldnet_predicting ? "replay" : "echo";
    else
    {
        if (tic != s_psndDupPlc)
        {
            s_psndDupPlc = tic;
            s_psndDupCnt = 0;
        }
        int dup = 0;
        for (int i = 0; i < s_psndDupCnt; i++)
            if (s_psndDupSnd[i] == (int16_t)soundNum)
                dup = 1;
        if (dup)
            why = "dup";
        else
        {
            if (s_psndDupCnt < ARRAY_SSIZE(s_psndDupSnd))
                s_psndDupSnd[s_psndDupCnt++] = (int16_t)soundNum;
            emit = 1;
            why  = "first";
        }
    }

    if (g_netForensics)
    {
        // emit=1 lines are bounded by real sounds and never rate-limited (the
        // smoke gate counts them); emit=0 echo/replay lines can fire per tic
        // (e.g. the falling-scream retry) and get a small per-window budget.
        static int32_t s_logWindow;
        static int     s_logBudget;
        if (emit)
            LOG_F(INFO, "[psnd] plc=%d snd=%d emit=1 why=%s", tic, soundNum, why);
        else
        {
            int32_t const win = (int32_t)totalclock >> 7;
            if (win != s_logWindow)
            {
                s_logWindow = win;
                s_logBudget = 6;
            }
            if (s_logBudget > 0)
            {
                s_logBudget--;
                LOG_F(INFO, "[psnd] plc=%d snd=%d emit=0 why=%s", tic, soundNum, why);
            }
        }
    }

    return emit;
}

static void Net_ProcessPrediction(void)
{
#ifdef __EMSCRIPTEN__
    // Wedge forensics: the joiner's first predicted tics hard-looped getzrange
    // (paused stack, live-reported joins). Log the state each early replica
    // tick enters P_ProcessInput with; only the first few MP ticks ever print.
    { static int s_pn; if (s_pn < 8) { s_pn++;
        EM_ASM({ console.log('[pred] tick plc=' + $0 + ' sect=' + $1 + ' spr=' + $2 + ' x=' + $3 + ' y=' + $4 + ' z=' + $5); },
               predictfifoplc, predictedPlayer.cursectnum, predictedPlayer.i,
               predictedPlayer.pos.x, predictedPlayer.pos.y, predictedPlayer.pos.z); } }
#endif
    input_t backupInput = g_player[myconnectindex].input;

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    // Canonical stream: a slave's consume fifo holds the master's echo (its own
    // column included, RTT late). The predictor exists to hide exactly that
    // latency, so it replays the LOCALLY SAMPLED inputs from g_netSendRing.
    // The master's samples go straight into its fifo, which stays its source.
    g_player[myconnectindex].input = (numplayers > 1 && myconnectindex != connecthead)
        ? g_netSendRing[INPUTFIFO_PREDICTTICK]
        : inputfifo[INPUTFIFO_PREDICTTICK][myconnectindex];
#else
    g_player[myconnectindex].input = inputfifo[INPUTFIFO_PREDICTTICK][myconnectindex];
#endif

    // Clear bits that could cause trouble
    g_player[myconnectindex].input.bits &= ~(BIT(SK_PAUSE) | BIT(SK_MULTIFLAG) | BIT(SK_GAMEQUIT));

    // View direction is FRAME-OWNED: P_GetInput applies avel/horz to the
    // predicted view at frame rate as the mouse moves. If the predicted tic
    // pass integrated them again (the open tick's partial input included),
    // every correction replay re-derived a different angle and the view shook
    // with mouse speed. The sim's copy of these inputs is untouched -- only
    // the predicted replica skips angle integration; its position physics run
    // from the frame-integrated angles it already has.
    g_player[myconnectindex].input.q16avel = 0;
    g_player[myconnectindex].input.q16horz = 0;

    g_player[myconnectindex].ps = &predictedPlayer;

    P_HandleSharedKeys(myconnectindex);
    if (ud.pause_on == 0)
    {
        //X_OnEvent(EVENT_FAKEDOMOVETHINGS, g_player[myconnectindex].ps->i, myconnectindex, -1);
        P_ProcessInput(myconnectindex);
        G_MovePlayerSprite(myconnectindex);
    }

    g_player[myconnectindex].input = backupInput;

#if 0
    predictBackup[INPUTFIFO_PREDICTTICK].pos = predictedPlayer.pos;
    predictBackup[INPUTFIFO_PREDICTTICK].q16ang = predictedPlayer.q16ang;
    predictBackup[INPUTFIFO_PREDICTTICK].q16horiz = predictedPlayer.q16horiz;
#endif

    // [P2] This tic's own-action sounds (if any) are spent: later re-entries
    // (correction replays, the authoritative echo) stay silent.
    Net_LocalSoundMarkTicDone(predictfifoplc);
    predictfifoplc++;
}

static void Net_ResetPredictionData(void)
{
    DukePlayer_t *p  = g_player[myconnectindex].ps;
    predictedPlayer  = *p;
    predicted_pActor = actor[p->i];

    memcpy(predicted_sprite, original_sprite, sizeof(predicted_sprite));

    reset_predicted_linked_list(spritesect);
    reset_predicted_linked_list(spritestat);
    predicted_Numsprites     = Numsprites;
    predicted_tailspritefree = tailspritefree;

    for (int i = g_gameVarCount - 1; i >= 0; i--)
    {
        if (aGameVars[i].flags & (/*GAMEVAR_READONLY |*/ GAMEVAR_SPECIAL | GAMEVAR_NOMULTI | GAMEVAR_PTR_MASK))
            continue;

        if (aGameVars[i].flags & GAMEVAR_PERPLAYER)
            Bmemcpy(predicted_pValues[i], original_pValues[i], sizeof(intptr_t) * MAXPLAYERS);
        else if (aGameVars[i].flags & GAMEVAR_PERACTOR)
            Bmemcpy(predicted_pValues[i], original_pValues[i], sizeof(intptr_t) * MAXSPRITES);
        else
            predicted_lValue[i] = aGameVars[i].global;  // [port] this tree's gamevar scalar is .global (netduke32: .lValue)
    }
}

void Net_CorrectPrediction(void)
{
    if (numplayers < 2)
        return;

    DukePlayer_t *p = g_player[myconnectindex].ps;   // original pointers active: the SIM copy

    // [P4] IDLE CORRECTION DEADBAND (UT99 netcode audit 2026-08-18). The
    // reset+replay below rebases the predicted RENDER copy onto the freshly
    // consumed authoritative tic UNCONDITIONALLY, ~30x/sec. When the guest is
    // standing still that reimports the sim copy's per-tic micro-variation
    // (floor z-snap, sector relink, opos/pos flip) into the rendered view every
    // tic -- the reported "flashing while standstill" shimmer. UT sends/applies
    // no correction when the position error is tiny. So: for a stream guest
    // whose own player is effectively stationary AND whose predicted copy
    // already matches the authoritative one within a deadband, SKIP the rebase
    // and let the per-frame PROCESS pass (Net_DoPrediction at G_MoveLoop) carry
    // the idle predicted state forward unchanged -- no per-tic jitter to import.
    // COSMETIC ONLY: touches nothing but the predicted render copy; any real
    // motion (sim or predicted) or divergence exceeds the band and rebases
    // exactly as before, so predicted drift is bounded by the band. The host
    // (truth, no self-prediction) and legacy lockstep are untouched. NN_PREDICT
    // bit4 kill-switches it.
    {
        extern int32_t g_netPredictMode, g_netStreamMode, g_netForensics;
        if ((g_netPredictMode & 16) && g_netStreamMode && numplayers > 1 && myconnectindex != connecthead)
        {
            // z is ~16x finer than x/y in Build; the bands sit well below a real
            // walk/step (hundreds of units/tic) and above idle micro-jitter.
            enum { DB_XY = 48, DB_Z = 400 };
            int32_t const matchXY = klabs(predictedPlayer.pos.x - p->pos.x) + klabs(predictedPlayer.pos.y - p->pos.y);
            bool const simStill  = klabs(p->pos.x - p->opos.x) + klabs(p->pos.y - p->opos.y) <= DB_XY
                                && klabs(p->pos.z - p->opos.z) <= DB_Z;
            bool const predStill = klabs(predictedPlayer.pos.x - predictedPlayer.opos.x)
                                 + klabs(predictedPlayer.pos.y - predictedPlayer.opos.y) <= DB_XY
                                && klabs(predictedPlayer.pos.z - predictedPlayer.opos.z) <= DB_Z;
            bool const matched   = matchXY <= DB_XY && klabs(predictedPlayer.pos.z - p->pos.z) <= DB_Z;
            bool const skip      = simStill && predStill && matched;
            if (g_netForensics)
            {
                static int32_t s_dbandPlc;
                if (movefifoplc - s_dbandPlc >= 60 || movefifoplc < s_dbandPlc)   // ~2s, plc-restart safe
                {
                    s_dbandPlc = movefifoplc;
                    LOG_F(INFO, "[dband] plc=%d skip=%d simStill=%d predStill=%d matched=%d dxy=%d dz=%d",
                          (int)movefifoplc, skip ? 1 : 0, simStill, predStill, matched,
                          matchXY, (int)klabs(predictedPlayer.pos.z - p->pos.z));
                }
            }
            if (skip)
                return;   // deadband: keep last frame's predicted state, no rebase
        }
    }

#if 0
    if (ud.config.PredictionDebug)
    {
        // Print mismatched data here.
    }
#endif

    // Save some crap for interpolation or bugfixing purposes.
    auto oq16ang      = predictedPlayer.oq16ang;
    auto oq16horiz    = predictedPlayer.oq16horiz;
    auto oq16horizoff = predictedPlayer.oq16horizoff;
    auto opos         = predictedPlayer.opos;
    auto orotscrnang  = predictedPlayer.orotscrnang;
    auto olook_ang    = predictedPlayer.olook_ang;
    auto scream_voice = predictedPlayer.scream_voice;
    // The DIRECTION GROUP is client-authoritative for the view (frame-owned,
    // see Net_ProcessPrediction): the reset below stomps it with the sim's
    // tic-quantized copy, so carry the whole group across the reset+replay.
    // The sim keeps its own direction for aim/movement; both integrate the
    // same input deltas, so they track without ever yanking the view.
    auto q16ang       = predictedPlayer.q16ang;
    auto q16horiz     = predictedPlayer.q16horiz;
    auto q16horizoff  = predictedPlayer.q16horizoff;
    auto rotscrnang   = predictedPlayer.rotscrnang;
    auto look_ang     = predictedPlayer.look_ang;

    Net_ResetPredictionData();
    predictfifoplc = movefifoplc;
    Net_DoPrediction(PREDICTSTATE_CORRECT);

    // Annoying Hacks to stop clobbering. Will need to figure out better ways to fix these.
    // If that's even possible given this mess. May just be the poison we need.
    predictedPlayer.scream_voice = scream_voice;

    // Check if pos is different from opos, if so, restore predicted opos.
    // This is to prevent jitter from moving sectors & corrected mispredictions,
    // while preventing teleports from being interp'd.
    if (p->opos != p->pos)
        predictedPlayer.opos = opos;

    predictedPlayer.q16ang      = q16ang;       predictedPlayer.oq16ang      = oq16ang;
    predictedPlayer.q16horiz    = q16horiz;     predictedPlayer.oq16horiz    = oq16horiz;
    predictedPlayer.q16horizoff = q16horizoff;  predictedPlayer.oq16horizoff = oq16horizoff;
    predictedPlayer.rotscrnang  = rotscrnang;   predictedPlayer.orotscrnang  = orotscrnang;
    predictedPlayer.look_ang    = look_ang;     predictedPlayer.olook_ang    = olook_ang;

    // BOUNDED VIEW glide -- LEGACY LOCKSTEP ONLY. It pulls the frame-owned
    // view a quarter of the way toward the sim's direction every consumed
    // tic; with echo RTT the sim's direction is where the mouse was ~100ms
    // ago, so during any sustained turn the view is dragged backward against
    // the mouse ~30x/sec (live, third report: "moving the mouse is still
    // glitchy" -- this was the root all along). The problem it solved
    // (crosshair drifting off the fired shot) is owned by the closed-loop
    // aim staging now: the SIM chases the VIEW every tic, truth flows from
    // the player that fires, and nothing may ever pull the view back.
    if (!g_netStreamMode)
    {
        DukePlayer_t const *sim = p;   // original pointers active here
        fix16_t gapH = fix16_ssub(predictedPlayer.q16horiz, sim->q16horiz);
        if (fix16_abs(gapH) > F16(1))
            predictedPlayer.q16horiz = fix16_ssub(predictedPlayer.q16horiz, gapH >> 2);
        fix16_t gapO = fix16_ssub(predictedPlayer.q16horizoff, sim->q16horizoff);
        if (fix16_abs(gapO) > F16(1))
            predictedPlayer.q16horizoff = fix16_ssub(predictedPlayer.q16horizoff, gapO >> 2);
        fix16_t gapA = fix16_ssub(predictedPlayer.q16ang, sim->q16ang);
        while (gapA > F16(1024))  gapA = fix16_ssub(gapA, F16(2048));
        while (gapA < -F16(1024)) gapA = fix16_sadd(gapA, F16(2048));
        if (fix16_abs(gapA) > F16(1))
        {
            fix16_t a = fix16_ssub(predictedPlayer.q16ang, gapA >> 2);
            while (a < 0)         a = fix16_sadd(a, F16(2048));
            while (a >= F16(2048)) a = fix16_ssub(a, F16(2048));
            predictedPlayer.q16ang = a;
        }
    }
}

void Net_InitializeStructPointers(void)
{
    original_sprite = sprite;
}

static void Net_InitializeGameVarPointers(void)
{
    // Init GameVar original pointers here
    for (int i = g_gameVarCount - 1; i >= 0; i--)
    {
        if (aGameVars[i].flags & (/*GAMEVAR_READONLY |*/ GAMEVAR_SPECIAL | GAMEVAR_NOMULTI | GAMEVAR_PTR_MASK))
            continue;

        // Store original addresses
        if (aGameVars[i].flags & (GAMEVAR_PERPLAYER | GAMEVAR_PERACTOR))
            original_pValues[i] = aGameVars[i].pValues;

        // Allocate memory for predicted values
        if (aGameVars[i].flags & GAMEVAR_PERPLAYER)
        {
            if (!predicted_pValues[i])
                predicted_pValues[i] = (intptr_t *)Xcalloc(MAXPLAYERS, sizeof(intptr_t));
        }
        else if (aGameVars[i].flags & GAMEVAR_PERACTOR)
        {
            if (!predicted_pValues[i])
                predicted_pValues[i] = (intptr_t *)Xcalloc(MAXSPRITES, sizeof(intptr_t));
        }
    }
}

static bool using_predicted_pointers;
void        Net_InitializePrediction(void)
{
    if ((numplayers < 2) || (g_player[myconnectindex].ps == NULL))
        return;

    if (using_predicted_pointers)
        OSD_Printf("^02Prediction Code Error! Tried to init prediction while currently using predicted pointers!\n");
    else
        // Capture the struct pointers HERE, not only in Net_ClearFIFO: a
        // barrier-free late joiner never passes the tic-0 barrier, so its
        // original_sprite stayed NULL -- Net_ResetPredictionData then memcpy'd
        // predicted_sprite from address 0 (the wasm data segment reads fine!),
        // filling it with the binary's string constants, and the first
        // prediction pass restored sprite = NULL: the whole sim walked rodata
        // as sprites until a clip walker hard-looped (live-reported as every
        // ?join= into a running match freezing seconds after the seat).
        Net_InitializeStructPointers();

    originalPlayer        = g_player[myconnectindex].ps;
    oldnet_predictcontext = NOT_PREDICTABLE;  // Must be initialized to NOT_PREDICTABLE.
    Net_InitializeGameVarPointers();
    Net_UseOriginalPointers();
    Net_ResetPredictionData();
    // The replica starts AT the consume cursor -- exactly like every
    // Net_CorrectPrediction rebase. Without this, a late joiner's first
    // prediction pass replayed from the SNAPSHOT tic: dozens of phantom tics
    // of a player that was not in the world yet, whose physics spiraled until
    // getzrange hard-looped the main thread (live-reported: every ?join= into
    // a running match froze seconds after the seat).
    predictfifoplc = movefifoplc;
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[pred] init me=' + $0 + ' sect=' + $1 + ' spr=' + $2 + ' x=' + $3 + ' y=' + $4 + ' z=' + $5 + ' ns=' + $6); },
           myconnectindex, predictedPlayer.cursectnum, predictedPlayer.i,
           predictedPlayer.pos.x, predictedPlayer.pos.y, predictedPlayer.pos.z, predicted_Numsprites);
#endif
}

void Net_UsePredictedPointers(void)
{
    if (numplayers < 2)
        return;

    g_player[myconnectindex].ps = &predictedPlayer;

    sprite = predicted_sprite;

    for (int i = g_gameVarCount - 1; i >= 0; i--)
    {
        if (aGameVars[i].flags & (/*GAMEVAR_READONLY |*/ GAMEVAR_SPECIAL | GAMEVAR_NOMULTI | GAMEVAR_PTR_MASK))
            continue;

        if (aGameVars[i].flags & (GAMEVAR_PERPLAYER | GAMEVAR_PERACTOR))
            aGameVars[i].pValues = predicted_pValues[i];
    }

    using_predicted_pointers = true;
}

// ── Predicted VIEW ──────────────────────────────────────────────────────────
// Swap ONLY the local player's ps pointer for RENDERING (camera + weapon
// drawing), so the player's own movement/turning/weapon feel is zero-latency
// while the authoritative lockstep runs bufferjitter+RTT behind. World arrays
// stay authoritative: everyone ELSE renders at confirmed positions, exactly
// like Quake's client prediction. Callers must pair Begin/End tightly around
// pure DISPLAY code -- never around simulation or menu consumers (the menus
// run snapshot saves and gm transitions that must see the real player).
static DukePlayer_t *s_viewSwapSaved;

void Net_BeginPredictedView(void)
{
    extern int32_t g_netPredictMode;  // DEBUG bisect (oldnet.cpp)
    if (!(g_netPredictMode & 2))
        return;
    // A healing guest mid-catchup keeps its own view but prediction is paused:
    // predictedPlayer holds pre-heal state. Render authoritative -- the world
    // visibly fast-forwards to live, then the resume re-inits prediction.
    if (g_netJoinCatchup)
        return;
    if (s_viewSwapSaved != NULL || numplayers < 2 || screenpeek != myconnectindex
        || originalPlayer == NULL || ud.pause_on)
        return;
    auto &ps = g_player[myconnectindex].ps;
    if (ps != originalPlayer || !(ps->gm & MODE_GAME) || (ps->gm & (MODE_EOL | MODE_RESTART | MODE_NEWGAME)))
        return;
    // Mode/UI bits stay authoritative under the swap (display code tests them).
    predictedPlayer.gm = ps->gm;
    s_viewSwapSaved = ps;
    ps = &predictedPlayer;
}

void Net_EndPredictedView(void)
{
    if (s_viewSwapSaved == NULL)
        return;
    g_player[myconnectindex].ps = s_viewSwapSaved;
    s_viewSwapSaved = NULL;
}

void Net_UseOriginalPointers(void)
{
    if (numplayers < 2)
        return;

    g_player[myconnectindex].ps = originalPlayer;

    sprite = original_sprite;

    // Switch to Original GameVars here
    for (int i = g_gameVarCount - 1; i >= 0; i--)
    {
        if (aGameVars[i].flags & (/*GAMEVAR_READONLY |*/ GAMEVAR_SPECIAL | GAMEVAR_NOMULTI | GAMEVAR_PTR_MASK))
            continue;

        if (aGameVars[i].flags & (GAMEVAR_PERPLAYER | GAMEVAR_PERACTOR))
            aGameVars[i].pValues = original_pValues[i];
    }

    using_predicted_pointers = false;
}

void Net_SwapPredictedLinkedLists(void)
{
    swap_predicted_linked_list(spritesect);
    swap_predicted_linked_list(spritestat);
    std::swap(Numsprites, predicted_Numsprites);
    std::swap(tailspritefree, predicted_tailspritefree);

    for (int i = g_gameVarCount - 1; i >= 0; i--)
    {
        if (aGameVars[i].flags & (/*GAMEVAR_READONLY |*/ GAMEVAR_SPECIAL | GAMEVAR_NOMULTI | GAMEVAR_PTR_MASK))
            continue;

        if (!(aGameVars[i].flags & (GAMEVAR_PERPLAYER | GAMEVAR_PERACTOR)))
            std::swap(aGameVars[i].global, predicted_lValue[i]);  // [port] .global == netduke32 .lValue
    }
}

void Net_DoPrediction(int state)
{
    if (numplayers < 2)
        return;

    oldnet_predicting = state;

    int16_t pSpriteNum = g_player[myconnectindex].ps->i;

    Net_UsePredictedPointers();

    // Backup data
    backupActor  = actor[pSpriteNum];
    backupSeed   = randomseed;
    // The per-tic krand draw counter (sync.cpp cat 16) is lockstep state like
    // randomseed itself: replay draws must not leak into it.
    int32_t const backupKrandCalls = g_krandCalls;

    // Change to predicted sprite/actor
    sprite[pSpriteNum].cstat &= ~257;
    actor[pSpriteNum] = predicted_pActor;

    Net_SwapPredictedLinkedLists();

    // Process. A canonical-stream slave replays to its SAMPLE head (movefifoend
    // is the master-echo high-water there, RTT behind the samples).
#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    int32_t const predictEnd = (numplayers > 1 && myconnectindex != connecthead)
        ? g_netSampleHead
        : g_player[myconnectindex].movefifoend;
    while (predictfifoplc < predictEnd) Net_ProcessPrediction();
#else
    while (predictfifoplc < g_player[myconnectindex].movefifoend) Net_ProcessPrediction();
#endif
    // -------

    Net_SwapPredictedLinkedLists();

    // Save modifications to predicted actor
    predicted_pActor = actor[pSpriteNum];

    Net_UseOriginalPointers();

    // Restore from backup
    actor[pSpriteNum] = backupActor;

    randomseed   = backupSeed;
    g_krandCalls = backupKrandCalls;

    oldnet_predicting = PREDICTSTATE_OFF;
}