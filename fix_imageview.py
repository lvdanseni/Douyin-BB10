import os, re, glob

d = r"E:\PCdesk\桌面\bilibili\BiliBar\assets"
total = 0
for p in glob.glob(os.path.join(d, "**", "*.qml"), recursive=True):
    with open(p, "rb") as f:
        s = f.read().decode("utf-8", errors="replace")
    s2, n = re.subn(r"(?m)^(\s*)Image\s*\{", r"\1ImageView {", s)
    if n:
        with open(p, "wb") as f:
            f.write(s2.encode("utf-8"))
        print("patched:", os.path.relpath(p, d), "->", n, "occurrences")
        total += n
print("total:", total)
