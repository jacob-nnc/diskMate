# -*- coding: utf-8 -*-
"""把 tree_C/D.json 合并为 tree_data.js (window.TREE_DATA=...), 供 HTML <script> 加载
file:// 下 <script src> 可直接加载本地 js(避开 fetch 的 CORS 限制)。"""
import json
import os
import time

ICON = os.path.dirname(os.path.abspath(__file__))
t0 = time.time()
roots = []
for fn in ("tree_C.json", "tree_D.json"):
    with open(os.path.join(ICON, fn), encoding="utf-8-sig") as f:
        roots.append(json.load(f))

root = {"name": "This PC", "size": roots[0]["size"] + roots[1]["size"],
        "children": roots}

out = os.path.join(ICON, "tree_data.js")
with open(out, "w", encoding="utf-8") as f:
    f.write("window.TREE_DATA=")
    json.dump(root, f, ensure_ascii=False, separators=(",", ":"))
    f.write(";")
print("已生成:", out)
print("大小: %.1f MB, 耗时 %.1fs" % (os.path.getsize(out) / 1e6, time.time() - t0))
