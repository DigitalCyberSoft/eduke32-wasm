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

    DukePlayer_t *p = g_player[myconnectindex].ps;

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

    // BOUNDED VIEW: the carried (frame-owned) direction and the sim's copy
    // integrate the same deltas but apply clamps/centering in different
    // orders, so they drift apart -- and the SIM is what shoots. A drifted
    // crosshair means aimed shots miss ("can't hit a fire hydrant",
    // live-reported twice). Glide any gap closed a quarter per correction
    // pass: imperceptible per frame, and drift can never accumulate.
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