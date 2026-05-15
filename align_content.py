"""
align_content.py
以 assets/smile/smile_01.png 的内容区域（去透明后的实际像素范围）为基准，
将 assets/idle/ 和 assets/laugh/ 下每张 PNG 的内容区域缩放到相同大小，
居中粘贴到与 smile_01 画布等大的透明底上，覆盖保存。

结果：三套动画内容像素完全等大，C++ 侧统一 scale 不再有视觉偏差。

依赖：pillow（pip install pillow）
"""

from pathlib import Path
from PIL import Image

REFERENCE = Path("assets/smile/smile_01.png")
TARGETS = [
    Path("assets/idle"),
    Path("assets/laugh"),
]
# alpha 阈值：低于此值的像素视为透明，不计入 bounding box
ALPHA_THRESHOLD = 10


def content_bbox(img: Image.Image) -> tuple[int, int, int, int] | None:
    """返回图片中非透明内容的 bounding box (left, top, right, bottom)，无内容返回 None。"""
    alpha = img.convert("RGBA").split()[3]
    # 将低于阈值的 alpha 置零，再取 bbox
    threshold_mask = alpha.point(lambda a: 255 if a >= ALPHA_THRESHOLD else 0)
    return threshold_mask.getbbox()


def process(src: Path, ref_content_w: int, ref_content_h: int,
            canvas_w: int, canvas_h: int) -> str:
    """将单张图片的内容对齐到基准尺寸，返回操作描述字符串。"""
    with Image.open(src) as img:
        img = img.convert("RGBA")
        bbox = content_bbox(img)

        if bbox is None:
            return f"  跳过（无内容像素）：{src}"

        # 裁剪出内容区域，缩放到基准内容尺寸
        content = img.crop(bbox)
        resized = content.resize((ref_content_w, ref_content_h), Image.LANCZOS)

        # 居中粘贴到与 smile_01 等大的透明画布
        canvas = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
        x = (canvas_w - ref_content_w) // 2
        y = (canvas_h - ref_content_h) // 2
        canvas.paste(resized, (x, y), resized)

    canvas.save(src, format="PNG")
    cw, ch = bbox[2] - bbox[0], bbox[3] - bbox[1]
    return (f"  内容 {cw}x{ch} → {ref_content_w}x{ref_content_h}"
            f"，画布 {canvas_w}x{canvas_h}：{src}")


def main() -> None:
    if not REFERENCE.exists():
        print(f"基准文件不存在：{REFERENCE}")
        return

    # 计算基准：smile_01 的画布尺寸和内容区域尺寸
    with Image.open(REFERENCE) as ref_img:
        ref_img = ref_img.convert("RGBA")
        canvas_w, canvas_h = ref_img.size
        ref_bbox = content_bbox(ref_img)

    if ref_bbox is None:
        print(f"基准文件无有效内容像素：{REFERENCE}")
        return

    ref_content_w = ref_bbox[2] - ref_bbox[0]
    ref_content_h = ref_bbox[3] - ref_bbox[1]

    print(f"基准文件  ：{REFERENCE}")
    print(f"画布尺寸  ：{canvas_w} x {canvas_h} px")
    print(f"内容区域  ：{ref_content_w} x {ref_content_h} px  bbox={ref_bbox}\n")

    changed = skipped = 0
    for folder in TARGETS:
        for p in sorted(folder.glob("*.png")):
            result = process(p, ref_content_w, ref_content_h, canvas_w, canvas_h)
            print(result)
            if "跳过" in result:
                skipped += 1
            else:
                changed += 1

    print(f"\n完成：处理 {changed} 张，跳过 {skipped} 张。")
    print(f"所有图片内容区域已对齐为 {ref_content_w} x {ref_content_h} px，"
          f"画布 {canvas_w} x {canvas_h} px。")


if __name__ == "__main__":
    main()
