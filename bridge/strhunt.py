import os
needles = [
    "Direct3D 12 is not supported",
    "Application will attempt to launch using",
    "is not supported on your system",
    "attempt to launch using Direct3D 11",
]
dirs = [
    r"D:\SteamLibrary\steamapps\common\Returnal\Returnal\Binaries\Win64",
    r"D:\SteamLibrary\steamapps\common\Returnal\Engine\Plugins\Streamline\Binaries\ThirdParty\Win64",
]
for d in dirs:
    for name in sorted(os.listdir(d)):
        if not name.lower().endswith((".exe", ".dll", ".addon64", ".asi")):
            continue
        p = os.path.join(d, name)
        try:
            if os.path.getsize(p) > 400 * 1024 * 1024:
                print("SKIP huge", name); continue
            data = open(p, "rb").read()
        except Exception as e:
            print("SKIP", name, e); continue
        hits = []
        for n in needles:
            u = n.encode("utf-16-le"); a = n.encode("ascii", "ignore")
            c16 = data.count(u); c8 = data.count(a)
            if c16 or c8:
                hits.append("%-46s utf16=%-3d ascii=%-3d" % (n, c16, c8))
        if hits:
            print("== " + name + " ==")
            for h in hits: print("   " + h)
print("string hunt done")
