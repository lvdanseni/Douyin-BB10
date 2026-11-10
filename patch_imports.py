import os, re, glob

d = r"E:\PCdesk\桌面\bilibili\BiliBar\assets"
for p in glob.glob(os.path.join(d, "*.qml")):
    with open(p, "rb") as f:
        raw = f.read()
    try:
        s = raw.decode("utf-8")
    except UnicodeDecodeError:
        s = raw.decode("utf-8", errors="replace")
    if re.search(r"import bb\.cascades 1\.4", s) and not re.search(r'import "\."', s):
        s2 = re.sub(r"(import bb\.cascades 1\.4\s*)", r"\1\nimport \".\"\n", s, count=1)
        with open(p, "wb") as f:
            f.write(s2.encode("utf-8"))
        print("patched:", os.path.basename(p))
    else:
        print("skip:", os.path.basename(p))
