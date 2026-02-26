/**
 * @file session_state_bridge.h
 * @brief Bridge helpers for session state synchronization across modules.
 */

#ifndef SESSION_STATE_BRIDGE_H
#define SESSION_STATE_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mark that a voice session has started from external/server command.
 *
 * Brings controller UI/session flag to the same state as wake-word start path.
 */
void session_state_mark_started_external(void);

#ifdef __cplusplus
}
#endif

#endif // SESSION_STATE_BRIDGE_H
