#define NET_PREDICT_CPP_

#include "duke3d.h"
#include "oldnet.h"
#include "net_predict.h"

// Without initializing value as done here, menu sounds might be missing.
int32_t oldnet_predictcontext = NOT_PREDICTABLE;

CREATE_PREDICTED_LINKED_LIST(spritesect);
CREATE_PREDICTED_LINKED_LIST(spritestat);
int32_t predicted_Numsprites;
int16_t predicted_tailspritefree;

actor_t  backupActor;
int32_t      backupSeed;
randomseed_t backupRandom;

static void Net_ProcessPrediction(void)
{
    input_t backupInput = g_player[myconnectindex].input;

    g_player[myconnectindex].input = inputfifo[INPUTFIFO_PREDICTTICK][myconnectindex];

    // Clear bits that could cause trouble
    g_player[myconnectindex].input.bits &= ~(BIT(SK_PAUSE) | BIT(SK_MULTIFLAG) | BIT(SK_GAMEQUIT));

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
            predicted_lValue[i] = aGameVars[i].lValue;
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

    if (p->oq16ang != p->q16ang)
        predictedPlayer.oq16ang = oq16ang;

    if (p->oq16horiz != p->q16horiz)
        predictedPlayer.oq16horiz = oq16horiz;

    if (p->oq16horizoff != p->q16horizoff)
        predictedPlayer.oq16horizoff = oq16horizoff;

    if (p->orotscrnang != p->rotscrnang)
        predictedPlayer.orotscrnang = orotscrnang;

    if (p->olook_ang != p->look_ang)
        predictedPlayer.olook_ang = olook_ang;
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

    originalPlayer        = g_player[myconnectindex].ps;
    oldnet_predictcontext = NOT_PREDICTABLE;  // Must be initialized to NOT_PREDICTABLE.
    Net_InitializeGameVarPointers();
    Net_UseOriginalPointers();
    Net_ResetPredictionData();
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
            std::swap(aGameVars[i].lValue, predicted_lValue[i]);
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
    backupRandom = g_random;

    // Change to predicted sprite/actor
    sprite[pSpriteNum].cstat &= ~257;
    actor[pSpriteNum] = predicted_pActor;

    Net_SwapPredictedLinkedLists();

    // Process
    while (predictfifoplc < g_player[myconnectindex].movefifoend) Net_ProcessPrediction();
    // -------

    Net_SwapPredictedLinkedLists();

    // Save modifications to predicted actor
    predicted_pActor = actor[pSpriteNum];

    Net_UseOriginalPointers();

    // Restore from backup
    actor[pSpriteNum] = backupActor;

    randomseed = backupSeed;
    g_random   = backupRandom;

    oldnet_predicting = PREDICTSTATE_OFF;
}