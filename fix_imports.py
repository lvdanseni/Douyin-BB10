import os, glob

d = r"E:\PCdesk\桌面\bilibili\BiliBar\assets"
for p in glob.glob(os.path.join(d, "*.qml")):
    with open(p, "rb") as f:
        raw = f.read()
    s = raw.decode("utf-8", errors="replace")
    if 'import \\".\\"' in s:
        s2 = s.replace('import \\".\\"', 'import "."')
        with open(p, "wb") as f:
            f.write(s2.encode("utf-8"))
        print("fixed:", os.path.basename(p))
    else:
        print("ok:", os.path.basename(p))
