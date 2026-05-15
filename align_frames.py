"""
align_frames.py
以 assets/smile/smile_01.png 的尺寸为基准，
将 assets/idle/ 和 assets/laugh/ 下所有 PNG 直接 resize 到相同尺寸并覆盖保存。

依赖：pillow（pip install pillow）
"""

from pathlib import Path
from PIL import Image

REFERENCE = Path("assets/smile/smile_01.png")
TARGETS = [
    Path("assets/idle"),
    Path("assets/laugh"),
]


def main() -> None:
    if not REFERENCE.exists():
        print(f"基准文件不存在：{REFERENCE}")
        return

    with Image.open(REFERENCE) as ref:
        target_size = ref.size  # (width, height)

    print(f"基准尺寸（来自 {REFERENCE}）：{target_size[0]} x {target_size[1]} px\n")

    changed = skipped = 0
    for folder in TARGETS:
        for p in sorted(folder.glob("*.png")):
            with Image.open(p) as img:
                if img.size == target_size:
                    print(f"  跳过（已达目标尺寸）：{p}")
                    skipped += 1
                    continue
                orig_size = img.size
                resized = img.resize(target_size, Image.LANCZOS)

            resized.save(p, format="PNG")
            print(f"  {orig_size[0]}x{orig_size[1]} → {target_size[0]}x{target_size[1]}：{p}")
            changed += 1

    print(f"\n完成：处理 {changed} 张，跳过 {skipped} 张。")
    print(f"所有图片已统一为 {target_size[0]} x {target_size[1]} px。")


if __name__ == "__main__":
    main()
