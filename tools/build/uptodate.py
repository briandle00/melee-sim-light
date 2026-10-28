"""uptodate.py <object>: exit 0 when the object exists and is newer than its source and every header its .d file lists
(build_windows.sh INCREMENTAL=1)."""
import os, re, sys
out = sys.argv[1]
dep = out[:-2] + '.d'
if not (os.path.exists(out) and os.path.exists(dep)):
    sys.exit(1)
t = os.path.getmtime(out)
text = open(dep, encoding='utf-8', errors='replace').read().replace('\\n', ' ').replace('\\r\n', ' ')
head, _, rest = text.partition(': ')
for f in rest.split():
    if not os.path.exists(f) or os.path.getmtime(f) > t:
        sys.exit(1)
sys.exit(0)
