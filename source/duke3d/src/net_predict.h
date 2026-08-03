#ifndef NET_PREDICT_H
#define NET_PREDICT_H

#include "build.h"
#include "player.h"

#ifdef NET_PREDICT_CPP_
#define NET_PREDICT_EXTERN
#else
#define NET_PREDICT_EXTERN extern
#endif

enum
{
    PREDICTSTATE_OFF     = 0,
    PREDICTSTATE_PROCESS = 1,
    PREDICTSTATE_CORRECT = 2,
};

// Set in oldnet_predictcontext in unpredictable functions.
#define NOT_PREDICTABLE -1

#define CREATE_PREDICTED_LINKED_LIST(name)     \
    decltype(head##name) predicted_head##name; \
    decltype(prev##name) predicted_prev##name; \
    decltype(next##name) predicted_next##name

#define swap_predicted_linked_list(name) \
    std::swap(head##name, predicted_head##name), std::swap(prev##name, predicted_prev##name), std::swap(next##name, predicted_next##name)

#define reset_predicted_linked_list(name)                                                                                                           \
    memcpy(predicted_head##name, head##name, sizeof(predicted_head##name)), memcpy(predicted_prev##name, prev##name, sizeof(predicted_prev##name)), \
    memcpy(predicted_next##name, next##name, sizeof(predicted_next##name))

// Variable Externs
extern int32_t oldnet_predictcontext;  // Must set to player number on code branches that are able to predict sounds and other effects.

NET_PREDICT_EXTERN int32_t oldnet_predicting;  // Check if oldnet_predicting is non-zero and return in code branches you want to avoid going into during prediction.
NET_PREDICT_EXTERN int predictfifoplc; // Current tick we're predicting

NET_PREDICT_EXTERN DukePlayer_t *originalPlayer;
NET_PREDICT_EXTERN DukePlayer_t  predictedPlayer;

NET_PREDICT_EXTERN spritetype predicted_sprite[MAXSPRITES];
NET_PREDICT_EXTERN spritetype *original_sprite;

NET_PREDICT_EXTERN ActorData_t predicted_pActor;

NET_PREDICT_EXTERN intptr_t *original_pValues[MAXGAMEVARS];
NET_PREDICT_EXTERN intptr_t *predicted_pValues[MAXGAMEVARS];
NET_PREDICT_EXTERN intptr_t  predicted_lValue[MAXGAMEVARS];

// Functions
void Net_InitializeStructPointers(void);
void Net_CorrectPrediction(void);
void Net_InitializePrediction(void);
void Net_SwapPredictedLinkedLists(void);
void Net_DoPrediction(int state);
void Net_UsePredictedPointers(void);
void Net_UseOriginalPointers(void);

// Returns true if we're in a valid predictable state and context for the local player.
// Used to prevent sounds and display events from triggering outside of prediction if they are predictable.
// Also prevents double-playing of one-shot events like light flashes and sound playback.
static inline bool Net_InPredictableState(int32_t spriteNum = -1)
{
    if (numplayers > 1)
    {
        auto const p = g_player[myconnectindex].ps;
        // Do not predict during correction phase, lest we annihilate our ears/eyes.
        if (oldnet_predicting == PREDICTSTATE_CORRECT)
            return false;

        // Predictable state, but we're not predicting, so block.
        if ((oldnet_predictcontext == myconnectindex) && (spriteNum == -1 || spriteNum == p->i) && (oldnet_predicting == PREDICTSTATE_OFF))
            return false;
    }

    return true;
}

// Template trickery

// [JM] Taken from this tutorial: https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md#e19-use-a-final_action-object-to-express-cleanup-if-no-suitable-resource-handle-is-available
//      Uses a struct's destructor as a clever way of performing an action when a function returns or the current scope is left.
template <typename A> struct final_action
{  // slightly simplified
    A act;
    final_action(A a) : act { a }
    {
    }
    ~final_action()
    {
        act();
    }
};

template <typename A> final_action<A> finally(A act)  // deduce action type
{
    return final_action<A> { act };
}

// Put at the highest point of predictable code branches.
#define SET_PREDICTION_CONTEXT(playerNum)                  \
    oldnet_predictcontext = playerNum;                     \
    auto onexit           = finally(                       \
    [&]                                          \
    {                                            \
        oldnet_predictcontext = NOT_PREDICTABLE; \
    })

// Put at the start of functions that cannot be predicted.
// Returns immediately if predicting. Automatically handles reverting to old context value on any return.
// Follow up with a value in parenthesis to return desired value when bailing during prediction. Eg: UNPREDICTABLE_FUNCTION(0);
#define UNPREDICTABLE_FUNCTION                            \
    auto const pcontext_old = oldnet_predictcontext;      \
    oldnet_predictcontext   = NOT_PREDICTABLE;            \
    auto onexit             = finally(                    \
    [&]                                       \
    {                                         \
        oldnet_predictcontext = pcontext_old; \
    });                                       \
    if (oldnet_predicting)                                \
    return

#endif