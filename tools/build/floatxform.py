"""floatxform.py in.ll out.ll rules.json [source.c]: the viewer build's float rules, applied to the LLVM IR of one translation unit (see llvmcc.py).

A function the rules select gets the console's arithmetic (MWCC, fp_contract on):
  fadd(fmul(a,b), c) / fadd(c, fmul(a,b))  ->  fma(a,b,c)                     fmadds / fmadd
  fsub(fmul(a,b), c)                       ->  fma(a,b,-c)                    fmsubs / fmsub
  fsub(c, fmul(a,b))                       ->  -fma(a,b,-c)                   fnmsubs / fnmsub (the sign of an exact zero as the instruction has it)
  when both operands are products, the first-listed one is fused and the other is rounded first.
  A product is fused only when it has no other use (MWCC does not keep a second copy of it).
  sqrt (llvm.sqrt / sqrtf / sqrt)          ->  msl_gekko_sqrtf / msl_gekko_sqrt (frsqrte seed, three Newton steps, frsp: src/MSL/math_ppc.h)
Every `contract` flag is removed from the module, so the backend never fuses on its own: functions the rules do not select compute plain, rounded
multiplies and adds, as the gcc build does (-ffp-contract=off). llvm.fma lowers to vfmadd on x86 and to a call of fmaf (exact) on WebAssembly.

rules.json: {"functions": [names], "files": [path fragments], "exclude": [names], "sqrt": true|false}
"""
import json
import re
import sys

TOK = re.compile(r'%[-\w.$"]+')
DEF = re.compile(r'^\s*(%[-\w.$"]+) = (.*)$')
FOP = re.compile(r'^(fmul|fadd|fsub|fneg)((?: [a-z]+)*) (float|double) (.*)$')


def select(rules, name, src):
    if name in rules.get('exclude', []):
        return False
    path = (src or '').replace(chr(92), '/')
    if any(d in path for d in rules.get('force_files', [])):
        return True
    if any(d in path for d in rules.get('not_in', [])):
        return False   # (code the simulator wrote itself, not the game's: its arithmetic is already what its author chose to match)
    if name in rules.get('functions', []):
        return True
    return any(f in (src or '').replace('\\', '/') for f in rules.get('files', []))


def split_ops(s):
    # "%a, %b" or "-0.0, %b" (operands of a binary float op, before any trailing ", !tag")
    parts = [p.strip() for p in s.split(',')]
    return parts


DBG = re.compile(r', !dbg (!\d+)')


def transform_function(lines, rules, need, loc):
    n = len(lines)
    defs = {}      # name -> (index, op, type, operands)
    pos = {}       # name -> (line, column) of the instruction in the source, from its !dbg location
    uses = {}
    bb = {}        # instruction index -> basic block number
    cur_bb = 0
    for i, l in enumerate(lines):
        if re.match(r'^[-\w.$"]+:', l):
            cur_bb += 1
        bb[i] = cur_bb
        m = DEF.match(l)
        if m:
            nm, rest = m.groups()
            fm = FOP.match(rest)
            if fm:
                defs[nm] = (i, fm.group(1), fm.group(3), split_ops(fm.group(4)))
                dm = DBG.search(rest)
                if dm and dm.group(1) in loc:
                    pos[nm] = loc[dm.group(1)]
            first = True
            for t in TOK.findall(rest):
                uses[t] = uses.get(t, 0) + 1
        else:
            for t in TOK.findall(l):
                uses[t] = uses.get(t, 0) + 1
    dead = set()
    out = list(lines)
    fresh = [0]

    def newname():
        fresh[0] += 1
        return '%%fx.%d' % fresh[0]

    for i, l in enumerate(lines):
        m = DEF.match(l)
        if not m:
            continue
        nm, rest = m.groups()
        fm = FOP.match(rest)
        if not fm or fm.group(1) not in ('fadd', 'fsub'):
            continue
        op, ty = fm.group(1), fm.group(3)
        a, b = split_ops(fm.group(4))[:2]

        def ismul(x):
            d = defs.get(x)
            if d is not None and rules.get('same_block', True) and bb[d[0]] != bb[i]:
                return False   # (a product is fused where the expression is: not when the optimizer moved it to another block)
            if d is None or d[1] != 'fmul' or d[2] != ty or uses.get(x, 0) != 1 or x in dead:
                return False
            if rules.get('nosquare', True) and d[3][0] == d[3][1]:
                return False   # (a * a is not fused: the sums of squares of the game's vector lengths are mul, mul, add)
            return True

        intr = 'llvm.fma.f32' if ty == 'float' else 'llvm.fma.f64'
        need.add(ty)
        indent = re.match(r'^(\s*)', l).group(1)
        if op == 'fadd' and ismul(a) and ismul(b) and a in pos and b in pos:
            if nm in pos and (pos[a][0] == pos[nm][0]) != (pos[b][0] == pos[nm][0]):
                # (a product written in an earlier statement is a variable to the console's compiler, not a part of this expression: the one written in the sum's own
                # statement is fused, and the other, rounded first: angle = 0.5f * x + offset, with offset = c * r on its own line, is fmadds(0.5, x, offset))
                if pos[b][0] == pos[nm][0]:
                    a, b = b, a
            elif pos[b] < pos[a]:
                a, b = b, a    # (the optimizer may have swapped the operands of a sum of two products: the one written first in the source is fused)
        if ismul(a):
            x, y = defs[a][3][:2]
            dead.add(a)
            if op == 'fadd':
                out[i] = '%s%s = call %s @%s(%s %s, %s %s, %s %s)' % (indent, nm, ty, intr, ty, x, ty, y, ty, b)
            else:
                ng = newname()
                out[i] = '%s%s = fneg %s %s\n%s%s = call %s @%s(%s %s, %s %s, %s %s)' % (indent, ng, ty, b, indent, nm, ty, intr, ty, x, ty, y, ty, ng)
        elif ismul(b):
            x, y = defs[b][3][:2]
            dead.add(b)
            if op == 'fadd':
                out[i] = '%s%s = call %s @%s(%s %s, %s %s, %s %s)' % (indent, nm, ty, intr, ty, x, ty, y, ty, a)
            else:
                ng, fv = newname(), newname()
                out[i] = '%s%s = fneg %s %s\n%s%s = call %s @%s(%s %s, %s %s, %s %s)\n%s%s = fneg %s %s' % (
                    indent, ng, ty, a, indent, fv, ty, intr, ty, x, ty, y, ty, ng, indent, nm, ty, fv)
    for d in dead:
        out[defs[d][0]] = None
    return [l for l in out if l is not None]


