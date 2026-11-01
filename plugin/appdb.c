/* JamaisVu savestate plugin
 *
 * Minimal read-only SQLite reader for the LiveArea names, see appdb.h.
 * File format: https://www.sqlite.org/fileformat.html
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include <string.h>

#include "appdb.h"

/* the Vita's app.db uses 4 KB pages; larger ones are refused (no 64 KB
 * buffer kept in kernel memory for a case that does not occur) */
#define MAX_PAGE 4096
#define MAX_PAGES_VISITED 4096 /* a corrupt tree must not loop forever */
#define STACK 512

/* tbl_appinfo_icon's columns, as created by the 3.60-3.74 shell: the
 * schema text is compared, so another layout is never misread */
static const char k_icon_sql[] =
  "CREATE TABLE tbl_appinfo_icon(pageId REFERENCES tbl_appinfo_page(pageId) "
  "ON DELETE RESTRICT NOT NULL,pos INT NOT NULL,iconPath TEXT,title TEXT "
  "COLLATE NOCASE,type NOT NULL,command TEXT, titleId TEXT,";
#define COL_TITLE 3
#define COL_TYPE 4
#define COL_TITLEID 6

typedef struct {
  AdbRead rd;
  void *ctx;
  uint32_t page_size, usable;
  uint8_t *page; /* page_size bytes */
} Db;

static uint8_t g_page[MAX_PAGE];
static uint32_t g_stack[STACK];

static uint32_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* SQLite varint at p (at most end); returns its length, 0 if cut off */
static int varint(const uint8_t *p, const uint8_t *end, uint64_t *v) {
  uint64_t x = 0;
  for (int i = 0; i < 9; i++) {
    if (p + i >= end)
      return 0;
    if (i == 8) {
      *v = (x << 8) | p[i];
      return 9;
    }
    x = (x << 7) | (p[i] & 0x7f);
    if (!(p[i] & 0x80)) {
      *v = x;
      return i + 1;
    }
  }
  return 0;
}

static int read_page(Db *db, uint32_t no) {
  if (no == 0)
    return -1;
  uint32_t off = (no - 1) * db->page_size;
  if (off / db->page_size != no - 1) /* overflow */
    return -1;
  return db->rd(db->ctx, off, db->page, db->page_size) == (int)db->page_size
             ? 0 : -1;
}

/* One column of a record held entirely in [rec, end): type and value. */
typedef struct {
  uint64_t type;
  const uint8_t *data;
  uint32_t len;
} Col;

static int record_cols(const uint8_t *rec, const uint8_t *end, Col *cols,
                       int ncols) {
  uint64_t hsize, t;
  int k = varint(rec, end, &hsize);
  if (!k || hsize > (uint64_t)(end - rec))
    return -1;
  const uint8_t *h = rec + k, *hend = rec + hsize, *body = hend;
  int i = 0;
  while (h < hend && i < ncols) {
    int m = varint(h, hend, &t);
    if (!m)
      return -1;
    h += m;
    uint32_t len;
    static const uint8_t int_len[10] = { 0, 1, 2, 3, 4, 6, 8, 8, 0, 0 };
    if (t < 10)
      len = int_len[t];
    else if (t >= 12)
      len = (uint32_t)((t - 12) / 2);
    else
      return -1;
    if (len > (uint64_t)(end - body))
      return -1; /* (part of) the record is on an overflow page */
    cols[i].type = t;
    cols[i].data = body;
    cols[i].len = len;
    body += len;
    i++;
  }
  for (; i < ncols; i++) { /* missing trailing columns are NULL */
    cols[i].type = 0;
    cols[i].data = NULL;
    cols[i].len = 0;
  }
  return 0;
}

static int64_t col_int(const Col *c) {
  if (c->type == 8)
    return 0;
  if (c->type == 9)
    return 1;
  if (c->type < 1 || c->type > 6)
    return -1;
  uint64_t v = (c->data[0] & 0x80) ? ~(uint64_t)0 : 0; /* sign */
  for (uint32_t i = 0; i < c->len; i++)
    v = (v << 8) | c->data[i];
  return (int64_t)v;
}

static int col_text_is(const Col *c, const char *s) {
  size_t n = strlen(s);
  return c->type >= 13 && (c->type & 1) && c->len == n &&
         memcmp(c->data, s, n) == 0;
}

/* Calls fn for each row of the table b-tree rooted at root whose record
 * fits on its page. Returns 0, or -1 if the tree is not readable. */
typedef void (*RowFn)(void *arg, const uint8_t *rec, const uint8_t *end);

