#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""tooltip_expr.py: record database access, expression evaluator, and client number formatters."""
import gzip
import json
import math
import sqlite3

T_ABILITY, T_HIT, T_EFFECT, T_STATMOD, T_FORMULA, T_EQUATION, T_CURVE, T_TERM = (
    'AoCAbilityRecordBase', 'AbilityHitRecordBase', 'EffectRecordBase', 'StatModRecordBase',
    'StatFormulaTypeRecordBase', 'StatEquationTypeRecordBase', 'StatCurveRecordBase', 'TermRecordBase')
TYPE_SCHEMA = {
    1841048597116038327: T_ABILITY, 3672506661632452365: T_HIT, 3377240744094973029: T_EFFECT,
    676208982154769454: T_STATMOD, 4547065559311335368: T_FORMULA, 7180619218012719533: T_EQUATION,
    7037248826340794574: T_CURVE, 6137747221710071900: T_TERM, 979581256175327151: 'ProjectileRecordBase',
    7793593157135267051: 'LingeringEffectRecordBase', 3999912890578843546: 'AbilityTargetingRecordBase',
    4146438674247114715: 'SkillRecordBase', 297887639323616934: 'SkillTreeRecordBase',
    5365104778484440215: 'StatTypeDefRecordBase'}

HEALTH_STAT = 108986352686712
TERM_BASE, TERM_MIN, TERM_MAX, TERM_ADD = (
    6064631035951644675, 6064631120485154816, 6064631120485482497, 6064631210309517324)

SCHEMA_TYPE = {}


class DB:
    def __init__(self, path):
        self.path = path
        self.c = sqlite3.connect('file:%s?mode=ro' % path.replace(chr(92), '/'), uri=True)
        self.cache = {}
        for t, sch in TYPE_SCHEMA.items():
            SCHEMA_TYPE[sch] = t
        self.names = {}
        marks = ','.join('?' * len(SCHEMA_TYPE))
        query = 'select schema,name,guid from record where schema in (%s)' % marks
        for sch, nm, g in self.c.execute(query, list(SCHEMA_TYPE)):
            d = self.names.setdefault(sch, {})
            d.setdefault(nm, g)
            d.setdefault(nm.lower(), g)

    def rec(self, schema, guid):
        k = (schema, guid)
        if k not in self.cache:
            r = self.c.execute(
                'select data,name from record where type_id=? and guid=?',
                (SCHEMA_TYPE[schema], guid)).fetchone()
            if r:
                j = json.loads(gzip.decompress(r[0]))
                j['__name'] = r[1]
                j['__guid'] = guid
                self.cache[k] = j
            else:
                self.cache[k] = None
        return self.cache[k]

    def ref(self, r):
        """a {Guid, TypeId} reference -> the record (None for the null reference)"""
        if not r or not r.get('Guid'):
            return None
        sch = TYPE_SCHEMA.get(r.get('TypeId'))
        return self.rec(sch, r['Guid']) if sch else None

    def by_name(self, schema, name):
        g = self.names.get(schema, {}).get(name)
        if g is None:
            g = self.names.get(schema, {}).get(name.lower())
        return self.rec(schema, g) if g is not None else None

    def stat_name(self, guid):
        r = self.rec('StatTypeDefRecordBase', guid)
        return r['__name'] if r else str(guid)

    def stat_display(self, guid):
        r = self.rec('StatTypeDefRecordBase', guid)
        if not r:
            return str(guid)
        dn = r.get('DisplayName')
        return (dn.get('Source') if isinstance(dn, dict) else dn) or r['__name']


class Unresolved(Exception):
    pass


class _Obj:
    def __init__(self, what):
        self.what = what


