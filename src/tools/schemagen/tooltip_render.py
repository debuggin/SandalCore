#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""tooltip_render.py: template splitter, token renderer, and ability/hit/effect fact extraction."""
from dataclasses import dataclass
import json
import math
import re

from tooltip_expr import (
    Env, LazyTerm, T_EFFECT, T_FORMULA, T_HIT, T_TERM,
    TERM_ADD, TERM_BASE, TERM_MAX, TERM_MIN, Unresolved,
    charges_display, cost_display, fmt_num, fmt_seconds, try_eval,
)


@dataclass
class TooltipDbContext:
    db: object
    level: int = 25
    skills: str = 'none'


def _step_template(text, i, depth, lit0, tok0):
    c = text[i]
    if c == '$':
        if depth == 0:
            return text[lit0:i], 1, lit0, i + 1
        depth -= 1
        return (text[tok0:i], 0, i + 1, tok0) if depth == 0 else (None, depth, lit0, tok0)
    if c == '{':
        seg = text[lit0:i] if depth < 1 else None
        return seg, depth + 1, lit0, (i + 1 if depth < 1 else tok0)
    if c == '}' and depth > 0:
        depth -= 1
        return (text[tok0:i], 0, i + 1, tok0) if depth == 0 else (None, depth, lit0, tok0)
    return None, depth, lit0, tok0


def split_template(text):
    """ProcessAbilityDescription's splitter: '$' toggles, '{' and '}' nest; alternates literal, token, literal."""
    segs, depth, lit0, tok0 = [], 0, 0, 0
    for i in range(len(text)):
        seg, depth, lit0, tok0 = _step_template(text, i, depth, lit0, tok0)
        if seg is not None:
            segs.append(seg)
    segs.append(text[lit0:len(text)])
    return segs


def num_after(s, key):
    """FUN_14696e3b0: the digits right after `key` in s, -1 if none"""
    i = s.lower().find(key.lower())
    if i < 0:
        return -1
    j = i + len(key)
    k = j
    while k < len(s) and s[k].isdigit():
        k += 1
    return int(s[j:k]) if k > j else -1


def keymap(lst):
    return {x['Key']: x['Value'] for x in lst}


def _format_statmod_vals(vals, f):
    pct = '%' in f
    digits = 2 if 'ff' in f else (1 if 'f' in f else 0)
    out = []
    for x in vals:
        if '-1' in f:
            x -= 1.0
        if 'abs' in f:
            x = abs(x)
        if pct:
            x *= 100.0
        out.append(fmt_num(x, digits) + ('%' if pct else ''))
    return '-'.join(out)


def _select_statmod_slot(f):
    if 'max' in f:
        return TERM_MAX, 'BaseValueMax'
    if 'min' in f:
        return TERM_MIN, 'BaseValueMin'
    if 'add' in f:
        return TERM_ADD, 'BaseValueAdd'
    return TERM_BASE, 'BaseValue'


def _parse_ref_token_parts(head, rest, kind):
    h_main, _, h_suf = head.partition('.')
    idx_s = h_main[len(kind):]
    idx = int(idx_s) if idx_s.isdigit() else None
    sub = rest
    if idx is None and not rest and '.' in head:
        sub = h_suf
    elif idx is not None and not rest:
        sub = h_suf
    elif idx is not None and rest:
        sub = rest if not h_suf else h_suf + rest
    name = None
    if idx is None and rest:
        m2 = re.match(r'([^:.]+)[:.]?(.*)$', rest, re.S)
        name, sub = m2.group(1), m2.group(2)
    return idx, name, sub


