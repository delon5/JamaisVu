/* JamaisVu savestate plugin
 *
 * No lock screen after a JamaisVu save or load (handhelds and PS TV),
 * without needing the NoLockScreen plugin.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_LOCKSKIP_H__
#define __JV_LOCKSKIP_H__

/* Creates the mutex the two calls below share; from module start, before
 * any thread can call them. */
void lockskip_init(void);

/* Patches SceShell in memory so that the next wake-up skips the lock
 * screen. Writes a line for log.txt into out. Returns 0 if applied. */
int lockskip_apply(char *out, int len);

/* Undoes lockskip_apply (no-op if not applied): normal sleep/wake
 * locks again. Safe to call from any thread. */
void lockskip_release(void);

#endif
