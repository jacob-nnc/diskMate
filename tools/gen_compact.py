# -*- coding: utf-8 -*-
"""把 tree_C.json + tree_D.json 转成紧凑二进制 -> zlib -> base64 内联 js
格式(小端):
  Header: magic 'TREE'(4) | version u32 | N u32 | nameBytes u32
  sizes:    Float64Array[N]           8N
  dirs:     Uint8Array[N]             N
  nameLen:  Uint16Array[N]            2N
  names:    UTF-8 拼接字节流          nameBytes
  childStart: Int32Array[N]           4N
  childCount: Uint32Array[N]          4N
  childIdx: Int32Array[N-1]           4(N-1)
节点按 DFS 先序编号(根=0); children 顺序=输入顺序, 布局时前端按 size 排序。
"""
import base64
import json
import os
import struct
import time
import zlib

ICON = os.path.dirname(os.path.abspath(__file__))
t0 = time.time()

roots = []
for fn in ("tree_C.json", "tree_D.json"):
    with open(os.path.join(ICON, fn), encoding="utf-8-sig") as f:
        roots.append(json.load(f))
print("JSON 加载完成 %.1fs" % (time.time() - t0))

root = {"name": "", "size": roots[0]["size"] + roots[1]["size"],
        "children": roots}

sizes = []
dirs = bytearray()
name_lens = []
name_bytes = bytearray()
child_count = []
idx_of = {}
nodes_by_idx = []

def assign(node):
    """DFS 先序编号并填充平行数组"""
    i = len(sizes)
    idx_of[id(node)] = i
    nodes_by_idx.append(node)     # DFS 序 = idx 顺序
    sizes.append(node["size"])
    dirs.append(1 if node.get("dir") else 0)
    nb = node["name"].encode("utf-8")
    name_lens.append(len(nb))
    name_bytes.extend(nb)
    kids = [c for c in (node.get("children") or []) if c.get("size", 0) > 0]
    if node is root:
        kids = [c for c in kids if c.get("dir")]
    kids.sort(key=lambda c: c["size"], reverse=True)  # 按 size 降序, 布局顺序直读
    node["_kids"] = kids
    child_count.append(len(kids))
    for c in kids:
        assign(c)

assign(root)
N = len(sizes)

# nodes_by_idx 顺序即 DFS 序(= idx), 直接遍历填 child_start/child_idx
child_start = [0] * N
child_idx = []
offset = 0
for i, node in enumerate(nodes_by_idx):
    kids = node["_kids"]
    child_start[i] = offset
    for c in kids:
        child_idx.append(idx_of[id(c)])
        offset += 1

assert len(child_idx) == N - 1, (len(child_idx), N)
print("节点数:", N, "| 名字池: %d 字节 | %.1fs"
      % (len(name_bytes), time.time() - t0))

# ---- 打包(按 8/4 字节对齐顺序, names 最后) ----
def pad(align):
    while len(buf) % align:
        buf.append(0)

buf = bytearray(b"TREE")
buf += struct.pack("<III", 1, N, len(name_bytes))
pad(8)
buf += struct.pack("<%dd" % N, *sizes)
pad(1)
buf += bytes(dirs)
pad(2)
buf += struct.pack("<%dH" % N, *name_lens)
pad(4)
buf += struct.pack("<%di" % N, *child_start)
buf += struct.pack("<%dI" % N, *child_count)
buf += struct.pack("<%di" % (N - 1), *child_idx)
buf += bytes(name_bytes)
raw = bytes(buf)
print("二进制: %.1f MB" % (len(raw) / 1e6))

comp = zlib.compress(raw, 9)
print("zlib: %.1f MB (%.2f%%)" % (len(comp) / 1e6, 100 * len(comp) / len(raw)))

b64 = base64.b64encode(comp).decode("ascii")
out = os.path.join(ICON, "tree_data_b64.js")
with open(out, "w", encoding="ascii") as f:
    f.write('window.TREE_B64="' + b64 + '";\n')
print("已生成 %s: %.1f MB | 总耗时 %.1fs"
      % (out, os.path.getsize(out) / 1e6, time.time() - t0))

# 无压缩版(兼容不支持 DecompressionStream 的环境, 仅 atob)
b64n = base64.b64encode(raw).decode("ascii")
outn = os.path.join(ICON, "tree_data_b64_noc.js")
with open(outn, "w", encoding="ascii") as f:
    f.write('window.TREE_B64_NOC="' + b64n + '";\n')
print("已生成 %s: %.1f MB" % (outn, os.path.getsize(outn) / 1e6))