class Renderer:
    def __init__(self, db, level=25, skills='none'):
        self.db, self.level, self.skills = db, level, skills
        self.tokens = []
        self.cond = []
        self.unres = []

    def env(self, terms=None):
        return Env(self.db, self.level, self.skills, terms or {})

    def ev(self, src, terms=None):
        return try_eval(self.db, src, self.level, self.skills, terms)

    def term_values(self, sm):
        out = {}
        for t in sm.get('ValueInputTerms', []):
            out[t['TermId']['Guid']] = LazyTerm(self.env(), t['Value'])
        return out

    def _eval_term_slot(self, terms, g):
        if g not in terms:
            return 0.0, None
        v, why = self.ev(terms[g])
        return (None, why) if v is None else (v, None)

    def _eval_statmod_terms(self, sm, f):
        terms = {t['TermId']['Guid']: t['Value'] for t in sm.get('ValueInputTerms', [])}
        if 'minmax' in f:
            lo, w1 = self._eval_term_slot(terms, TERM_MIN)
            hi, w2 = self._eval_term_slot(terms, TERM_MAX)
            return [lo, hi], (w1 or w2), 'BaseValueMin-BaseValueMax', terms
        slot, kind = _select_statmod_slot(f)
        v, why = self._eval_term_slot(terms, slot)
        if v == 0.0 and slot == TERM_BASE:
            kind = 'Value (the whole stat mod expression, evaluated for the player)'
            v, why = self.ev(sm['Value'], self.term_values(sm))
        return [v], why, kind, terms

    def statmod_value(self, sm, flags, tok):
        f = flags.lower()
        stat = self.db.stat_display(sm['StatRefId']['Guid'])
        if 'onlystat' in f:
            tok['value_kind'] = 'stat name'
            return stat
        vals, why, kind, terms = self._eval_statmod_terms(sm, f)
        tok['value_kind'] = kind
        slot_keys = (TERM_BASE, TERM_MIN, TERM_MAX, TERM_ADD)
        tok['value_expr'] = sm['Value'] if kind.startswith('Value') else {k: terms[k] for k in terms if k in slot_keys}
        if any(x is None for x in vals):
            tok['needs_stats'] = why
            self.unres.append(tok['raw'] + ': ' + str(why))
            return None
        tok['value'] = vals if len(vals) > 1 else vals[0]
        txt = _format_statmod_vals(vals, f)
        if 'nostat' in f:
            return txt
        if 'by' in f.replace('nostat', ''):
            neg = txt.startswith('-')
            return '%s %s by %s' % (stat, 'decreased' if neg else 'increased', txt.lstrip('-'))
        return '%s %s' % (txt, stat)

    def hit_kind(self, h):
        tags = [t['TagName'] for t in h.get('EventTags', [])]
        for t in tags:
            if t.startswith('Element.Healing'):
                return 'healing', tags
        for t in tags:
            if t.startswith('Element.Physical'):
                return 'physical', tags
            if t.startswith('Element.Magical'):
                return 'magical', tags
        return '', tags

    def _hit_apply_text(self, h, s, ab, tok):
        low = s.lower()
        selfmode = 'applyself' in low
        key = 'applyself' if selfmode else 'apply'
        n = num_after(s, key)
        lst = h.get('EffectsAppliedToSelf' if selfmode else 'ApplyEffects', [])
        if not (0 <= n < len(lst)):
            return ''
        ap = lst[n]
        eff = self.db.ref(ap.get('EffectId') or ap.get('Effect'))
        name = self.effect_name(eff)
        tok.setdefault('applies', []).append({'effect': eff['__name'], 'name': name})
        out = ('%s: %s' % (name, self.effect_desc(eff, ab))) if 'inline' in low else name
        if 'hide' in low:
            return ''
        if 'dur' in low or 'for' in low:
            d, _ = self.ev(ap.get('EffectDuration', ''))
            if not d or d <= 0:
                d, _ = self.ev(eff.get('EffectDuration', ''))
            if d and not eff.get('bInfiniteDuration'):
                tok.setdefault('durations', []).append(d)
                out = (out + ' for ' + fmt_seconds(d)) if 'for' in low else fmt_seconds(d)
        st, _ = self.ev(ap.get('StackCount', ''))
        if st and st >= 1.5:
            out = '%d stacks of %s' % (int(st), out)
        return out

    def _hit_default_text(self, h, s, tok):
        sms = h.get('StatModsIds', [])
        if not sms:
            return ''
        sm = self.db.ref(sms[0])
        txt = self.statmod_value(sm, s + '%nostat', tok)
        kind, tags = self.hit_kind(h)
        tok['damage_kind'] = kind or 'other'
        tok['event_tags'] = tags
        leaf = tags[0].split('.')[-1] if tags and kind and tags[0].count('.') > 1 else ''
        label = leaf or {'healing': 'Healing', 'physical': 'Physical', 'magical': 'Magical'}.get(kind, '')
        tok['label'] = label
        if txt is not None:
            return txt + (' ' + label if label else '')
        terms_map = {t['TermId']['Guid']: t['Value'] for t in sm.get('ValueInputTerms', [])}
        coef, _ = self.ev(terms_map.get(TERM_BASE, ''))
        lab = {'healing': 'healing', 'physical': 'physical damage', 'magical': 'magical damage'}.get(kind, 'effect')
        if coef is not None:
            tok['coefficient'] = coef
            return '[%sx %s%s]' % (fmt_num(coef, 3), (leaf + ' ') if leaf else '', lab)
        return '[%s%s]' % ((leaf + ' ') if leaf else '', lab)

    def hit_text(self, h, suffix, ab, tok):
        s = suffix or ''
        low = s.lower()
        tok['hit'] = h['__name']
        if low.startswith('statmod'):
            n = num_after(s, 'statmod')
            sms = h.get('StatModsIds', [])
            if 0 <= n < len(sms):
                sm = self.db.ref(sms[n])
                flags = s[len('statmod') + len(str(n)):]
                txt = self.statmod_value(sm, flags, tok)
                return txt if txt is not None else '?'
            return '<ParseFail>hit statmod index error</>'
        if low.startswith(('apply', 'hideapply', 'applyself')):
            return self._hit_apply_text(h, s, ab, tok)
        return self._hit_default_text(h, s, tok)

    def effect_name(self, eff):
        return eff.get('EffectName', {}).get('Source') or eff['__name']

    def effect_desc(self, eff, ab):
        src = eff.get('EffectDescription', {}).get('Source', '')
        r = Renderer(self.db, self.level, self.skills)
        r.frame = eff
        r.tokens = self.tokens
        r.unres = self.unres
        r.cond = self.cond
        return r.render(src, ab, eff)

    def _token_skill(self, t, tok, ab, frame):
        neg = t.lower().startswith('!')
        parts = t.split(':', 3)
        if len(parts) < 4:
            return t
        _, cls, skill, body = parts
        has = (self.skills == 'all')
        tok.update({'kind': 'skill', 'skill': skill, 'class': cls, 'negated': neg, 'shown': has != neg})
        self.cond.append({'skill': skill, 'negated': neg, 'shown': has != neg})
        self.tokens.append(tok)
        return self.render(body, ab, frame) if has != neg else ''

    def _token_formula(self, raw, tok, ab, frame):
        neg = raw.lower().startswith('!')
        m = re.match(r'!?formula:([^:.]+)(?:\.([^:]*))?(?::(.*))?$', raw, re.S)
        if not m:
            return raw
        name, suf, body = m.group(1), m.group(2) or '', m.group(3)
        f = self.db.by_name(T_FORMULA, name)
        v, why = self.ev('EvalFormula($#4547065559311335368:%d$)' % f['__guid']) if f else (None, 'no formula ' + name)
        tok.update({'kind': 'formula', 'formula': name, 'value': v, 'negated': neg})
        self.tokens.append(tok)
        if body is None:
            if v is None:
                tok['needs_stats'] = why
                self.unres.append(raw + ': ' + str(why))
                return '?'
            pct = '%' in suf
            return fmt_num(v * 100 if pct else v, 1 if 'F' in suf else 0) + ('%' if pct else '')
        if v is None:
            tok['needs_stats'] = why
            tok['condition'] = 'unresolved'
            self.unres.append(raw[:60] + ': ' + str(why))
            return '[if %s: %s]' % (name, self.render(body, ab, frame))
        shown = (v != 0.0) != neg
        tok['shown'] = shown
        return self.render(body, ab, frame) if shown else ''

    def _token_charges(self, raw, tok, ab):
        v, why = self.ev(ab.get('CooldownCharges', ''))
        if v is None and why == 'empty':
            v = 1.0
        tok.update({'kind': 'charges', 'value': v})
        self.tokens.append(tok)
        if v is None:
            self.unres.append(raw + ': ' + str(why))
            return '?'
        return '%d charges' % charges_display(v)

    def token(self, raw, ab, frame):
        tok = {'raw': raw}
        head, _, rest = raw.partition(':')
        low = head.lower()
        if low.startswith(('!skill', 'skill')):
            return self._token_skill(raw, tok, ab, frame)
        if low.startswith(('formula', '!formula')):
            return self._token_formula(raw, tok, ab, frame)
        if low == 'charges' and not rest:
            return self._token_charges(raw, tok, ab)
        if low in ('duration', 'maxduration', 'tick') and frame is not None and not rest:
            return self.frame_value(low, frame, tok)
        if low.startswith('flavor'):
            tok['kind'] = 'flavor'
            self.tokens.append(tok)
            return '<Flavor>%s</>' % rest
        ref_kinds = ('hit', 'effect', 'linger', 'proj', 'statmod', 'equation', 'target', 'tick', 'apply')
        kind = next((k for k in ref_kinds if low.startswith(k)), None)
        if kind is None:
            tok['kind'] = 'literal'
            return raw
        tok['kind'] = kind
        self.tokens.append(tok)
        idx, name, sub = _parse_ref_token_parts(head, rest, kind)
        try:
            return self._resolve_token(kind, (idx, name, sub), tok, (ab, frame))
        except Unresolved as e:
            tok['needs_stats'] = str(e)
            self.unres.append(raw + ': ' + str(e))
            return '?'

    def frame_value(self, what, eff, tok):
        tok['kind'] = what
        self.tokens.append(tok)
        if what == 'tick':
            v = float(eff.get('TickTimer', 0) or 0)
        elif what == 'maxduration':
            v, _ = self.ev(str(eff.get('ExtendedMaximum', '')))
            v = v or 0.0
        else:
            v, why = self.ev(eff.get('EffectDuration', ''))
            if v is None:
                self.unres.append('duration of %s: %s' % (eff['__name'], why))
                tok['needs_stats'] = why
                return '?'
        tok['value'] = v
        return fmt_seconds(v)

    def _resolve_hit(self, spec, tok, ab, frame):
        idx, name, sub = spec
        h = None
        if idx is not None:
            m = keymap(ab.get('HitsIds', [])) if frame is None or frame.get('__kind') == 'ability' else {}
            ref = m.get(idx)
            h = self.db.ref(ref) if ref else None
        elif name:
            h = self.db.by_name(T_HIT, name)
        if h is None:
            tok['unresolved'] = 'no hit'
            self.unres.append(tok['raw'] + ': no such hit')
            return '?'
        return self.hit_text(h, sub, ab, tok)

    def _resolve_effect(self, spec, tok, ab):
        idx, name, sub = spec
        eff = None
        if idx is not None:
            ref = keymap(ab.get('EffectsIds', [])).get(idx)
            eff = self.db.ref(ref) if ref else None
        elif name:
            eff = self.db.by_name(T_EFFECT, name)
        if eff is None:
            tok['unresolved'] = 'no effect'
            self.unres.append(tok['raw'] + ': no such effect')
            return '?'
        tok['effect'] = eff['__name']
        low = sub.lower()
        nm = self.effect_name(eff)
        if 'description' in low:
            return self.effect_desc(eff, ab)
        if 'inline' in low:
            return '%s: %s' % (nm, self.effect_desc(eff, ab))
        return nm

    def _resolve_linger(self, z, sub, tok, ab):
        s2 = sub.lower()
        if s2.startswith('duration'):
            tok['value'] = z['LifeTime']
            return fmt_seconds(float(z['LifeTime']))
        if s2.startswith('tick'):
            tok['value'] = z['TickRate']
            return fmt_seconds(float(z['TickRate']))
        m = re.match(r'(init|linger|beginoverlap|endoverlap|endhit)(\d+)(?:\.(.*))?', sub, re.I)
        if m:
            key = {'init': 'InitialHitsIds', 'linger': 'LingeringHitsIds', 'beginoverlap': 'OnBeginOverlapHitsIds',
                   'endoverlap': 'OnEndOverlapHitsIds', 'endhit': 'EndHitsIds'}[m.group(1).lower()]
            lst = z.get(key, [])
            n = int(m.group(2))
            if n < len(lst):
                return self.hit_text(self.db.ref(lst[n]), m.group(3) or '', ab, tok)
        return ''

    def _resolve_linger_or_proj(self, kind, spec, tok, ab):
        idx, name, sub = spec
        if kind == 'linger':
            ref = keymap(ab.get('LingeringEffectsIds', [])).get(idx) if idx is not None else None
            z = self.db.ref(ref) if ref else (self.db.by_name('LingeringEffectRecordBase', name) if name else None)
        else:
            ref = keymap(ab.get('ProjectilesIds', [])).get(idx) if idx is not None else None
            z = self.db.ref(ref) if ref else None
        if z is None:
            tok['unresolved'] = 'no ' + kind
            self.unres.append(tok['raw'] + ': no such ' + kind)
            return '?'
        tok[kind] = z['__name']
        if kind == 'linger':
            return self._resolve_linger(z, sub, tok, ab)
        m = re.match(r'hit(\d+)\.?(.*)$', sub, re.S)
        lst = z.get('HitsIds', [])
        if m and int(m.group(1)) < len(lst):
            return self.hit_text(self.db.ref(lst[int(m.group(1))]), m.group(2), ab, tok)
        tok['unresolved'] = 'no projectile hit'
        self.unres.append(tok['raw'] + ': no such projectile hit')
        return '?'

    def _resolve_frame_member(self, kind, spec, tok, ctx):
        idx, _, sub = spec
        ab, frame = ctx
        n = idx if idx is not None else 0
        if kind == 'statmod':
            sms = frame.get('StatModsIds', [])
            if n < len(sms):
                txt = self.statmod_value(self.db.ref(sms[n]), sub or '', tok)
                return txt if txt is not None else '?'
            return ''
        key = {'tick': 'TickHitsIds', 'apply': 'ApplyHitsIds'}[kind]
        lst = frame.get(key, [])
        if n < len(lst):
            return self.hit_text(self.db.ref(lst[n]), sub, ab, tok)
        return ''

    def _resolve_token(self, kind, spec, tok, ctx):
        ab, frame = ctx
        spec = (spec[0], spec[1], spec[2] or '')
        if kind == 'hit':
            return self._resolve_hit(spec, tok, ab, frame)
        if kind == 'effect':
            return self._resolve_effect(spec, tok, ab)
        if kind in ('linger', 'proj'):
            return self._resolve_linger_or_proj(kind, spec, tok, ab)
        if kind in ('statmod', 'tick', 'apply') and frame is not None:
            return self._resolve_frame_member(kind, spec, tok, ctx)
        if kind in ('target', 'equation'):
            tok['needs_stats'] = 'target radius' if kind == 'target' else 'equation'
            return '?'
        return '?'

    def resolve(self, kind, idx, name, *rest):
        sub, tok, ab, frame = (rest + ('', {}, {}, None))[:4]
        return self._resolve_token(kind, (idx, name, sub), tok, (ab, frame))

    def render(self, text, ab, frame=None):
        segs = split_template(text)
        return ''.join(s if i % 2 == 0 else self.token(s, ab, frame) for i, s in enumerate(segs))


