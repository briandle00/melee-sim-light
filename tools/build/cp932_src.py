#!/usr/bin/env python3
"""cp932_src.py <in> <out>: a UTF-8 source file with its non-ASCII string literals rewritten as the octal escapes of their Shift-JIS (CP932) bytes.

gcc's -finput-charset=UTF-8 -fexec-charset=CP932 (src/present/melee_gm_gm_1601.c) does this while compiling; emscripten's clang has no iconv, so
the WebAssembly build converts the file first and compiles the result as it is. Octal escapes (at most three digits) cannot merge with the
character that follows them, as hexadecimal ones could."""
import re
import sys

LIT = re.compile(r'"(?:[^"\\n]|\.)*"')


def conv(m):
    s = m.group(0)
    if s.isascii():
        return s
    out = []
    for ch in s:
        if ord(ch) < 128:
            out.append(ch)
        else:
            out.extend('\\%03o' % b for b in ch.encode('cp932'))
    return ''.join(out)


text = open(sys.argv[1], encoding='utf-8', newline='').read()
open(sys.argv[2], 'w', encoding='ascii', newline='').write(LIT.sub(conv, text))
