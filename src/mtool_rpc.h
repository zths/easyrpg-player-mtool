/*
 * MTool online translation integration.
 * Connects to a running MTool instance over the mtoolsocket transport
 * and provides runtime text translation plus string extraction RPCs.
 *
 * Enabled by setting the MTOOL_PORT environment variable before launch, either to
 * the port number or to the path of a file containing it (MTool's loaders/listenPort).
 * Builds without PLAYER_WITH_MTOOL (or without nlohmann-json) get stubs.
 */

#ifndef EP_MTOOL_RPC_H
#define EP_MTOOL_RPC_H

#include <string>

namespace MtoolRpc {

/** @return True when the MTool integration was enabled at startup. */
bool Enabled();

/**
 * Resolve MTOOL_PORT and start the background connection thread.
 * No-op when the variable is unset. Call once early in Player::Init.
 */
void InitFromEnv();

/**
 * @return True when messages should be word-wrapped to the window width even
 * though the game did not ask for it. Translated lines are usually much longer
 * than the original ones and RM2k would just draw them past the window border.
 */
bool ForceWordWrap();

/**
 * Applies pending requests that must not run on the transport thread
 * (currently font changes). Call once per frame from the main loop.
 */
void Update();

/**
 * Tell the RPC runtime whether the caller is inside the focus-lost wait
 * loop. Scene operations are deferred to the main loop while set.
 */
void SetFocusWait(bool waiting);

/**
 * Translate a text through MTool. Returns an empty string when offline,
 * when no translation data is loaded, or when the request fails.
 * Returns the input unchanged when MTool has no translation for it.
 */
std::string Translate(const std::string& text);

/**
 * @return MTool-requested game speed multiplier (1 when none). The main loop
 * takes the max of this and the fast-forward key speed each frame.
 */
double SpeedFactor();

/**
 * @return True when MTool asked for walk-through-walls. Player movement
 * re-derives the through flag on every step, so this must be consulted at
 * the same place as DEBUG_THROUGH instead of set from outside.
 */
bool ForceThrough();

/**
 * @return True when MTool disabled random encounters. Consulted next to the
 * DEBUG_THROUGH early-out in Game_Player::UpdateEncounterSteps. Scripted
 * battles (BattleProcessing) are not affected.
 */
bool NoEncounter();

} // namespace MtoolRpc

#endif