def plain(s):
    """the visible words of a rich text: tags dropped, <img id=X/> as [X]"""
    s = re.sub(r'<img id="([^"]+)"\s*/>', r'[\1]', s)
    s = re.sub(r'<a [^>]*>', '', s)
    s = re.sub(r'</>', '', s)
    s = re.sub(r'<(?:Highlight|Bold|Flavor|Physical|Magical|Healing|NodeBuffStat|TooltipRegular|Red)>', '', s)
    return s.replace('\r\n', '\n')


TARGET_BITS = [(1, 'Self'), (2, 'Enemy'), (4, 'Ally'), (8, 'Party'), (16, 'Raid')]


def target_names(mask):
    return [n for b, n in TARGET_BITS if mask & b]


def _hit_statmod_facts(db, sm, f, level, skills):
    f['stat'] = db.stat_name(sm['StatRefId']['Guid'])
    f['stat_mod_value'] = sm['Value']
    terms = {}
    for t in sm.get('ValueInputTerms', []):
        g = t['TermId']['Guid']
        rec = db.rec(T_TERM, g)
        nm = rec['__name'] if rec else str(g)
        v, why = try_eval(db, t['Value'], level, skills)
        terms[nm] = {'expr': t['Value'], 'value': v, 'unresolved': why if v is None else None}
    f['terms'] = terms
    shown = {}
    for nm, key in (('BaseValue', 'percent'), ('BaseValueMin', 'min_percent'), ('BaseValueMax', 'max_percent')):
        t = terms.get(nm)
        if t and t['value'] is not None:
            shown[key] = round(t['value'] * 100.0, 3)
    if 'BaseValueAdd' in terms and terms['BaseValueAdd']['value'] is not None:
        shown['add_flat'] = terms['BaseValueAdd']['value']
    f['tooltip_shows'] = shown
    f['absolute_amount'] = 'needs the stat engine (the stat mod formula over the caster stats)'


