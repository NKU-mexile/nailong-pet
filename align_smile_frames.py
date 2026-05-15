"""
align_smile_frames.py
以 assets/smile/smile_01.png ~ smile_05.png 内容 bounding box 的平均宽高为基准，
将 smile_06.png ~ smile_09.png 的内容区域缩放对齐，居中粘贴到同款透明画布，覆盖保存。

依赖：pillow（pip install pillow）
"""

from pathlib import Path
from PIL import Image

SMILE_DIR   = Path("assets/smile")
REF_RANGE   = range(1, 6)   # smile_01 ~ smile_05 作为基准样本
ALIGN_RANGE = range(6, 10)  # smile_06 ~ smile_09 需要对齐
ALPHA_THRESHOLD = 10


def content_bbox(img: Image.Image) -> tuple[int, int, int, int] | None:
    """返回图片非透明内容的 bounding box，无内容返回 None。"""
    alpha = img.convert("RGBA").split()[3]
    mask  = alpha.point(lambda a: 255 if a >= ALPHA_THRESHOLD else 0)
    return mask.getbbox()


def main() -> None:
    # ---- 第一步：计算 smile_01~05 内容区域的平均宽高 ----
    ref_sizes: list[tuple[int, int]] = []
    canvas_size: tuple[int, int] | None = None

    for i in REF_RANGE:
        p = SMILE_DIR / f"smile_{i:02d}.png"
        if not p.exists():
            print(f"基准文件缺失，跳过：{p}")
            continue
        with Image.open(p) as img:
            img = img.convert("RGBA")
            if canvas_size is None:
                canvas_size = img.size
            bbox = content_bbox(img)
            if bbox is None:
                print(f"无内容像素，跳过：{p}")
                continue
            cw, ch = bbox[2] - bbox[0], bbox[3] - bbox[1]
            ref_sizes.append((cw, ch))
            print(f"  基准样本 {p.name}：内容 {cw} x {ch} px")

    if not ref_sizes:
        print("未能读取任何基准样本，退出。")
        return

    ref_w = round(sum(s[0] for s in ref_sizes) / len(ref_sizes))
    ref_h = round(sum(s[1] for s in ref_sizes) / len(ref_sizes))
    print(f"\n基准内容平均尺寸：{ref_w} x {ref_h} px")
    print(f"画布尺寸（来自 smile_01）：{canvas_size[0]} x {canvas_size[1]} px\n")

    # ---- 第二步：对齐 smile_06~09 ----
    changed = skipped = 0
    for i in ALIGN_RANGE:
        p = SMILE_DIR / f"smile_{i:02d}.png"
        if not p.exists():
            print(f"  文件缺失，跳过：{p}")
            skipped += 1
            continue

        with Image.open(p) as img:
            img   = img.convert("RGBA")
            bbox  = content_bbox(img)
            if bbox is None:
                print(f"  跳过（无内容像素）：{p}")
                skipped += 1
                continue
            orig_cw = bbox[2] - bbox[0]
            orig_ch = bbox[3] - bbox[1]
            content = img.crop(bbox)

        resized = content.resize((ref_w, ref_h), Image.LANCZOS)
        canvas  = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
        x = (canvas_size[0] - ref_w) // 2
        y = (canvas_size[1] - ref_h) // 2
        canvas.paste(resized, (x, y), resized)
        canvas.save(p, format="PNG")

        print(f"  {p.name}：内容 {orig_cw}x{orig_ch} → {ref_w}x{ref_h}，画布 {canvas_size[0]}x{canvas_size[1]}")
        changed += 1

    print(f"\n完成：处理 {changed} 张，跳过 {skipped} 张。")


if __name__ == "__main__":
    main()
