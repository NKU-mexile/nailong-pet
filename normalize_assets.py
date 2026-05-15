"""
normalize_assets.py
将 assets/idle、assets/smile、assets/laugh 下所有 PNG 统一 padding 到同一尺寸。
找出三个文件夹中最大的宽和最大的高作为目标尺寸，内容居中，四周补透明像素。
处理后所有图片尺寸完全一致，C++ 侧可直接用统一 scale，无需补偿计算。

依赖：pillow（pip install pillow）
"""

from pathlib import Path
from PIL import Image


FOLDERS = [
    Path("assets/idle"),
    Path("assets/smile"),
    Path("assets/laugh"),
]


def collect_pngs() -> list[Path]:
    pngs = []
    for folder in FOLDERS:
        pngs.extend(sorted(folder.glob("*.png")))
    return pngs


def find_max_size(pngs: list[Path]) -> tuple[int, int]:
    max_w = max_h = 0
    for p in pngs:
        with Image.open(p) as img:
            w, h = img.size
            if w > max_w:
                max_w = w
            if h > max_h:
                max_h = h
    return max_w, max_h


def pad_to(img: Image.Image, target_w: int, target_h: int) -> Image.Image:
    """将 img 居中粘贴到 target_w × target_h 的透明画布上。"""
    canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
    src = img.convert("RGBA")
    x = (target_w - src.width) // 2
    y = (target_h - src.height) // 2
    canvas.paste(src, (x, y))
    return canvas


def main() -> None:
    pngs = collect_pngs()
    if not pngs:
        print("未找到任何 PNG，请确认 assets/ 目录存在且已包含素材。")
        return

    target_w, target_h = find_max_size(pngs)
    print(f"目标尺寸：{target_w} × {target_h} px（共 {len(pngs)} 张）\n")

    changed = skipped = 0
    for p in pngs:
        with Image.open(p) as img:
            if img.size == (target_w, target_h):
                print(f"  跳过（已达目标尺寸）：{p}")
                skipped += 1
                continue
            padded = pad_to(img, target_w, target_h)

        padded.save(p, format="PNG")
        print(f"  处理完成 {img.size} → {padded.size}：{p}")
        changed += 1

    print(f"\n完成：处理 {changed} 张，跳过 {skipped} 张。")
    print(f"所有图片已统一为 {target_w} × {target_h} px。")


if __name__ == "__main__":
    main()