def hit_facts(db, h, level, skills):
    sm = db.ref(h['StatModsIds'][0]) if h.get('StatModsIds') else None
    f = {'name': h['__name'], 'applicable_targets': target_names(h.get('ApplicableTargets', 0)),
         'applicable_mask': h.get('ApplicableTargets', 0),
         'fallback': h.get('DefaultFallbackTargetType', '').split('::')[-1],
         'net_type': h.get('NetworkType', '').split('::')[-1],
         'event_tags': [t['TagName'] for t in h.get('EventTags', [])]}
    tags = f['event_tags']
    f['kind'] = ('healing' if any(t.startswith('Element.Healing') for t in tags) else
                 'physical damage' if any(t.startswith('Element.Physical') for t in tags) else
                 'magical damage' if any(t.startswith('Element.Magical') for t in tags) else
                 '' if sm is None else 'stat change')
    if sm is not None:
        _hit_statmod_facts(db, sm, f, level, skills)
    f['applies'] = []
    for ap in h.get('ApplyEffects', []):
        e = db.ref(ap['EffectId'])
        if e:
            d, _ = try_eval(db, ap.get('EffectDuration', ''), level, skills)
            ed, _ = try_eval(db, e.get('EffectDuration', ''), level, skills)
            dur = d if d else ('infinite' if e.get('bInfiniteDuration') else ed)
            f['applies'].append({'effect': e['__name'], 'duration_s': dur,
                                 'stacks': try_eval(db, ap.get('StackCount', ''), level, skills)[0]})
    f['applies_to_self'] = [
        db.ref(x.get('Effect'))['__name'] for x in h.get('EffectsAppliedToSelf', []) if db.ref(x.get('Effect'))
    ]
    return f