SELF, TARGET = _Obj('caster'), _Obj('target')
KW = ('const', 'var', 'set', 'if', 'else', 'return')
PUNCT = ['&&', '||', '==', '!=', '<=', '>=', '+', '-', '*', '/', '%', '<', '>', '!', '=', '(', ')', '{', '}', ',', ';']
BIN = {'||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '<=': 4, '>': 4, '>=': 4, '+': 5, '-': 5, '*': 6, '/': 6, '%': 6}


def _skip_ws_or_comment(src, i, n):
    if src[i] in ' \t\r\n':
        return i + 1
    if src.startswith('//', i):
        while i < n and src[i] != '\n':
            i += 1
        return i
    if src.startswith('/*', i):
        j = src.find('*/', i + 2)
        return n if j < 0 else j + 2
    return i


def _lex_number(src, i, n):
    j = i
    while j < n and src[j].isdigit():
        j += 1
    if j < n and src[j] == '.':
        j += 1
        while j < n and src[j].isdigit():
            j += 1
    return ('num', float(src[i:j]), '.' in src[i:j]), j


def _lex_ident_or_enum(src, i, n):
    j = i
    while j < n and (src[j].isalnum() or src[j] == '_'):
        j += 1
    last = i
    while j + 2 < n and src[j:j + 2] == '::' and (src[j + 2].isalpha() or src[j + 2] == '_'):
        last = j + 2
        j += 2
        while j < n and (src[j].isalnum() or src[j] == '_'):
            j += 1
    if last != i:
        return ('enum', src[i:last - 2], src[last:j]), j
    return ('id', src[i:j]), j


def _lex_dollar_ref(src, i):
    j = src.find('$', i + 1)
    body = src[i + 1:j]
    if body.startswith('#'):
        t, g = body[1:].split(':')
        return ('ref', int(t), int(g)), j + 1
    k, nm = body.split(':', 1)
    return ('nref', k, nm), j + 1


def _lex_string(src, i):
    c = src[i]
    j = i + 1
    s = ''
    while src[j] != c:
        if src[j] == '\\':
            j += 1
        s += src[j]
        j += 1
    return ('str', s), j + 1


def _lex_punct(src, i):
    for p in PUNCT:
        if src.startswith(p, i):
            return ('p', p), i + len(p)
    raise Unresolved('lex %r' % src[i])


def _lex_token(src, i, n):
    c = src[i]
    if c.isdigit() or (c == '.' and i + 1 < n and src[i + 1].isdigit()):
        return _lex_number(src, i, n)
    if c.isalpha() or c == '_':
        return _lex_ident_or_enum(src, i, n)
    if c == '$':
        return _lex_dollar_ref(src, i)
    if c in '"\'':
        return _lex_string(src, i)
    return _lex_punct(src, i)


def lex(src):
    out, i, n = [], 0, len(src)
    while i < n:
        j = _skip_ws_or_comment(src, i, n)
        if j > i:
            i = j
            continue
        tok, i = _lex_token(src, i, n)
        out.append(tok)
    out.append(('eof',))
    return out


class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self):
        return self.t[self.i]

    def nxt(self):
        t = self.t[self.i]
        self.i += 1
        return t

    def ispunct(self, s):
        t = self.peek()
        return t[0] == 'p' and t[1] == s

    def expect(self, s):
        if not self.ispunct(s):
            raise Unresolved('parse: expected %s' % s)
        self.i += 1

    def block(self, braced):
        stmts, res = [], None
        while True:
            t = self.peek()
            if t[0] == 'eof':
                return stmts, res
            if braced and self.ispunct('}'):
                self.i += 1
                return stmts, res
            if res is not None:
                raise Unresolved('parse: after final expression')
            s, e = self.statement()
            if e is not None:
                res = e
            else:
                stmts.append(s)

    def assignment(self):
        name = self.nxt()[1].lower()
        self.expect('=')
        x = self.expr(0)
        self.expect(';')
        return name, x

    def statement(self):
        t = self.peek()
        if t[0] == 'id':
            if t[1] in ('const', 'var'):
                self.i += 1
                n, x = self.assignment()
                return ('decl', n, x), None
            if t[1] == 'set':
                self.i += 1
                n, x = self.assignment()
                return ('set', n, x), None
            if t[1] == 'if':
                return self.ifstmt(), None
            if t[1] == 'return':
                self.i += 1
                x = self.expr(0)
                if self.ispunct(';'):
                    self.i += 1
                return None, x
        x = self.expr(0)
        if self.ispunct(';'):
            self.i += 1
            return ('expr', x), None
        return None, x

    def ifstmt(self):
        self.i += 1
        self.expect('(')
        cond = self.expr(0)
        self.expect(')')
        self.expect('{')
        then, _ = self.block(True)
        els = []
        t = self.peek()
        if t[0] == 'id' and t[1] == 'else':
            self.i += 1
            t = self.peek()
            if t[0] == 'id' and t[1] == 'if':
                els = [self.ifstmt()]
            else:
                self.expect('{')
                els, _ = self.block(True)
        return ('if', cond, then, els)

    def expr(self, mn):
        left = self.unary()
        while self.peek()[0] == 'p' and self.peek()[1] in BIN and BIN[self.peek()[1]] > mn:
            op = self.nxt()[1]
            right = self.expr(BIN[op])
            left = ('bin', op, left, right)
        return left

    def unary(self):
        if self.peek()[0] == 'p' and self.peek()[1] in ('-', '!'):
            op = self.nxt()[1]
            return ('un', op, self.unary())
        return self.primary()

    def primary(self):
        t = self.nxt()
        k = t[0]
        if k == 'num':
            return ('num', t[1])
        if k in ('ref', 'nref'):
            return (k, t[1], t[2])
        if k == 'str':
            return ('str', t[1])
        if k == 'enum':
            return ('enum', t[1] + '::' + t[2])
        if k == 'id':
            return self._primary_id(t[1])
        if k == 'p' and t[1] == '(':
            x = self.expr(0)
            self.expect(')')
            return x
        raise Unresolved('parse: unexpected %r' % (t,))

    def _primary_id(self, ident):
        if ident in ('true', 'false'):
            return ('bool', ident == 'true')
        if not self.ispunct('('):
            return ('var', ident.lower())
        self.i += 1
        args = []
        while not self.ispunct(')'):
            if args:
                self.expect(',')
            args.append(self.expr(0))
        self.i += 1
        return ('call', ident.lower(), args)


