# -*- coding: utf-8 -*-
"""磁盘占用数据树 v2 —— 模拟树木"顶端优势"
  主枝(延续): 每个目录下最大的子目录, 沿原方向小偏转继续生长(数据决定谁是主枝)
  侧枝(分叉): 其余子目录左右交替、小角度分出; 分叉角/枝长/枝粗 ∝ log(size)
  叶簇: 只长在末端(到达深度上限或无可见子目录的子树), 大小 ∝ 整子树 size
剪枝: MAX_DEPTH / MIN_RATIO / KIDS_CAP
C: D: 从根部对称展开; 确定性 hash 微扰(可复现, 非随机)。
"""
import json
import math
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection

ICON = os.path.dirname(os.path.abspath(__file__))

MAX_DEPTH = 9
MIN_RATIO = 0.004
KIDS_CAP = 26

segments = []   # (x1,y1,x2,y2,depth,size)
leaves = []     # (x,y,size)
labels = []     # (x,y,text,size)


def jitter(name):
    h = sum(ord(c) for c in name)
    return ((h % 200) / 200.0 - 0.5) * 8.0     # ±4° 确定性微扰


def logr(size):
    return min(1.0, max(0.0, math.log10(size + 1) / 12.0))


def visible_kids(node):
    kids = [c for c in node.get("children", []) if c.get("dir") and c["size"] > 0]
    kids.sort(key=lambda c: c["size"], reverse=True)
    total = node["size"]
    shown = [k for k in kids if k["size"] >= total * MIN_RATIO][:KIDS_CAP]
    return shown


def up(angle, k):
    """背地性: 方向以系数 k 向正上方(90°)回归"""
    return angle + (90.0 - angle) * k


def grow(node, x, y, angle, depth):
    """node 位于 (x,y), 主枝沿 angle 延续, 侧枝左右分出"""
    kids = visible_kids(node)
    if depth >= MAX_DEPTH or not kids:
        leaves.append((x, y, node["size"]))     # 末端: 整子树变叶簇
        return

    main, sides = kids[0], kids[1:]

    # 主延续枝: 方向基本不变 + 向上回归, 长度满格
    ma = up(angle + jitter(main["name"]) * 0.4, 0.28)
    ml = 1.02
    rad = math.radians(ma)
    mx, my = x + ml * math.cos(rad), y + ml * math.sin(rad)
    segments.append((x, y, mx, my, depth, main["size"]))
    grow(main, mx, my, ma, depth + 1)

    # 侧枝: 左右交替, 分叉角随深度收窄(老枝角大、新梢角小)
    taper = max(0.45, 1.0 - depth * 0.06)
    for i, s in enumerate(sides):
        side = 1 if i % 2 == 0 else -1
        r = logr(s["size"])
        fork = (24 + 26 * r) * taper            # 24°~50° × 收窄
        sa = up(angle + side * fork + jitter(s["name"]), 0.10)
        sl = (0.5 + 0.6 * r) * (0.85 + 0.15 * taper)
        rad = math.radians(sa)
        sx, sy = x + sl * math.cos(rad), y + sl * math.sin(rad)
        segments.append((x, y, sx, sy, depth, s["size"]))
        grow(s, sx, sy, sa, depth + 1)


def main():
    with open(os.path.join(ICON, "tree_C.json"), encoding="utf-8-sig") as f:
        c = json.load(f)
    with open(os.path.join(ICON, "tree_D.json"), encoding="utf-8-sig") as f:
        d = json.load(f)

    fig, ax = plt.subplots(figsize=(13, 13), dpi=150)
    fig.patch.set_facecolor("#14171f")
    ax.set_facecolor("#14171f")

    # 根 -> C: D: 对称展开
    root_x, root_y = 0.0, 0.0
    labels.append((root_x, root_y - 0.15, "This PC", 0))
    for disk, side in ((c, -1), (d, 1)):
        ang = 90 + side * 26
        rad = math.radians(ang)
        dx, dy = root_x + 1.0 * math.cos(rad), root_y + 1.0 * math.sin(rad)
        segments.append((root_x, root_y, dx, dy, 0, disk["size"]))
        labels.append((dx - 0.15 * math.cos(rad), dy - 0.15 * math.sin(rad),
                       disk["name"].rstrip("\\") + "\\", disk["size"]))
        grow(disk, dx, dy, ang, 1)

    # 树枝
    lines = [[(s[0], s[1]), (s[2], s[3])] for s in segments]
    maxd = max(s[4] for s in segments)
    colors, widths = [], []
    for s in segments:
        depth, size = s[4], s[5]
        t = depth / maxd
        colors.append((0.40 + 0.30 * t, 0.25 + 0.14 * t, 0.12))
        widths.append(max(0.6, 0.7 + logr(size) * 5.2))
    ax.add_collection(LineCollection(lines, colors=colors, linewidths=widths,
                                     capstyle="round", zorder=2))

    # 叶簇(集中在树冠)
    ax.scatter([p[0] for p in leaves], [p[1] for p in leaves],
               s=[10 + logr(p[2]) * 42 for p in leaves],
               color="#4ca64c", alpha=0.85, zorder=3, edgecolors="none")

    # 标签
    for x, y, text, size in labels:
        ax.text(x, y, text, color="#dfe5f0", fontsize=12, ha="center", va="center",
                zorder=4)

    ax.set_aspect("equal")
    ax.autoscale_view()
    ax.margins(0.08)
    ax.set_xticks([]); ax.set_yticks([])
    for sp in ax.spines.values():
        sp.set_visible(False)

    plt.tight_layout(pad=0.5)
    out = os.path.join(ICON, "disk_tree.png")
    plt.savefig(out, facecolor=fig.get_facecolor(), bbox_inches="tight")
    print("已保存:", out, "| 树枝段数:", len(segments), "| 叶簇:", len(leaves))


if __name__ == "__main__":
    main()