def effect_facts(db, e, level, skills):
    d, _ = try_eval(db, e.get('EffectDuration', ''), level, skills)
    return {'name': e['__name'], 'display_name': e.get('EffectName', {}).get('Source', ''),
            'infinite': bool(e.get('bInfiniteDuration')), 'duration_s': d, 'duration_expr': e.get('EffectDuration', ''),
            'tick_s': e.get('TickTimer', 0), 'max_duration_s': try_eval(db, str(e.get('ExtendedMaximum', '')), level, skills)[0],
            'stack_limit': e.get('InPlaceStackLimit'), 'dispellable': bool(e.get('bDispellable')),
            'description': e.get('EffectDescription', {}).get('Source', '')}


def _eval_stat_cost(db, sc, g, level):
    row = {'stat': db.stat_name(sc['Stat']['Guid']), 'expr': sc['Value'], 'enabled_expr': g.get('Enabled', ''),
           'charge_cost_expr': g.get('ChargeCost', ''), 'triggers_cooldown': bool(g.get('bTriggersCooldown'))}
    for sk in ('none', 'all'):
        v, why = try_eval(db, sc['Value'], level, sk)
        if v is None and why == 'empty':
            v = 0.0
        row['value_' + sk] = v
        if v is not None:
            row['shown_' + sk] = cost_display(v) if cost_display(v) > 0 else None
        else:
            row['unresolved'] = why
    return row


