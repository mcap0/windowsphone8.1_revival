# print the latest version of a OneNote page as an outline tree
import sys, re
from pyOneNote.OneDocument import OneDocment
doc = OneDocment(open(sys.argv[1], 'rb'))
title = sys.argv[2].lower() if len(sys.argv) > 2 else None
props = doc.get_properties()
last = {}
for p in props: last[str(p['identity'])] = p
def text(v):
    if 'RichEditTextUnicode' in v: return v['RichEditTextUnicode']
    if 'TextExtendedAscii' in v:
        x = v['TextExtendedAscii']
        try: return bytes.fromhex(x).decode('cp1252')
        except Exception: return x.encode('utf-16-le').decode('cp1252', 'replace')
    return None
KIDS = ('StructureElementChildNodes', 'ElementChildNodes', 'ElementChildNodesOfVersionHistory',
        'ContentChildNodes', 'ContentChildNodesOfPageManifest', 'ChildGraphSpaceElementNodes')
def walk(k, d, seen):
    if k in seen or k not in last: return
    seen.add(k); p = last[k]; v = p['val']
    t = text(v)
    tags = v.get('prop_40003489')   # NoteTags: checkbox, ActionItemStatus bit 0 = checked
    box = ''
    if isinstance(tags, list) and tags and isinstance(tags[0], dict):
        box = '[x] ' if '\\x01' in tags[0].get('ActionItemStatus', '') else '[ ] '
    if t is not None and t.strip(): print('  ' * d + box + t.strip())
    for kk in KIDS:
        for c in v.get(kk, []) or []:
            walk(str(c), d + 1, seen)
pages = [p for p in props if p['type'] == 'jcidPageNode']
seen_pages = []
for p in pages:
    k = str(p['identity'])
    if k not in seen_pages: seen_pages.append(k)
for k in seen_pages:
    print('=== page', k, last[k]['val'].get('LastModifiedTime'))
    walk(k, 0, set())