class Env:
    """what an expression sees: the level, the skills bought, no effects, no stats; `terms` are the GetTerm values"""

    def __init__(self, db, level=25, skills='none', terms=None):
        self.db, self.level, self.skills, self.terms = db, level, skills, terms or {}
        self.depth = 0


def _num(v):
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    if isinstance(v, (int, float)):
        return float(v)
    raise Unresolved('not a number: %r' % (v,))


def _trunc(v):
    return float(math.trunc(v))


def _set_var(sc, name, val):
    for d in reversed(sc):
        if name in d:
            d[name] = val
            return


def _lookup_var(sc, name):
    for d in reversed(sc):
        if name in d:
            return d[name]
    raise Unresolved('variable ' + name)


_NUM_BIN_OPS = {
    '<': lambda a, b: a < b,
    '<=': lambda a, b: a <= b,
    '>': lambda a, b: a > b,
    '>=': lambda a, b: a >= b,
    '+': lambda a, b: a + b,
    '-': lambda a, b: a - b,
    '*': lambda a, b: a * b,
    '/': lambda a, b: a / b if b else 0.0,
    '%': lambda a, b: math.fmod(a, b) if b else 0.0,
}

_CONST_CALLS = {
    'getinstigator': SELF, 'getowner': SELF, 'getsource': SELF,
    'getplayer': SELF, 'getself': SELF, 'getcreatureowner': SELF,
    'gettarget': TARGET, 'getactivetarget': TARGET,
    'gethitinprogress': None, 'gethitrecord': None, 'gethitsourcerecord': None,
    'getabilityprogressratio': 0.0,
    'hithastag': False,
    'getrecordtypeid': 0.0,
    'getprojectiletypeid': 1.0,
    'getcurrentweaponskilltree': ('ref', 297887639323616934, 0),
    'geteffectcount': 0.0, 'geteffectcountbytag': 0.0, 'geteffectcountbyrecordandinstigator': 0.0,
}