def _facts_costs(db, ab, level):
    costs = []
    for g in ab.get('Costs', []):
        for sc in g.get('StatCosts', []):
            costs.append(_eval_stat_cost(db, sc, g, level))
        for ec in g.get('EffectCosts', []):
            costs.append({'effect_cost': ec.get('EffectFilter', ec), 'enabled_expr': g.get('Enabled', '')})
    return costs


def _facts_range(db, ab, level):
    if ab.get('bOverrideRangeInTooltip'):
        r, why = try_eval(db, ab.get('CustomTooltipRange', ''), level, 'none')
        src = 'CustomTooltipRange'
    elif ab.get('bIsAOEClickedAbility'):
        r, why, src = float(ab.get('AreaTargetRange', 0)), None, 'AreaTargetRange'
    elif ab.get('bUseTargetingRecord'):
        t = db.ref(ab.get('TargetingRecordId') or {})
        r = float(t['LockOnInfo']['ValidDistance']) if t else None
        why, src = None, 'targeting record ValidDistance'
    elif ab.get('bCheckTargetValidity') and ab.get('bPerformConeCheck'):
        r, why = try_eval(db, ab.get('ValidDistanceExp', ''), level, 'none')
        src = 'ValidDistanceExp'
    else:
        r, why, src = 0.0, None, 'none'
    return {'source': src, 'cm': r, 'shown_m': (math.floor(r * 0.01) if r and r * 0.01 > 1e-8 else None),
            'unresolved': why if r is None else None}


