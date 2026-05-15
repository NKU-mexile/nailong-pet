import os

folders = {
    "assets/idle":  "idle",
    "assets/smile": "smile",
    "assets/laugh": "laugh",
    "assets/cry":   "cry",
}

for folder, prefix in folders.items():
    pngs = sorted(
        f for f in os.listdir(folder) if f.lower().endswith(".png")
    )
    for i, name in enumerate(pngs, start=1):
        src = os.path.join(folder, name)
        dst = os.path.join(folder, f"{prefix}_{i:02d}.png")
        if src != dst:
            os.rename(src, dst)
            print(f"  {src}  ->  {dst}")
        else:
            print(f"  {dst}  (unchanged)")
