"""
compute_body_bbox.py
计算 assets/idle/idle_01.png 奶龙内容的 bounding box，
输出供 C++ 碰撞检测使用的四个常量：
  offsetX, offsetY  —— 内容左上角相对于图片左上角的偏移（原始像素）
  contentW, contentH —— 内容区域宽高（原始像素）

依赖：pillow（pip install pillow）
"""

from pathlib import Path
from PIL import Image

SRC = Path("assets/idle/idle_01.png")
ALPHA_THRESHOLD = 10


def main() -> None:
    if not SRC.exists():
        print(f"文件不存在：{SRC}")
        return

    with Image.open(SRC) as img:
        img  = img.convert("RGBA")
        iw, ih = img.size
        alpha = img.split()[3]
        mask  = alpha.point(lambda a: 255 if a >= ALPHA_THRESHOLD else 0)
        bbox  = mask.getbbox()  # (left, top, right, bottom)

    if bbox is None:
        print("未找到非透明像素")
        return

    ox, oy   = bbox[0], bbox[1]
    cw, ch   = bbox[2] - bbox[0], bbox[3] - bbox[1]

    print(f"图片尺寸     : {iw} x {ih}")
    print(f"内容 bbox    : left={bbox[0]}  top={bbox[1]}  right={bbox[2]}  bottom={bbox[3]}")
    print(f"offsetX={ox}  offsetY={oy}  contentW={cw}  contentH={ch}")
    print()
    print("// ---- 将以下常量复制到 C++ Physics 结构体中 ----")
    print(f"static constexpr int IDLE_BBOX_OX = {ox};  // 内容左边距（原始像素）")
    print(f"static constexpr int IDLE_BBOX_OY = {oy};  // 内容上边距（原始像素）")
    print(f"static constexpr int IDLE_BBOX_W  = {cw};  // 内容宽度（原始像素）")
    print(f"static constexpr int IDLE_BBOX_H  = {ch};  // 内容高度（原始像素）")
    print(f"// 图片原始尺寸：{iw} x {ih}")


if __name__ == "__main__":
    main()