static int scan_table(Db *db, uint32_t root, RowFn fn, void *arg) {
  int sp = 0, visited = 0;
  g_stack[sp++] = root;

  while (sp > 0) {
    uint32_t no = g_stack[--sp];
    if (++visited > MAX_PAGES_VISITED || read_page(db, no) < 0)
      return -1;
    const uint8_t *pg = db->page;
    const uint8_t *hdr = pg + (no == 1 ? 100 : 0);
    const uint8_t *pend = pg + db->usable;
    uint8_t kind = hdr[0];
    uint32_t ncells = be16(hdr + 3);
    int interior = kind == 0x05;
    if (kind != 0x05 && kind != 0x0d)
      return -1;
    const uint8_t *ptrs = hdr + (interior ? 12 : 8);
    if (ptrs + 2 * ncells > pend)
      return -1;

    if (interior) {
      if (sp + (int)ncells + 1 > STACK)
        return -1;
      g_stack[sp++] = be32(hdr + 8); /* right-most child */
      for (uint32_t i = 0; i < ncells; i++) {
        uint32_t off = be16(ptrs + 2 * i);
        if (off + 4 > db->usable)
          return -1;
        g_stack[sp++] = be32(pg + off);
      }
      continue;
    }

    /* leaf: payload size, rowid, payload */
    uint32_t max_local = db->usable - 35;
    for (uint32_t i = 0; i < ncells; i++) {
      uint32_t off = be16(ptrs + 2 * i);
      uint64_t psize, rowid;
      if (off >= db->usable)
        continue;
      const uint8_t *c = pg + off;
      int a = varint(c, pend, &psize);
      if (!a)
        continue;
      int b = varint(c + a, pend, &rowid);
      if (!b)
        continue;
      const uint8_t *rec = c + a + b;
      /* only records entirely on this page (the names are short) */
      if (psize > max_local || psize > (uint64_t)(pend - rec))
        continue;
      fn(arg, rec, rec + psize);
    }
  }
  return 0;
}

/* --- sqlite_master: find tbl_appinfo_icon -------------------------------- */

typedef struct {
  uint32_t root;
  int schema_ok;
} Master;

static void master_row(void *arg, const uint8_t *rec, const uint8_t *end) {
  Master *m = arg;
  Col c[5]; /* type, name, tbl_name, rootpage, sql */
  if (record_cols(rec, end, c, 5) < 0)
    return;
  if (!col_text_is(&c[0], "table") || !col_text_is(&c[1], "tbl_appinfo_icon"))
    return;
  int64_t root = col_int(&c[3]);
  if (root <= 0 || root > 0x7fffffff)
    return;
  m->root = (uint32_t)root;
  m->schema_ok = c[4].type >= 13 && (c[4].type & 1) &&
                 c[4].len >= sizeof(k_icon_sql) - 1 &&
                 memcmp(c[4].data, k_icon_sql, sizeof(k_icon_sql) - 1) == 0;
}

/* --- tbl_appinfo_icon rows --------------------------------------------- */

typedef struct {
  const char *const *ids;
  int n, found;
  char (*names)[APPDB_NAME_LEN];
} Icons;

static void icon_row(void *arg, const uint8_t *rec, const uint8_t *end) {
  Icons *q = arg;
  Col c[COL_TITLEID + 1];
  if (record_cols(rec, end, c, COL_TITLEID + 1) < 0)
    return;
  const Col *id = &c[COL_TITLEID], *title = &c[COL_TITLE];
  if (col_int(&c[COL_TYPE]) != 0) /* 0: an application */
    return;
  if (!(title->type >= 13 && (title->type & 1)) || title->len == 0)
    return;
  for (int i = 0; i < q->n; i++) {
    if (q->names[i][0] || !col_text_is(id, q->ids[i]))
      continue;
    uint32_t len = title->len;
    if (len > APPDB_NAME_LEN - 1)
      len = APPDB_NAME_LEN - 1;
    /* never end inside a UTF-8 character */
    while (len > 0 && len < title->len && (title->data[len] & 0xc0) == 0x80)
      len--;
    memcpy(q->names[i], title->data, len);
    q->names[i][len] = '\0';
    q->found++;
  }
}

int appdb_names(AdbRead rd, void *ctx, const char *const *ids, int n,
                char names[][APPDB_NAME_LEN]) {
  static uint8_t hdr[100];
  Db db;

  for (int i = 0; i < n; i++)
    names[i][0] = '\0';
  if (rd(ctx, 0, hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
      memcmp(hdr, "SQLite format 3", 16) != 0)
    return -1;
  db.page_size = be16(hdr + 16);
  if (db.page_size == 1)
    db.page_size = 65536;
  if (db.page_size < 512 || db.page_size > MAX_PAGE ||
      (db.page_size & (db.page_size - 1)))
    return -1;
  db.usable = db.page_size - hdr[20];
  if (db.usable < 480 || be32(hdr + 56) != 1) /* UTF-8 text only */
    return -1;
  db.rd = rd;
  db.ctx = ctx;
  db.page = g_page;

  Master m = { 0, 0 };
  if (scan_table(&db, 1, master_row, &m) < 0 || !m.root || !m.schema_ok)
    return -1;

  Icons q = { ids, n, 0, names };
  if (scan_table(&db, m.root, icon_row, &q) < 0) {
    for (int i = 0; i < n; i++)
      names[i][0] = '\0'; /* changed while reading: use nothing */
    return -1;
  }
  return q.found;
}
