"""llvmcc.py: the viewer build's compiler driver (MSL_VIEWER_LLVM=1 in build_windows.sh, and tools/build/build_wasm_gx.sh). A drop-in for the compile
command line of one translation unit:

    llvmcc.py <compiler arguments of one compile, ending in -c file.c -o out.o>

Why: the console's float code (MWCC, fp_contract) fuses multiply-adds where the game's expression allows, takes square roots with its Gekko frsqrte
sequence, and the browser build (WebAssembly) has no scalar fused multiply-add. One LLVM pipeline for both targets gives one answer:

  1. clang -O2 -S -emit-llvm   (the same sources and defines as the gcc build; -ffp-contract=fast marks every fmul/fadd pair `contract`)
  2. tools/build/floatxform.py rewrites the IR of the functions the float rules list (tools/build/float_rules.json): a single-use fmul feeding an
     fadd/fsub in the same block becomes a call to llvm.fma, and the square root becomes the game's inline Gekko sequence (msl_llvm_sqrtf /
     msl_llvm_sqrt, src/present/llvm_float.c). Every `contract` flag is removed, so no backend ever fuses on its own.
  3. clang -c on the rewritten IR. llvm.fma lowers to vfmadd on x86 (-mfma) and to a call of fmaf (an exact software fma) on WebAssembly.
Functions not listed keep plain, rounded multiplies and adds, which is what the gcc build computes.

MSL_LLVM_TARGET=wasm drives emscripten's compiler (emcc) instead of the mingw clang. MSL_LLVM_ONLY=<regex> sends only the matching sources through
LLVM (the rest go to $CC, default gcc): a bisection aid. MSL_KEEP_LL=1 keeps the .ll files beside the object.
"""
import os
import re
import subprocess
import sys

WASM = os.environ.get('MSL_LLVM_TARGET', '') == 'wasm'
CLANG = os.environ.get('MSL_CLANG', 'clang')
SYSINC = os.environ.get('MSL_MINGW_INC', '')
HERE = os.path.dirname(os.path.abspath(__file__))
DROP = ('-fno-aggressive-loop-optimizations', '-g', '-g0', '-ffp-contract=off', '-ffp-contract=fast', '-ffp-contract=on', '-fno-pie', '-O2', '-O0', '-O1', '-O3', '-mfma')


def main():
    args = sys.argv[1:]
    out = args[args.index('-o') + 1]
    only = os.environ.get('MSL_LLVM_ONLY')
    if only and not any(re.search(only, a.replace(chr(92), '/')) for a in args if a.endswith('.c')):
        return subprocess.run([os.environ.get('CC', 'gcc')] + args).returncode
    if not WASM and any(a.startswith('-fexec-charset=') for a in args):
        # (clang knows only UTF-8 execution strings; the units with Shift-JIS strings are compiled as before)
        return subprocess.run([os.environ.get('CC', 'gcc')] + args).returncode
    keep = []
    i = 0
    src = None
    while i < len(args):
        a = args[i]
        if a == '-o':
            i += 2
            continue
        if a == '-c' or a in DROP:
            i += 1
            continue
        if a.endswith('.c') and not a.startswith('-'):
            src = a
        if a == '-MMD':
            keep += ['-MMD', '-MF', os.path.splitext(out)[0] + '.d', '-MT', out]
            i += 1
            continue
        keep.append(a)
        i += 1
    if WASM:
        cc = ['emcc']
        base = cc + ['-fno-math-errno', '-fno-vectorize', '-fno-slp-vectorize', '-fno-builtin-fmaf', '-fno-builtin-fma', '-w'] + keep
        tail = ['emcc', '-O2', '-w']
    else:
        cc = [CLANG, '--target=x86_64-w64-windows-gnu']
        base = cc + (['-isystem', SYSINC] if SYSINC else []) + ['-femulated-tls',  # (no -mfma here: the source's #if defined(__FMA__) hand-vectorized paths must not be taken, as in WebAssembly; the fma instructions come from the second step)
                                     '-fno-math-errno', '-fno-vectorize', '-fno-slp-vectorize', '-fno-builtin-fmaf', '-fno-builtin-fma', '-w'] + keep
        tail = [CLANG, '--target=x86_64-w64-windows-gnu', '-mfma', '-femulated-tls', '-O2', '-fPIC', '-ffunction-sections', '-fdata-sections', '-w']
    ll = out + '.ll'
    r = subprocess.run(base + ['-O2', '-gline-tables-only', '-ffp-contract=fast', '-S', '-emit-llvm', '-o', ll], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return 1
    rules = os.environ.get('MSL_FLOAT_RULES', os.path.join(HERE, 'float_rules.json'))
    ll2 = out + '.x.ll'
    r = subprocess.run([sys.executable, os.path.join(HERE, 'floatxform.py'), ll, ll2, rules, src or ''], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return 1
    r = subprocess.run(tail + ['-c', ll2, '-o', out], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return 1
    if not os.environ.get('MSL_KEEP_LL'):
        for p in (ll, ll2):
            try:
                os.remove(p)
            except OSError:
                pass
    return 0


sys.exit(main())
