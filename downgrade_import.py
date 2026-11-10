import os, re, glob

d = r"E:\PCdesk\桌面\bilibili\BiliBar\assets"
for p in glob.glob(os.path.join(d, "*.qml")):
    with open(p, "rb") as f:
        raw = f.read()
    s = raw.decode("utf-8", errors="replace")
    if "import bb.cascades 1.4" in s:
        s2 = s.replace("import bb.cascades 1.4", "import bb.cascades 1.1")
        with open(p, "wb") as f:
            f.write(s2.encode("utf-8"))
        print("downgraded:", os.path.basename(p))
    else:
        print("skip:", os.path.basename(p))