def facts(db, ab, level, skills):
    f = {}
    cd = {}
    for sk in ('none', 'all'):
        v, why = try_eval(db, ab.get('Cooldown', ''), level, sk)
        cd[sk] = v if v is not None else (0.0 if why == 'empty' else 'unresolved: %s' % why)
    f['cooldown_s'] = {'expr': ab.get('Cooldown', ''), 'by_skills': cd, 'in_tooltip': False,
                       'shared_tag': ab.get('SharedCooldownTag', ''),
                       'disable_during_cooldown': bool(ab.get('bDisableDuringCooldown'))}
    ch = {}
    for sk in ('none', 'all'):
        v, why = try_eval(db, ab.get('CooldownCharges', ''), level, sk)
        ch[sk] = (charges_display(v) if v is not None else (1 if why == 'empty' else 'unresolved: %s' % why))
    f['charges'] = {'expr': ab.get('CooldownCharges', ''), 'by_skills': ch, 'shown_when_over_1': True}
    f['cost'] = _facts_costs(db, ab, level)
    dc = ab.get('DynamicCastDuration', '')
    v, _ = try_eval(db, dc, level, 'none')
    cast_note = (
        'the cast time line is the montage notify time over the cast speed '
        '(UUILibrary::SetAbilityCastTime, 0x1469bf..): not in the design data'
        if not dc else 'DynamicCastDuration'
    )
    f['cast'] = {'dynamic_expr': dc, 'dynamic_s': v,
                 'montage': ((ab.get('AnimMontages') or [{}])[0].get('AbilityAnimationSoftPaths') or [''])[0],
                 'note': cast_note}
    f['held'] = {'behavior': ab.get('HeldAbilityBehavior', '').split('::')[-1],
                 'max_hold_s': ab.get('MaximumHoldTime', 0),
                 'bard_music': 'bard_music' in json.dumps(ab.get('AbilityTags', '')).lower()}
    f['range'] = _facts_range(db, ab, level)
    mask = ab.get('ApplicableTargets', 0)
    f['targets'] = {'applicable': target_names(mask), 'mask': mask,
                    'requirement': ab.get('TargetActivationRequirement', '').split('::')[-1],
                    'fallback': ab.get('DefaultFallbackTargetType', '').split('::')[-1]}
    f['weapon_requirement'] = ab.get('WeaponRequirement', '').split('::')[-1]
    return f


