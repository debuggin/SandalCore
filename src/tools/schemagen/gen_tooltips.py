#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""gen_tooltips.py: what the game client's ability tooltip says, for every ability of every class.

The client builds an ability's tooltip in `USkillToolTipWidget::BuildFinalTooltip` and
`USkillToolTipWidget::ProcessAbilityDescription`: the ability record's
`AbilityDescription` text is a template, `{...}` and `$...$` are tokens (hit, effect, linger, proj, statmod, formula,
equation, skill, target, charges, tick, duration, maxduration, flavor), and the numbers the header lines state
(cast time, charges, cost, range) come from the record's expressions, evaluated for the PLAYER.  See evidence.md.

This script reads dist/content/content.db (the design database, one gzip-JSON blob per record) and writes
server/worldd/tests/tooltip_key.h (plan E1, stage 3 fix, Part C: the answer key of test_worldd_tooltips, "the tooltip is the
truth"): for each ability of every class, the weapon combos, the general, consumable and profession abilities, what the tooltip
states for a level-LEVEL character with no gear, no skill bought and no effects: the cooldown (the data's; the skill tooltip has no
cooldown line), the charges, each cost with the number the line prints, the cast time where the data has one, the range and its
source, who it may be aimed at, the hits the text states a number for (a PERCENT: the stat mod's term x 100) and the effects the
text names.  With --out it also writes the full JSON (the template, the text, each token's source and value, the whole closure of
hits, effects, projectiles and zones; `text_all_skills` is the text with every skill-conditional part shown).  A number only the
player's stats give (a damage or heal amount) stays the expression plus its terms: the runtime evaluates it with the stat engine.

    python tools/capture/gen_tooltips.py                         # rewrite the header (about 1 s)
    python tools/capture/gen_tooltips.py --check                 # write nothing, exit 1 when the header would change
    python tools/capture/gen_tooltips.py --out tooltips.json --tsv tooltips_summary.tsv   # the research's files too
    python tools/capture/gen_tooltips.py --db <path>/content.db --level 25

It needs only Python and the content.db (the default is the repository's dist/content/content.db); nothing but the header (and the
files --out / --tsv name) is written.  Labels: [observed] read in the client's code or data, [data] the design data, [inferred] a
reading the code allows but this script could not prove.
"""
import argparse
import functools
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
DEFAULT_DB = os.path.join(ROOT, 'dist', 'content', 'content.db')
HEADER = os.path.join(ROOT, 'build', 'gen', 'world', 'tests', 'tooltip_key.h')

if HERE not in sys.path:
    sys.path.insert(0, HERE)

from tooltip_expr import (  # noqa: E402
    BIN, DB, HEALTH_STAT, KW, PUNCT, SCHEMA_TYPE, SELF, TARGET,
    TERM_ADD, TERM_BASE, TERM_MAX, TERM_MIN, TYPE_SCHEMA,
    T_ABILITY, T_CURVE, T_EFFECT, T_EQUATION, T_FORMULA, T_HIT, T_STATMOD, T_TERM,
    Env, Interp, LazyTerm, Parser, Unresolved, _Obj, _num, _trunc,
    charges_display, cost_display, eval_curve, eval_formula,
    fmt_num, fmt_seconds, lex, run_expr, try_eval,
)
from tooltip_render import (  # noqa: E402
    TARGET_BITS, Renderer, TooltipDbContext, closure, effect_facts,
    facts, hit_facts, keymap, num_after, plain, split_template, target_names,
)

CLASSES = ['Bard', 'Cleric', 'Fighter', 'Mage', 'Ranger', 'Rogue', 'Summoner', 'Tank']


def roster(db):
    """-> {group: [(ability name, source note)]}: the 8 classes' tree abilities (+ their Stage tree), the weapons'
    combo and attack abilities, the general abilities and the consumables' abilities; the NPC kits are counted apart"""
    c = db.c
    groups = {k: {} for k in CLASSES}
    for tree, kind, an in c.execute("""select distinct t.name, t.kind, r.ability_name from skill_tree t join skill_node n on n.tree_guid=t.guid
            join skill_rank r on r.skill_guid=n.skill_guid where r.ability_guid<>0 and t.kind in ('class','other') order by 1,3"""):
        cls = tree.split('_')[0]
        if cls in groups:
            groups[cls].setdefault(an, 'class tree' if kind == 'class' else 'Stage tree')
    prof = {}
    for tree, an in c.execute("""select distinct t.name, r.ability_name from skill_tree t join skill_node n on n.tree_guid=t.guid
            join skill_rank r on r.skill_guid=n.skill_guid where r.ability_guid<>0 and t.kind='profession'"""):
        prof.setdefault('Profession', {})[an] = tree
    weapon, general, items, npc = {}, {}, {}, []
    for a in c.execute("select a.name from ab_ability a join item_weapon w on w.attack_ability_guid=a.guid group by a.guid"):
        weapon[a[0]] = 'item_weapon attack ability'
    tsv = os.path.join(ROOT, 'src', 'server', 'services', 'cmd', 'contentc', 'ability_names.tsv')
    cats = {}
    if os.path.exists(tsv):
        for line in open(tsv, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            nm, cat = line.rstrip('\r\n').split('\t')
            cats[nm] = cat
    for nm, cat in cats.items():
        if cat == 'weapon_combo':
            weapon[nm] = 'ability_names.tsv weapon_combo'
        elif cat == 'general':
            general[nm] = 'ability_names.tsv general'
        elif cat == 'item':
            items[nm] = 'ability_names.tsv item'
        elif cat == 'npc':
            npc.append(nm)
    return groups, prof.get('Profession', {}), weapon, general, items, npc


def _build_entry(db, level, name, note):
    ab = db.by_name(T_ABILITY, name)
    if ab is None:
        return {'ability': name, 'source': note, 'error': 'no AoCAbilityRecordBase named so'}
    ab['__kind'] = 'ability'
    desc = ab.get('AbilityDescription', {}).get('Source', '')
    e = {'ability': name, 'guid': ab['__guid'], 'display_name': ab.get('AbilityName', {}).get('Source', ''), 'source': note,
         'template': desc}
    for variant, sk in (('text', 'none'), ('text_all_skills', 'all')):
        r = Renderer(db, level, sk)
        r.frame = None
        txt = r.render(desc, ab, None) if desc else ''
        e[variant] = plain(txt).strip()
        if sk == 'none':
            e['tokens'] = r.tokens
            e['unresolved'] = sorted(set(r.unres))
            e['conditional_parts'] = r.cond
        else:
            e['unresolved_all_skills'] = sorted(set(r.unres))
    if e['text'] == e['text_all_skills']:
        del e['text_all_skills']
    if not e['unresolved_all_skills'] or e['unresolved_all_skills'] == e['unresolved']:
        del e['unresolved_all_skills']
    e.update(facts(db, ab, level, 'none'))
    hits, effects, projs, zones = closure(db, ab, level, 'none')
    e['hits'], e['effects'], e['projectiles'], e['zones'] = hits, effects, projs, zones
    own = [h for h in hits if h['role'].startswith(('ability hit', 'projectile', 'zone'))]
    e['targets']['hits_union'] = target_names(functools.reduce(lambda a, b: a | b, (h['applicable_mask'] for h in own), 0))
    return e


def build(db, level):
    groups, prof, weapon, general, items, npc = roster(db)
    out = {'level': level, 'character': 'level %d, no gear, no skill bought (text) / every skill bought (text_all_skills), no effects' % level,
           'groups': {}}
    for cls in CLASSES:
        out['groups'][cls] = [_build_entry(db, level, n, s) for n, s in sorted(groups[cls].items())]
    out['groups']['Weapons'] = [_build_entry(db, level, n, s) for n, s in sorted(weapon.items())]
    out['groups']['General'] = [_build_entry(db, level, n, s) for n, s in sorted(general.items())]
    out['groups']['Consumables'] = [_build_entry(db, level, n, s) for n, s in sorted(items.items())]
    out['groups']['Professions'] = [_build_entry(db, level, n, s) for n, s in sorted(prof.items())]
    nd = 0
    for n in npc:
        ab = db.by_name(T_ABILITY, n)
        if ab and ab.get('AbilityDescription', {}).get('Source'):
            nd += 1
    out['npc_abilities'] = {'count': len(npc), 'with_description': nd,
                            'note': 'NPC abilities are not player tooltips; counted only'}
    return out


TERM_SLOT = {'BaseValue': TERM_BASE, 'BaseValueMin': TERM_MIN, 'BaseValueMax': TERM_MAX, 'BaseValueAdd': TERM_ADD}


def cstr(s):
    """a C string literal of `s` (UTF-8; the bytes outside ASCII as octal escapes, so the header is plain ASCII)"""
    out = []
    for ch in s or '':
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            continue
        elif ch == '\t':
            out.append('\\t')
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append(''.join('\\%03o' % b for b in ch.encode('utf-8')))
    return '"' + ''.join(out) + '"'


def cnum(v, none=-1.0):
    if v is None or isinstance(v, str):
        return '%r' % float(none)
    return '%r' % float(v)


def _key_hit_term(t, k):
    ex = t.get('value_expr')
    if isinstance(ex, dict):
        term = ex.get(str(TERM_SLOT.get(k, 0)), ex.get(TERM_SLOT.get(k, 0), ''))
    else:
        term = ex or ''
    return ' '.join(str(term).split()).replace(';', ',').replace('|', '/')


def key_hits(e):
    """the hits the text states a number for, as `hit|term|value|closure|term text`"""
    out, seen = [], set()
    own = {h['name'] for h in e['hits']}
    for t in e['tokens']:
        if 'hit' not in t or 'value_kind' not in t or t['value_kind'] == 'stat name':
            continue
        kind = t['value_kind']
        kinds = kind.split('-') if '-' in kind else [kind]
        vals = t.get('value')
        if not isinstance(vals, list):
            vals = [vals] * len(kinds)
        for k, v in zip(kinds, vals):
            key = (t['hit'], k)
            if key in seen:
                continue
            seen.add(key)
            out.append('%s|%s|%s|%d|%s' % (
                t['hit'], 'Value' if k.startswith('Value') else k,
                '?' if v is None else '%r' % float(v),
                1 if t['hit'] in own else 0, _key_hit_term(t, k)))
    return ';'.join(out)


def _effect_duration_str(closure_map, name):
    x = closure_map.get(name.lstrip('!'))
    if x is None or (not x['infinite'] and x['duration_s'] is None):
        return '?'
    if x['infinite']:
        return 'inf'
    return '%r' % float(x['duration_s'])


def key_applies(e):
    """the effects the text names that the ability applies (no skill bought), as `effect=duration;...`"""
    closure_map = {x['name']: x for x in e['effects']}
    own = {h['name'] for h in e['hits']}
    out = []
    for t in e['tokens']:
        names = [t['effect']] if t['kind'] == 'effect' and t['effect'] in closure_map else []
        ref = 'hit' in t and t['hit'] not in own
        for a in t.get('applies', []) or []:
            names.append(('!' if ref else '') + a['effect'])
        for n in names:
            if n and n not in out and n.lstrip('!') not in out:
                out.append(n)
    return ';'.join('%s=%s' % (n, _effect_duration_str(closure_map, n)) for n in out)


def key_costs(db, e, level):
    """`stat=value|shown|enabled;...`: every stat cost"""
    out = []
    for c in e['cost']:
        if 'stat' not in c or c['stat'] == '0':
            continue
        v = c.get('value_none')
        shown = c.get('shown_none')
        en = '1'
        if c.get('enabled_expr'):
            ev, _ = try_eval(db, c['enabled_expr'], level, 'none')
            en = '?' if ev is None else ('1' if ev else '0')
        out.append('%s=%s|%s|%s' % (c['stat'], '?' if v is None else '%r' % float(v), '' if shown is None else str(shown), en))
    return ';'.join(out)


HEADER_BANNER_DOC = (
    ' ============================================================================',
    '  tooltip_key.h',
    '',
    '  GENERATED by tools/capture/gen_tooltips.py; do not edit.',
    '',
    '  What the game client\'s ability tooltip states, for every ability of every class, the weapon combos and the general, consumable and profession',
    '  abilities (plan E1, stage 3 fix, Part C: the tooltip is the truth; test_worldd_tooltips.cpp holds the runtime to it).  The client builds the tooltip',
    '  in USkillToolTipWidget::BuildFinalTooltip from the ability record\'s AbilityDescription',
    '  template and the record\'s expressions evaluated for the PLAYER; this table is that evaluation for a level-%(level)d character with no gear, no skill',
    '  bought and no effects (research: the scratchpad\'s research/tooltips/evidence.md).  One Key per ability:',
    '',
    '    cooldown_s          the data\'s Cooldown (the skill tooltip prints no cooldown line); -1 when the generator could not evaluate it',
    '    charges             CooldownCharges rounded half up (the line is printed when it is over 1); charges_all: with every skill bought',
    '    costs               "stat=value|shown|enabled;..." each stat cost: the value, the number the line prints (about ceil) and whether the cost',
    '                        group is enabled for the player (1/0/?); "?" needs the stat engine',
    '    cast_s              DynamicCastDuration; -1: the cast time is the montage\'s notify time over the cast speed (client-side, not in the data)',
    '    range_cm, range_source   CustomTooltipRange, else AreaTargetRange (a clicked AoE), else the targeting record\'s ValidDistance, else ValidDistanceExp;',
    '                        the line prints floor(cm / 100) meters when over 0',
    '    target_mask         ApplicableTargets (Self 1, Enemy 2, Ally 4, Party 8, Raid 16); target_requirement: Always (a target is needed) / Never',
    '    hits                "hit|term|value|closure|term text;..." every hit the text states a number for: the value is the stat mod\'s term (BaseValue,',
    '                        BaseValueMin/Max/Add, or the whole Value when BaseValue is 0), which the tooltip prints x 100 with a percent sign; closure 0',
    '                        when the text describes another ability\'s hit (a pet\'s)',
    '    applies             "effect=duration;..." the effects the text names that the ability applies (the applyK tokens, and the effect: tokens the data\'s',
    '                        closure reaches; the rest are conditions or descriptions the text links to), with the record\'s duration (inf, ?); "!effect" is',
    '                        applied by a hit the ability itself does not fire (a reaction\'s hit, another ability\'s)',
    '    text                the tooltip\'s description as the client prints it (no skill bought)',
    ' ============================================================================',
)

HEADER_BANNER = '\n'.join('//' + line for line in HEADER_BANNER_DOC) + """
#pragma once

#include <cstdint>

namespace tooltip_key {

constexpr int kLevel = %(level)d;

struct Key {
    const char* group;
    const char* name;
    int64_t     guid;
    double      cooldown_s;
    double      cooldown_all_s;
    const char* shared_tag;
    int         charges;
    int         charges_all;
    const char* costs;
    double      cast_s;
    double      range_cm;
    const char* range_source;
    int         target_mask;
    const char* target_requirement;
    const char* held;
    const char* weapon_requirement;
    const char* hits;
    const char* applies;
    const char* text;
};

const Key kKeys[] = {
"""


def render_header(db, res):
    lines = [HEADER_BANNER % {'level': res['level']}]
    n = 0
    for grp, lst in res['groups'].items():
        for e in lst:
            if 'error' in e:
                continue
            cd = e['cooldown_s']['by_skills']
            ch = e['charges']['by_skills']
            rng = e['range']
            lines.append('    {%s, %s, %dll, %s, %s, %s, %d, %d, %s, %s, %s, %s, %d, %s, %s, %s, %s, %s, %s},\n' % (
                cstr(grp), cstr(e['ability']), e['guid'], cnum(cd['none']), cnum(cd['all']), cstr(e['cooldown_s']['shared_tag']),
                ch['none'] if isinstance(ch['none'], int) else -1, ch['all'] if isinstance(ch['all'], int) else -1, cstr(key_costs(db, e, res['level'])),
                cnum(e['cast']['dynamic_s']), cnum(rng['cm']), cstr(rng['source']), e['targets']['mask'], cstr(e['targets']['requirement']),
                cstr(e['held']['behavior']), cstr(e['weapon_requirement']), cstr(key_hits(e)), cstr(key_applies(e)), cstr(e['text'])))
            n += 1
    lines.append('};\n\nconstexpr int kKeyCount = %d;\n\n}  // namespace tooltip_key\n' % n)
    return ''.join(lines)


def write_if_changed(path, text, check):
    """Write `text` (LF) to `path` with the line ending the file already has; with `check` write nothing. Returns True when nothing changed."""
    old = open(path, 'rb').read() if os.path.exists(path) else b''
    crlf = old.count(b'\r\n') * 2 > old.count(b'\n') > 0
    new = text.replace('\n', '\r\n' if crlf else '\n').encode('utf-8')
    try:
        rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
    except ValueError:
        rel = path
    if new == old:
        print('%s: unchanged (%d bytes)' % (rel, len(new)))
        return True
    if check:
        print('%s: DIFFERS from the regenerated text (%d bytes on disk, %d regenerated)' % (rel, len(old), len(new)))
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(new)
    print('%s: updated (%d bytes)' % (rel, len(new)))
    return False


def _tsv_row(g, e):
    cost = ';'.join('%s %s' % (c.get('shown_none'), c.get('stat')) for c in e['cost'] if 'stat' in c)
    return '\t'.join([
        g, e['ability'], e['display_name'], str(e['cooldown_s']['by_skills']['none']),
        '%s/%s' % (e['charges']['by_skills']['none'], e['charges']['by_skills']['all']), cost,
        str(e['range']['shown_m'] or ''), '+'.join(e['targets']['applicable']),
        e['held']['behavior'], e['text'].replace('\n', ' / '),
    ]) + '\n'


def _write_tsv(path, res):
    rows = ['\t'.join(['group', 'ability', 'display', 'cooldown_s', 'charges(none/all)', 'cost', 'range_m', 'targets', 'holds', 'text']) + '\n']
    for g, lst in res['groups'].items():
        rows.extend(_tsv_row(g, e) for e in lst if 'error' not in e)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--db', default=DEFAULT_DB, help='the content.db (default: the repository\'s dist/content/content.db)')
    ap.add_argument('--header', default=HEADER, help='the test header to write (default: server/worldd/tests/tooltip_key.h)')
    ap.add_argument('--out', help='also write the full JSON table here')
    ap.add_argument('--tsv', help='also write a one-line-per-ability summary here')
    ap.add_argument('--check', action='store_true', help='write nothing; exit 1 when the header (or the --out JSON) would change')
    ap.add_argument('--level', type=int, default=25)
    a = ap.parse_args()
    if not os.path.exists(a.db):
        sys.exit('no content.db at %s (cd services && go run ./cmd/contentc)' % a.db)
    db = DB(a.db)
    res = build(db, a.level)
    ok = write_if_changed(a.header, render_header(db, res), a.check)
    if a.out:
        text = json.dumps(res, indent=1, sort_keys=False, ensure_ascii=False) + '\n'
        ok = write_if_changed(a.out, text, a.check) and ok
    if a.tsv and not a.check:
        _write_tsv(a.tsv, res)
    n = sum(len(v) for v in res['groups'].values())
    print('%d abilities in %d groups' % (n, len(res['groups'])))
    return 0 if ok or not a.check else 1


if __name__ == '__main__':
    sys.exit(main())