def main():
    src_ll, dst_ll, rules_path = sys.argv[1:4]
    src = sys.argv[4] if len(sys.argv) > 4 else ''
    try:
        rules = json.load(open(rules_path))
    except OSError:
        rules = {}
    text = open(src_ll, encoding='utf-8', errors='surrogateescape').read()
    loc = {}
    for m in re.finditer(r'^(!\d+) = !DILocation\(line: (\d+), column: (\d+)', text, re.M):
        loc[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    lines = text.split('\n')
    out = []
    need = set()
    i = 0
    do_sqrt = rules.get('sqrt', True)
    sq_need = set()
    while i < len(lines):
        l = lines[i]
        m = re.match(r'^define .*@([-\w.$"]+)\(', l)
        if m and l.rstrip().endswith('{'):
            j = i + 1
            while lines[j] != '}':
                j += 1
            name = m.group(1).strip('"')
            body = lines[i + 1:j]
            if select(rules, name, src):
                body = transform_function(body, rules, need, loc)
                if do_sqrt:
                    nb = []
                    for b in body:
                        b2 = b.replace('call float @llvm.sqrt.f32(', 'call float @msl_llvm_sqrtf(').replace('call double @llvm.sqrt.f64(', 'call double @msl_llvm_sqrt(')
                        b2 = re.sub(r'call (?:[a-z ]+ )?float @sqrtf\(', 'call float @msl_llvm_sqrtf(', b2)
                        b2 = re.sub(r'call (?:[a-z ]+ )?double @sqrt\(', 'call double @msl_llvm_sqrt(', b2)
                        if b2 != b:
                            sq_need.add('f' if 'sqrtf' in b2 else 'd')
                        nb.append(b2)
                    body = nb
            out.append(l)
            out.extend(body)
            out.append('}')
            i = j + 1
            continue
        out.append(l)
        i += 1
    text = '\n'.join(out)
    # explicit fmaf() / fma() of the sources (stage 1 keeps them as opaque library calls: -fno-builtin-fmaf, so the optimizer cannot rewrite fma(x, 1.0, y)
    # into a plain add that the rule above would then fuse with a product); they are the fused operation: llvm.fma
    fmaf_re = re.compile(r'call (?:[a-z]+ )*float @fmaf\(float(?: [a-z]+)* ([^,]+), float(?: [a-z]+)* ([^,]+), float(?: [a-z]+)* ([^)]+)\)')
    fma_re = re.compile(r'call (?:[a-z]+ )*double @fma\(double(?: [a-z]+)* ([^,]+), double(?: [a-z]+)* ([^,]+), double(?: [a-z]+)* ([^)]+)\)')
    if '@fmaf(' in text:
        text = fmaf_re.sub(lambda m: 'call float @llvm.fma.f32(float %s, float %s, float %s)' % m.groups(), text)
        need.add('float')
    if '@fma(' in text:
        text = fma_re.sub(lambda m: 'call double @llvm.fma.f64(double %s, double %s, double %s)' % m.groups(), text)
        need.add('double')
    text = text.replace(' contract ', ' ')
    text = re.sub(r', !dbg !\d+', '', text)
    decl = []
    if 'float' in need and '@llvm.fma.f32(float, float, float)' not in text.split('declare')[0] and 'declare float @llvm.fma.f32' not in text:
        decl.append('declare float @llvm.fma.f32(float, float, float)')
    if 'double' in need and 'declare double @llvm.fma.f64' not in text:
        decl.append('declare double @llvm.fma.f64(double, double, double)')
    if 'f' in sq_need and 'declare float @msl_llvm_sqrtf' not in text:
        decl.append('declare float @msl_llvm_sqrtf(float)')
    if 'd' in sq_need and 'declare double @msl_llvm_sqrt' not in text:
        decl.append('declare double @msl_llvm_sqrt(double)')
    if decl:
        text += '\n' + '\n'.join(decl) + '\n'
    open(dst_ll, 'w', encoding='utf-8', errors='surrogateescape').write(text)


main()
