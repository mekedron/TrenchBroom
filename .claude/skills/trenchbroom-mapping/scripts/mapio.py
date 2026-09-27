import re
FACE=re.compile(r'\(\s*([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s*\)\s*\(\s*([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s*\)\s*\(\s*([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s*\)\s*(\S+)\s+(.*)')
def parse(text):
    """Returns list of entities: {'props': [(k,v)], 'brushes': [[face_line,...]]}"""
    ents=[]; cur=None; brush=None
    for line in text.splitlines():
        s=line.strip()
        if not s or s.startswith('//'): continue
        if s=='{':
            if cur is None: cur={'props':[],'brushes':[]}
            else: brush=[]
        elif s=='}':
            if brush is not None: cur['brushes'].append(brush); brush=None
            else: ents.append(cur); cur=None
        elif s.startswith('"'):
            m=re.match(r'"([^"]*)"\s+"([^"]*)"',s); cur['props'].append((m.group(1),m.group(2)))
        elif s.startswith('('):
            brush.append(s)
    return ents
def P(e): return dict(e['props'])
def face_points(line):
    m=FACE.match(line); g=m.groups()
    return [tuple(float(x) for x in g[i:i+3]) for i in (0,3,6)], g[9], g[10]
def bounds(brush):
    pts=[p for l in brush for p in face_points(l)[0]]
    return [min(p[i] for p in pts) for i in range(3)],[max(p[i] for p in pts) for i in range(3)]
