#!/usr/bin/env python3
"""Builds test databases shaped like the Vita's ur0:shell/db/app.db
(same tbl_appinfo_icon schema) for tests/host/test_appdb.c.
usage: make_appdb.py <outdir>"""
import os
import sqlite3
import sys

PAGE = ("CREATE TABLE tbl_appinfo_page(pageId INTEGER PRIMARY KEY NOT NULL ,"
        "pageNo INT NOT NULL,themeFile TEXT,bgColor INT,texWidth INT,"
        "texHeight INT,imageWidth INT,imageHeight INT,reserved01,reserved02,"
        "reserved03,reserved04,reserved05)")
ICON = ("CREATE TABLE tbl_appinfo_icon(pageId REFERENCES "
        "tbl_appinfo_page(pageId) ON DELETE RESTRICT NOT NULL,pos INT NOT "
        "NULL,iconPath TEXT,title TEXT COLLATE NOCASE,type NOT NULL,command "
        "TEXT, titleId TEXT,icon0Type NOT NULL,parentalLockLv INT,status INT,"
        "reserved01,reserved02,reserved03,reserved04,reserved05,"
        "PRIMARY KEY(pageId, pos))")
INFO = ("CREATE TABLE tbl_appinfo(titleId TEXT NOT NULL,key INTEGER NOT NULL,"
        "val,PRIMARY KEY(titleId, key))")

CATHERINE = "キャサリン・ＦＢ"


def make(path, page_size=4096, napps=40, icon_sql=ICON, extra=()):
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.execute(f"PRAGMA page_size={page_size}")
    db.execute(PAGE)
    db.execute(icon_sql)
    db.execute(INFO)
    db.execute("INSERT INTO tbl_appinfo_page VALUES(1,0,NULL,0,0,0,0,0,"
               "NULL,NULL,NULL,NULL,NULL)")
    rows = [(None, "Folders", 5)]
    rows += [(f"FILL{i:05d}", f"Filler app {i}", 0) for i in range(napps)]
    rows += [("PCSG01179", CATHERINE, 0),
             ("PCSE00349", "1001 Spikes", 0),
             ("PCSG00490", "Trails in the Sky the 3rd", 0)]  # user renamed
    rows += list(extra)
    for pos, (tid, title, typ) in enumerate(rows):
        db.execute("INSERT INTO tbl_appinfo_icon VALUES(1,?,?,?,?,?,?,0,0,0,"
                   "NULL,NULL,NULL,NULL,NULL)",
                   (pos, f"ur0:appmeta/{tid}/icon0.png", title, typ,
                    None, tid))
        db.execute("INSERT OR REPLACE INTO tbl_appinfo VALUES(?,572932585,?)",
                   (tid or "", title))
    db.commit()
    db.close()


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    make(f"{out}/small.db")
    make(f"{out}/big.db", napps=3000)            # interior pages
    make(f"{out}/p1024.db", page_size=1024, napps=500)
    make(f"{out}/p65536.db", page_size=65536, napps=200)
    long_name = "英" * 60                      # 180 bytes of UTF-8
    make(f"{out}/long.db", extra=[
        ("PCSB00001", long_name, 0),
        # a record too big for its page (overflow): skipped, not misread
        ("PCSB00002", "x" * 9000, 0),
        # a folder with a game's ID is not the game
        ("PCSB00003", "Folder", 5),
        ("PCSB00003", "Real name", 0)])
    make(f"{out}/schema.db", icon_sql=ICON.replace("iconPath TEXT,title",
                                                   "title TEXT,iconPath"))


if __name__ == "__main__":
    main()