_MATH_CALLS = {
    'float': lambda a: _num(a[0]),
    'int': lambda a: _trunc(_num(a[0])),
    'round': lambda a: float(math.floor(_num(a[0]) + 0.5)),
    'abs': lambda a: abs(_num(a[0])),
    'max': lambda a: max(_num(x) for x in a),
    'min': lambda a: min(_num(x) for x in a),
    'clamp': lambda a: min(max(_num(a[0]), _num(a[1])), _num(a[2])),
    'lerp': lambda a: _num(a[0]) + (_num(a[1]) - _num(a[0])) * _num(a[2]),
    'selectfloat': lambda a: a[1] if a[0] else a[2],
    'selectint': lambda a: a[1] if a[0] else a[2],
    'select': lambda a: a[1] if a[0] else a[2],
}


class Interp:
    def __init__(self, env):
        self.env = env

    def run(self, src):
        stmts, res = Parser(lex(src)).block(False)
        if res is None and stmts and stmts[-1][0] == 'expr':
            res = stmts.pop()[1]
        if res is None:
            raise Unresolved('no value')
        sc = [{}]
        self.stmts(stmts, sc)
        return self.ev(res, sc)

    def stmts(self, lst, sc):
        for s in lst:
            if s[0] == 'decl':
                sc[-1][s[1]] = self.ev(s[2], sc)
            elif s[0] == 'set':
                _set_var(sc, s[1], self.ev(s[2], sc))
            elif s[0] == 'if':
                c = self.ev(s[1], sc)
                self.stmts(s[2] if c else s[3], sc + [{}])
            elif s[0] == 'expr':
                self.ev(s[1], sc)

    def ev(self, e, sc):
        k = e[0]
        if k in ('num', 'bool', 'str', 'enum'):
            return e[1]
        if k in ('ref', 'nref'):
            return (k, e[1], e[2])
        if k == 'var':
            return _lookup_var(sc, e[1])
        if k == 'un':
            x = self.ev(e[2], sc)
            return -_num(x) if e[1] == '-' else (not x)
        if k == 'bin':
            return self._ev_bin(e[1], e[2], e[3], sc)
        if k == 'call':
            return self.call(e[1], [self.ev(a, sc) for a in e[2]], e[2], sc)
        raise Unresolved('expr ' + k)

    def _ev_bin(self, op, left, right, sc):
        if op == '&&':
            return bool(self.ev(left, sc)) and bool(self.ev(right, sc))
        if op == '||':
            return bool(self.ev(left, sc)) or bool(self.ev(right, sc))
        a, b = self.ev(left, sc), self.ev(right, sc)
        if op == '==':
            return a == b
        if op == '!=':
            return a != b
        return _NUM_BIN_OPS[op](_num(a), _num(b))

    def call(self, name, a, raw, sc):
        if name in _CONST_CALLS:
            return _CONST_CALLS[name]
        if name in _MATH_CALLS:
            return _MATH_CALLS[name](a)
        return self._call_env(name, a)

    def _call_env(self, name, a):
        env = self.env
        if name in ('getcharacterlevel', 'getplayerlevel'):
            return float(env.level)
        if name == 'getskillpurchased':
            if env.skills in ('all', 'none'):
                return env.skills == 'all'
            return a[2][2] in env.skills
        if name == 'getterm':
            return self._resolve_term(a[0][2])
        if name in ('evalformula', 'evalequation'):
            return eval_formula(env, a[0])
        if name == 'evalcurve':
            return eval_curve(env.db, a[0], _num(a[1]))
        if name == 'getstat':
            who = 'caster' if a[0] is SELF else 'target'
            stat = env.db.stat_name(a[1][2]) if isinstance(a[1], tuple) else a[1]
            raise Unresolved('GetStat(%s, %s)' % (who, stat))
        raise Unresolved('function ' + name)

    def _resolve_term(self, g):
        if g not in self.env.terms:
            raise Unresolved('term %d not given' % g)
        v = self.env.terms[g]
        if isinstance(v, LazyTerm):
            rem = {k: x for k, x in self.env.terms.items() if k != g}
            v = Interp(Env(v.env.db, v.env.level, v.env.skills, rem)).run(v.src)
            self.env.terms[g] = v
        return v


