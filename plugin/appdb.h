/* JamaisVu savestate plugin
 *
 * The names the LiveArea shows, read from the system's app database
 * (ur0:shell/db/app.db, table tbl_appinfo_icon): that is where a game
 * renamed by the user (e.g. Japanese -> English) has its new name.
 *
 * A minimal, read-only SQLite reader: it only walks table b-trees, and
 * checks every offset, so a database that is being written at the same
 * moment (or is damaged) gives "not found", never a bad read. Plain C:
 * file access through a callback, so it runs in the host tests too.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_APPDB_H__
#define __JV_APPDB_H__

#include <stdint.h>

#define APPDB_PATH "ur0:shell/db/app.db"
#define APPDB_NAME_LEN 128

/* Reads len bytes at offset off; returns the number read, < 0 on error. */
typedef int (*AdbRead)(void *ctx, uint32_t off, void *buf, uint32_t len);

/* For each of the n title IDs, copies the LiveArea name (UTF-8) into
 * names[i], or sets names[i][0] = '\0' if it has none. Returns the number
 * of names found, or < 0 if the file is not a database it understands. */
int appdb_names(AdbRead rd, void *ctx, const char *const *ids, int n,
                char names[][APPDB_NAME_LEN]);

#endif
