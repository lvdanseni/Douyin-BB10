import os, re

a = r"E:\PCdesk\桌面\bilibili\BiliBar\assets"

def read(p):
    with open(p, "rb") as f:
        return f.read().decode("utf-8", errors="replace")

def write(p, s):
    with open(p, "wb") as f:
        f.write(s.encode("utf-8"))

# 1) Root page files: import "." -> import "components"
for name in ["HomePage.qml", "PopularPage.qml", "RankingPage.qml", "RegionPage.qml", "SearchPage.qml"]:
    p = os.path.join(a, name)
    s = read(p)
    s2 = s.replace('import "."', 'import "components"')
    # 2) RegionPage: ComponentDefinition source path
    if name == "RegionPage.qml":
        s2 = s2.replace('source: "RegionChip.qml"', 'source: "components/RegionChip.qml"')
    write(p, s2)
    print("root page:", name, "changed:", s != s2)

# 3) components members: drop the import "." line entirely
for name in ["FeedPage.qml", "RegionChip.qml"]:
    p = os.path.join(a, "components", name)
    s = read(p)
    s2 = re.sub(r'^import "\.".*\n', '', s, count=1, flags=re.MULTILINE)
    write(p, s2)
    print("component:", name, "changed:", s != s2)

# 4) VideoPage / SettingsPage: drop import "."
for name in ["VideoPage.qml", "SettingsPage.qml"]:
    p = os.path.join(a, name)
    s = read(p)
    s2 = re.sub(r'^import "\.".*\n', '', s, count=1, flags=re.MULTILINE)
    write(p, s2)
    print("page:", name, "changed:", s != s2)