def eval_curve(db, ref, x):
    r = db.rec(T_CURVE, ref[2])
    if r is None:
        raise Unresolved('curve %d' % ref[2])
    keys = r['Curve']['EditorCurveData']['Keys']
    if not keys:
        return float(r['Curve']['EditorCurveData']['DefaultValue'])
    if x <= keys[0]['Time']:
        return float(keys[0]['Value'])
    for i in range(1, len(keys)):
        if x <= keys[i]['Time']:
            a, b = keys[i - 1], keys[i]
            t = (x - a['Time']) / (b['Time'] - a['Time']) if b['Time'] != a['Time'] else 0
            return float(a['Value'] + (b['Value'] - a['Value']) * t)
    return float(keys[-1]['Value'])


def eval_formula(env, ref):
    """EvalFormula($#type:guid$): the formula's equation over its InputTerms and the caller's terms"""
    if ref[0] != 'ref':
        raise Unresolved('formula ref')
    f = env.db.rec(T_FORMULA, ref[2])
    if f is None:
        raise Unresolved('formula %d' % ref[2])
    if env.depth > 12:
        raise Unresolved('formula depth')
    eq = env.db.ref(f['EquationId'])
    sub = Env(env.db, env.level, env.skills, dict(env.terms))
    sub.depth = env.depth + 1
    for t in f['InputTerms']:
        g = t['TermId']['Guid']
        if g not in sub.terms:
            sub.terms[g] = LazyTerm(sub, t['Value'])
    return run_expr(sub, eq['Equation'], lazy=True)


class LazyTerm:
    def __init__(self, env, src):
        self.env, self.src = env, src


def run_expr(env, src, lazy=False):
    if lazy:
        e2 = Env(env.db, env.level, env.skills, dict(env.terms))
        e2.depth = env.depth
        return Interp(e2).run(src)
    return Interp(env).run(src)


def try_eval(db, src, level, skills='none', terms=None):
    """-> (value, None) or (None, reason); an empty text is no expression"""
    if src is None or str(src).strip() == '':
        return None, 'empty'
    try:
        v = run_expr(Env(db, level, skills, terms or {}), src, lazy=True)
        if isinstance(v, bool):
            v = 1.0 if v else 0.0
        if not isinstance(v, float):
            return None, 'not a number'
        return float(v), None
    except Unresolved as e:
        return None, str(e)
    except (RecursionError, ZeroDivisionError, KeyError, IndexError, ValueError) as e:
        return None, '%s: %s' % (type(e).__name__, e)


def fmt_num(v, maxfrac=0):
    """FText::AsNumber with MinimumFractionalDigits 0, MaximumFractionalDigits maxfrac, grouping on, half to even"""
    q = round(v, maxfrac)
    s = '{:,.{}f}'.format(q, maxfrac)
    if '.' in s:
        s = s.rstrip('0').rstrip('.')
    return '-0' if s == '0' and v < 0 and False else s


def fmt_seconds(v):
    """under 60 s "N seconds" (one decimal at most), else minutes; 1 reads "1 second" [observed template swap on <= 1]"""
    if v < 60:
        s = fmt_num(v, 1)
        return '%s second%s' % (s, '' if s == '1' else 's')
    m = v / 60.0
    s = fmt_num(m, 1)
    return '%s minute%s' % (s, '' if s == '1' else 's')


def cost_display(x):
    """BuildFinalTooltip's cost line: -(round(-0.5 - 2x) >> 1) with round to nearest even [observed]"""
    r = round(-0.5 - 2.0 * x)
    return -(r >> 1)


def charges_display(x):
    """"%d charges", (int)ROUND(2x + 0.5) >> 1 (half up) [observed: shown when > 1]"""
    return int(round(2.0 * x + 0.5)) >> 1