_EFFECT_HIT_KEYS = (
    ('ApplyHitsIds', 'effect apply'), ('TickHitsIds', 'effect tick'), ('ExpireHitsIds', 'effect expire'),
    ('EndHitsIds', 'effect end'), ('RemoveHitsIds', 'effect remove'), ('PreApplyHitsIds', 'effect preapply'),
    ('ConsumeHitsIds', 'effect consume'),
)


def _closure_add_hit(ctx, ref, role):
    db = ctx['db']
    h = db.ref(ref)
    if h is None or (h['__guid'], role) in ctx['seen']:
        return
    ctx['seen'].add((h['__guid'], role))
    hf = hit_facts(db, h, ctx['level'], ctx['skills'])
    hf['role'] = role
    ctx['hits'].append(hf)
    for ap in h.get('ApplyEffects', []):
        _closure_add_effect(ctx, ap['EffectId'])
    for x in h.get('EffectsAppliedToSelf', []):
        _closure_add_effect(ctx, x.get('Effect'))


def _closure_add_effect(ctx, ref):
    db = ctx['db']
    e = db.ref(ref)
    if e is None or e['__guid'] in ctx['ek']:
        return
    ctx['ek'][e['__guid']] = 1
    ctx['effects'].append(effect_facts(db, e, ctx['level'], ctx['skills']))
    for key, role in _EFFECT_HIT_KEYS:
        for h in e.get(key, []):
            _closure_add_hit(ctx, h, role + ' of ' + e['__name'])


def closure(db, ab, level, skills):
    """the hits, effects, zones and projectiles the ability reaches, with who each hit applies to"""
    ctx = {'db': db, 'level': level, 'skills': skills, 'seen': set(), 'ek': {}, 'hits': [], 'effects': []}
    projs, zones = [], []
    for x in ab.get('HitsIds', []):
        _closure_add_hit(ctx, x['Value'], 'ability hit %d' % x['Key'])
    for x in ab.get('EffectsIds', []):
        _closure_add_effect(ctx, x['Value'])
    for x in ab.get('ProjectilesIds', []):
        p = db.ref(x['Value'])
        if p is None:
            continue
        projs.append({'key': x['Key'], 'name': p['__name'], 'type': p['ProjectileType'].split('::')[-1],
                      'speed': p['Speed'], 'range': p['Range'], 'life_s': p['LifeTime'],
                      'max_hits': p['MaxProjectileHits']})
        for h in p.get('HitsIds', []):
            _closure_add_hit(ctx, h, 'projectile %d' % x['Key'])
    for x in ab.get('LingeringEffectsIds', []):
        z = db.ref(x['Value'])
        if z is None:
            continue
        zones.append({'key': x['Key'], 'name': z['__name'], 'life_s': z['LifeTime'], 'tick_s': z['TickRate'],
                      'initial_hits': len(z.get('InitialHitsIds', [])),
                      'lingering_hits': len(z.get('LingeringHitsIds', []))})
        for key in ('InitialHitsIds', 'LingeringHitsIds', 'OnBeginOverlapHitsIds', 'OnEndOverlapHitsIds', 'EndHitsIds'):
            for h in z.get(key, []):
                _closure_add_hit(ctx, h, 'zone %d %s' % (x['Key'], key[:-3]))
    return ctx['hits'], ctx['effects'], projs, zones
