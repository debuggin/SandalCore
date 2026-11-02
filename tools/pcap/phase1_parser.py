#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Phase 1: Complete UE5/AoC Packet & Bunch Parser."""

import argparse
from collections import Counter, defaultdict
import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from phase1_bitstream import (  # noqa: E402,F401
    CUSTOM_FIELD_BITS,
    GUIDCache,
    MAX_CHSEQ,
    MAX_PKT_BITS,
    OUTER_HDR_BITS,
    decode_content_block,
    decode_guid_exports,
    decode_new_actor,
    decode_rep_layout_exports,
    extract_realigned,
    find_content_end,
    internal_load_object,
    parse_bunch_header,
    parse_packet,
    read_bit,
    read_bits_le,
    read_double_vector,
    read_fstring,
    read_packed_vector,
    read_rotation,
    read_uint8,
    read_uint16,
    read_uint32,
    serialize_int,
    serialize_int_packed,
    serialize_int_packed64,
    static_parse_name,
)


def _decode_bunch_exports(inner_data, pos, end_pos, guid_cache, result):
    export_start = pos
    b_rep_layout = read_bit(inner_data, pos)
    pos += 1
    export_had_error = False
    if b_rep_layout:
        rle, pos = decode_rep_layout_exports(inner_data, pos, end_pos)
        result['rep_layout'] = rle
        export_had_error = 'error' in rle
    else:
        ge, pos = decode_guid_exports(inner_data, pos, end_pos, guid_cache)
        result['guid_exports'] = ge
        export_had_error = 'error' in ge
    result['export_bits'] = pos - export_start
    return export_had_error, pos


def _decode_must_map(inner_data, pos, end_pos, result):
    if pos + 16 > end_pos:
        return pos
    count, pos = read_uint16(inner_data, pos)
    guids = []
    for _ in range(min(count, 200)):
        if pos + 8 > end_pos:
            break
        g, pos = serialize_int_packed64(inner_data, pos)
        guids.append(g)
    result['must_map'] = {'count': count, 'guids': guids}
    return pos


def _try_bunch_fallback(inner_data, start_pos, end_pos, bunch, ctx):
    direction, guid_cache, result, flag_key = ctx
    pos2 = start_pos
    if bunch['open'] and bunch['ch'] > 0 and pos2 < end_pos:
        _, pos2 = decode_new_actor(inner_data, pos2, end_pos, guid_cache)
    blocks2, errors2, pos2 = _parse_content_blocks(inner_data, pos2, end_pos, direction, guid_cache)
    remaining2 = max(0, end_pos - pos2)
    cur_err = result['block_errors']
    if (errors2 < cur_err) or (errors2 == cur_err and remaining2 < result['bits_remaining']):
        result['blocks'] = blocks2
        result['block_errors'] = errors2
        result['bits_consumed'] = pos2 - bunch['data_start']
        result['bits_remaining'] = remaining2
        result[flag_key] = True


def decode_bunch_data(inner_data, bunch, direction, guid_cache):
    """Decode complete bunch data: exports -> must_map -> new_actor -> content_blocks."""
    pos = bunch['data_start']
    end_pos = pos + bunch['bunch_data_bits']
    result = {}
    export_had_error = False
    if bunch['has_exports']:
        export_had_error, pos = _decode_bunch_exports(inner_data, pos, end_pos, guid_cache, result)
    pos_before_exports = bunch['data_start']
    pos_after_exports = pos
    if bunch['has_must_map']:
        pos = _decode_must_map(inner_data, pos, end_pos, result)
    if bunch['open'] and bunch['ch'] > 0 and pos < end_pos:
        new_actor, pos = decode_new_actor(inner_data, pos, end_pos, guid_cache)
        result['new_actor'] = new_actor
    blocks, block_errors, pos = _parse_content_blocks(inner_data, pos, end_pos, direction, guid_cache)
    result['blocks'] = blocks
    result['block_errors'] = block_errors
    result['bits_consumed'] = pos - bunch['data_start']
    result['bits_total'] = bunch['bunch_data_bits']
    result['bits_remaining'] = max(0, end_pos - pos)
    if bunch['has_must_map'] and (block_errors > 0 or result['bits_remaining'] > 2):
        _try_bunch_fallback(
            inner_data, pos_after_exports, end_pos, bunch,
            (direction, guid_cache, result, 'must_map_skipped'),
        )
    if export_had_error and (result['block_errors'] > 0 or result['bits_remaining'] > 2):
        _try_bunch_fallback(
            inner_data, pos_before_exports, end_pos, bunch,
            (direction, guid_cache, result, 'exports_skipped'),
        )
    return result


def _parse_content_blocks(inner_data, pos, end_pos, direction, guid_cache):
    """Parse content blocks loop. Returns (blocks, block_errors, final_pos)."""
    blocks = []
    block_errors = 0
    max_blocks = 200
    while pos < end_pos and len(blocks) < max_blocks:
        if (end_pos - pos) < 3:
            break
        block, new_pos = decode_content_block(inner_data, pos, end_pos, direction, guid_cache)
        if block is None:
            break
        if 'error' in block and block.get('error', '').startswith('bad_payload_'):
            blocks.append({
                'header': block['header'],
                'payload_bits': (end_pos - pos) - 2,
                '_payload_fallback': True,
            })
            pos = end_pos
            break
        blocks.append(block)
        if 'error' in block:
            block_errors += 1
            break
        pos = new_pos
    return blocks, block_errors, pos


def _parse_packet_line(line_no, line):
    line = line.strip()
    if not line:
        return None
    obj = json.loads(line)
    hex_str = obj.get('hex')
    if not hex_str or len(hex_str) < 20:
        return None
    raw = bytes.fromhex(hex_str)
    if len(raw) < 10 or ((raw[4] >> 5) & 1):
        return None
    return {
        'raw': raw,
        'dir': obj.get('dir', '?'),
        'ts': obj.get('ts', ''),
        'size': len(raw),
        'line': line_no,
    }


def load_packets(capture_file):
    """Load data packets from JSONL capture file."""
    packets = []
    with open(capture_file) as f:
        for line_no, line in enumerate(f):
            pkt = _parse_packet_line(line_no, line)
            if pkt is not None:
                packets.append(pkt)
    return packets


def _collect_partial_chains(packets):
    partial_chains = defaultdict(list)
    for pkt_idx, pkt in enumerate(packets):
        parsed = parse_packet(pkt['raw'], pkt['dir'])
        if not parsed or 'bunches' not in parsed:
            continue
        for b_idx, bunch in enumerate(parsed['bunches']):
            if bunch['partial']:
                partial_chains[bunch['ch']].append({
                    'pkt_idx': pkt_idx,
                    'bunch_idx': b_idx,
                    'bunch': bunch,
                    'inner_data': parsed['inner_data'],
                    'dir': pkt['dir'],
                })
    return partial_chains


def _copy_fragment_bits(inner, src_start, num_bits, reassembled_data, write_pos):
    for i in range(num_bits):
        src_bit = src_start + i
        src_byte = src_bit >> 3
        if src_byte < len(inner) and ((inner[src_byte] >> (src_bit & 7)) & 1):
            reassembled_data[write_pos >> 3] |= 1 << (write_pos & 7)
        write_pos += 1
    return write_pos


def _stitch_chain_bits(current_chain):
    total_bits = sum(e['bunch']['bunch_data_bits'] for e in current_chain)
    reassembled_data = bytearray((total_bits + 7) // 8)
    write_pos = 0
    for ce in current_chain:
        cb = ce['bunch']
        write_pos = _copy_fragment_bits(
            ce['inner_data'], cb['data_start'], cb['bunch_data_bits'],
            reassembled_data, write_pos,
        )
    first_bunch = current_chain[0]['bunch']
    synth_bunch = dict(first_bunch)
    synth_bunch['bunch_data_bits'] = total_bits
    synth_bunch['data_start'] = 0
    synth_bunch['_reassembled'] = True
    synth_bunch['_chain_len'] = len(current_chain)
    synth_bunch['partial'] = True
    synth_bunch['partial_initial'] = True
    synth_bunch['partial_final'] = True
    return synth_bunch, bytes(reassembled_data), current_chain[0]['dir']


def _finalize_chain(current_chain, reassembled, skip_set):
    reassembled.append(_stitch_chain_bits(current_chain))
    for ce in current_chain:
        skip_set.add((ce['pkt_idx'], ce['bunch_idx']))


def _reassemble_channel_chains(entries, reassembled, skip_set):
    current_chain = None
    for entry in entries:
        b = entry['bunch']
        if b['partial_initial']:
            current_chain = None if b['partial_final'] else [entry]
        elif current_chain is not None:
            current_chain.append(entry)
            if b['partial_final']:
                _finalize_chain(current_chain, reassembled, skip_set)
                current_chain = None


def reassemble_partial_bunches(packets):
    """Find complete partial bunch chains and reassemble them into full synthetic bunches."""
    partial_chains = _collect_partial_chains(packets)
    reassembled = []
    skip_set = set()
    for entries in partial_chains.values():
        _reassemble_channel_chains(entries, reassembled, skip_set)
    return reassembled, skip_set


def _init_analysis_ctx(total_packets, skip_set):
    return {
        'guid_cache': GUIDCache(),
        'skip_set': skip_set,
        'stats': {
            'total_packets': total_packets,
            'parsed_packets': 0,
            'total_bunches': 0,
            'open_bunches': 0,
            'close_bunches': 0,
            'export_bunches': 0,
            'partial_bunches': 0,
            'perfect_bunches': 0,
            'error_bunches': 0,
            'partial_skipped': 0,
            'reassembled_perfect': 0,
            'reassembled_error': 0,
            'total_blocks': 0,
            'guid_exports': 0,
            'rep_layout_exports': 0,
            'actors_spawned': 0,
            'dynamic_actors': 0,
        },
        'channel_stats': defaultdict(lambda: {
            'bunches': 0, 'opens': 0, 'closes': 0, 'exports': 0,
            'perfect': 0, 'errors': 0, 'names': set(), 'dirs': Counter(),
            'total_bits': 0, 'consumed_bits': 0,
        }),
        'actor_catalog': {},
        'export_catalog': [],
        'all_bunch_results': [],
        'events': [],
    }


def _classify_bunch_quality(result):
    is_perfect = result['block_errors'] == 0 and result['bits_remaining'] <= 2
    has_error = result['block_errors'] > 0
    if 'new_actor' in result and 'error' in result['new_actor']:
        na_err = result['new_actor'].get('error', '')
        if na_err == 'truncated' and result['bits_remaining'] <= 0 and result['block_errors'] == 0:
            is_perfect = True
        else:
            has_error = True
    return is_perfect, has_error


def _record_guid_exports(ctx, result, ch):
    if 'guid_exports' not in result:
        return
    guid_cache = ctx['guid_cache']
    for exp in result['guid_exports'].get('exports', []):
        ctx['stats']['guid_exports'] += 1
        export_entry = {
            'value': exp['value'],
            'dynamic': exp.get('dynamic', False),
            'flags': exp.get('flags', 0),
            'channel': ch,
        }
        if exp.get('name'):
            export_entry['name'] = exp['name']
            export_entry['path'] = guid_cache.resolve(exp['value'])
        outer = exp.get('outer')
        if outer and outer.get('value'):
            export_entry['outer_value'] = outer['value']
            if outer.get('name'):
                export_entry['outer_name'] = outer['name']
        ctx['export_catalog'].append(export_entry)


def _build_actor_info(na, guid_cache, filter_raw):
    loc = None if (filter_raw and na.get('location_raw')) else na.get('location')
    scale = None if (filter_raw and na.get('scale_raw')) else na.get('scale')
    vel = None if (filter_raw and na.get('velocity_raw')) else na.get('velocity')
    return {
        'guid': na.get('value', 0),
        'dynamic': na.get('dynamic', False),
        'archetype': na.get('archetype_value'),
        'level': na.get('level_value'),
        'location': loc,
        'rotation': na.get('rotation'),
        'scale': scale,
        'velocity': vel,
        'archetype_path': guid_cache.resolve(na.get('archetype_value', 0)),
        'level_path': guid_cache.resolve(na.get('level_value', 0)),
    }


def _record_bunch_events(ctx, pkt_idx, pkt, ch, result):
    if 'new_actor' in result:
        na = result['new_actor']
        ctx['stats']['actors_spawned'] += 1
        if na.get('dynamic'):
            ctx['stats']['dynamic_actors'] += 1
        actor_info = _build_actor_info(na, ctx['guid_cache'], True)
        ctx['actor_catalog'][ch] = actor_info
        ctx['events'].append({
            'type': 'spawn', 'ch': ch, 'ts': pkt['ts'],
            'pkt': pkt_idx, 'dir': pkt['dir'], 'actor': actor_info,
        })
    blocks = result.get('blocks')
    if blocks:
        ctx['events'].append({
            'type': 'replication', 'ch': ch, 'ts': pkt['ts'],
            'pkt': pkt_idx, 'dir': pkt['dir'],
            'num_blocks': len(blocks),
            'total_payload_bits': sum(b.get('payload_bits', 0) for b in blocks),
            'block_summary': [
                {
                    'has_rep': b['header']['has_rep'],
                    'is_actor': b['header']['is_actor'],
                    'payload_bits': b.get('payload_bits', 0),
                    'sub_guid': b['header'].get('sub_value'),
                }
                for b in blocks
            ],
        })


def _update_bunch_header_stats(ctx, pkt_idx, pkt, bunch):
    stats = ctx['stats']
    ch = bunch['ch']
    cs = ctx['channel_stats'][ch]
    cs['bunches'] += 1
    cs['dirs'][pkt['dir']] += 1
    if bunch['ch_name']:
        cs['names'].add(bunch['ch_name'])
    cs['total_bits'] += bunch['bunch_data_bits']
    if bunch['open']:
        stats['open_bunches'] += 1
        cs['opens'] += 1
    if bunch['close']:
        stats['close_bunches'] += 1
        cs['closes'] += 1
        ctx['events'].append({
            'type': 'close', 'ch': ch, 'ts': pkt['ts'],
            'pkt': pkt_idx, 'dir': pkt['dir'],
        })
    if bunch['has_exports']:
        stats['export_bunches'] += 1
        cs['exports'] += 1
    if bunch['partial']:
        stats['partial_bunches'] += 1


def _process_packet_bunch(ctx, pkt_idx, pkt, parsed, b_idx):
    bunch = parsed['bunches'][b_idx]
    stats = ctx['stats']
    stats['total_bunches'] += 1
    if (pkt_idx, b_idx) in ctx['skip_set']:
        ctx['all_bunch_results'].append(None)
        return
    _update_bunch_header_stats(ctx, pkt_idx, pkt, bunch)
    ch = bunch['ch']
    if ch == 0:
        ctx['all_bunch_results'].append(None)
        return
    result = decode_bunch_data(parsed['inner_data'], bunch, pkt['dir'], ctx['guid_cache'])
    ctx['all_bunch_results'].append(result)
    is_perfect, has_error = _classify_bunch_quality(result)
    is_partial_frag = bunch['partial'] and not (bunch['partial_initial'] and bunch['partial_final'])
    result['_is_partial_fragment'] = is_partial_frag
    cs = ctx['channel_stats'][ch]
    if is_perfect:
        stats['perfect_bunches'] += 1
        cs['perfect'] += 1
    elif has_error:
        if is_partial_frag:
            stats['partial_skipped'] += 1
        else:
            stats['error_bunches'] += 1
            cs['errors'] += 1
    cs['consumed_bits'] += result['bits_consumed']
    stats['total_blocks'] += len(result.get('blocks', []))
    _record_guid_exports(ctx, result, ch)
    if 'rep_layout' in result:
        stats['rep_layout_exports'] += 1
    _record_bunch_events(ctx, pkt_idx, pkt, ch, result)


def _process_reassembled_bunch(ctx, synth_bunch, reassembled_data, direction):
    ch = synth_bunch['ch']
    if ch == 0:
        return
    cs = ctx['channel_stats'][ch]
    stats = ctx['stats']
    result = decode_bunch_data(reassembled_data, synth_bunch, direction, ctx['guid_cache'])
    ctx['all_bunch_results'].append(result)
    result['_reassembled'] = True
    is_perfect, has_error = _classify_bunch_quality(result)
    if is_perfect:
        stats['perfect_bunches'] += 1
        stats['reassembled_perfect'] += 1
        cs['perfect'] += 1
    elif has_error:
        stats['error_bunches'] += 1
        stats['reassembled_error'] += 1
        cs['errors'] += 1
    cs['consumed_bits'] += result['bits_consumed']
    stats['total_blocks'] += len(result.get('blocks', []))
    _record_guid_exports(ctx, result, ch)
    if 'new_actor' in result:
        na = result['new_actor']
        stats['actors_spawned'] += 1
        if na.get('dynamic'):
            stats['dynamic_actors'] += 1
        ctx['actor_catalog'][ch] = _build_actor_info(na, ctx['guid_cache'], False)


def _print_stats_summary(ctx):
    stats = ctx['stats']
    non_ctrl = sum(1 for b in ctx['all_bunch_results'] if b is not None)
    pct = (stats['perfect_bunches'] / non_ctrl * 100) if non_ctrl else 0
    print(f"\n{'='*70}\n  PHASE 1 PARSER RESULTS\n{'='*70}")
    print(f"  Packets parsed:     {stats['parsed_packets']}/{stats['total_packets']}")
    print(f"  Total bunches:      {stats['total_bunches']}")
    print(f"    Open:             {stats['open_bunches']}")
    print(f"    Close:            {stats['close_bunches']}")
    print(f"    Exports:          {stats['export_bunches']}")
    print(f"    Partial:          {stats['partial_bunches']}")
    print(f"  Perfect bunches:    {stats['perfect_bunches']}/{non_ctrl} ({pct:.1f}%)")
    print(f"  Error bunches:      {stats['error_bunches']}")
    if stats['partial_skipped']:
        print(f"  Partial skipped:    {stats['partial_skipped']} (fragment errors ignored)")
    if stats['reassembled_perfect'] or stats['reassembled_error']:
        tot_r = stats['reassembled_perfect'] + stats['reassembled_error']
        print(f"  Reassembled:        {tot_r} ({stats['reassembled_perfect']} perfect, {stats['reassembled_error']} errors)")
    print(f"  GUID exports:       {stats['guid_exports']}")
    print(f"  RepLayout exports:  {stats['rep_layout_exports']}")
    print(f"  Actors spawned:     {stats['actors_spawned']} ({stats['dynamic_actors']} dynamic)")
    print(f"  Unique GUID paths:  {len(ctx['guid_cache'].names)}")


def _print_guid_summary(guid_cache):
    game_paths = {}
    script_paths = {}
    other_paths = {}
    for gv, name in guid_cache.names.items():
        full = guid_cache.resolve(gv) or name
        if '/Game/' in full:
            game_paths[gv] = full
        elif '/Script/' in full:
            script_paths[gv] = full
        else:
            other_paths[gv] = full
    print(f"\n  /Game/ paths:   {len(game_paths)}")
    print(f"  /Script/ paths: {len(script_paths)}")
    print(f"  Other paths:    {len(other_paths)}")
    if game_paths:
        print("\n  /Game/ and /Script/ paths:")
        for gv in sorted(list(game_paths.keys()) + list(script_paths.keys()))[:50]:
            path = game_paths.get(gv) or script_paths.get(gv, '?')
            print(f"    GUID {gv:>10}: {path[:100]}")


def _print_channel_summary(channel_stats):
    print(f"\n{'='*70}\n  CHANNEL SUMMARY (top 40 by bunch count)\n{'='*70}")
    print(f"  {'Ch':>5}  {'Bnch':>5}  {'Open':>4}  {'Cls':>4}  {'Exp':>4}  "
          f"{'Perf':>5}  {'Err':>4}  {'ConsPct':>7}  {'Name':20}  {'Dir'}")
    for ch, cs in sorted(channel_stats.items(), key=lambda x: -x[1]['bunches'])[:40]:
        names = ', '.join(sorted(cs['names']))[:20] or '-'
        dirs = ' '.join(f"{d}:{c}" for d, c in cs['dirs'].items())
        cons_pct = (cs['consumed_bits'] / cs['total_bits'] * 100) if cs['total_bits'] else 0
        print(f"  {ch:>5}  {cs['bunches']:>5}  {cs['opens']:>4}  {cs['closes']:>4}  "
              f"{cs['exports']:>4}  {cs['perfect']:>5}  {cs['errors']:>4}  "
              f"{cons_pct:>6.1f}%  {names:20}  {dirs}")


def _print_actor_catalog(actor_catalog, dynamic_count):
    if not actor_catalog:
        return
    print(f"\n{'='*70}\n  ACTOR CATALOG ({len(actor_catalog)} actors, {dynamic_count} dynamic)\n{'='*70}")
    dynamic_actors = {ch: a for ch, a in actor_catalog.items() if a['dynamic']}
    static_actors = {ch: a for ch, a in actor_catalog.items() if not a['dynamic']}
    if dynamic_actors:
        print(f"\n  Dynamic actors ({len(dynamic_actors)}):")
        for ch, a in sorted(dynamic_actors.items())[:50]:
            arch_path = a.get('archetype_path') or f"GUID={a.get('archetype', '?')}"
            loc = a.get('location', '?')
            rot = a.get('rotation', '?')
            print(f"    Ch {ch:>4}: GUID={a['guid']:>10}  arch={arch_path[:60]}")
            if loc and loc != '?':
                print(f"            loc={loc}  rot={rot}")
    if static_actors:
        print(f"\n  Static actors ({len(static_actors)}):")
        for ch, a in sorted(static_actors.items())[:30]:
            print(f"    Ch {ch:>4}: GUID={a['guid']:>10}")


def _print_single_bunch_error(idx, result):
    rem = result['bits_remaining']
    cons = result['bits_consumed']
    tot = result['bits_total']
    frag_tag = " (partial fragment)" if result.get('_is_partial_fragment') else ""
    print(f"\n  [{idx}] consumed={cons}/{tot}b remaining={rem}b{frag_tag}")
    if 'new_actor' in result and 'error' in result['new_actor']:
        print(f"    NewActor ERROR: {result['new_actor'].get('error')}")
    for j, block in enumerate(result.get('blocks', [])):
        if 'error' in block:
            print(f"    Block[{j}]: {block['error']} | hdr={block['header']}")


def _print_error_summary(all_bunch_results):
    print(f"\n{'='*70}\n  FIRST 20 ERRORS\n{'='*70}")
    err_shown = 0
    for i, result in enumerate(all_bunch_results):
        if result is None:
            continue
        has_err = result.get('block_errors', 0) > 0 or (
            'new_actor' in result and 'error' in result.get('new_actor', {})
        )
        if not has_err:
            continue
        err_shown += 1
        if err_shown > 20:
            break
        _print_single_bunch_error(i, result)


def _write_json_output(ctx, json_output):
    guid_cache = ctx['guid_cache']
    output = {
        'stats': ctx['stats'],
        'guid_names': {str(k): v for k, v in guid_cache.names.items()},
        'guid_outers': {str(k): v for k, v in guid_cache.outers.items()},
        'guid_dynamic': {str(k): v for k, v in guid_cache.dynamic.items()},
        'actor_catalog': {str(k): v for k, v in ctx['actor_catalog'].items()},
        'export_catalog': ctx['export_catalog'][:2000],
        'events': ctx['events'],
        'channel_stats': {
            str(ch): {
                'bunches': cs['bunches'], 'opens': cs['opens'], 'closes': cs['closes'],
                'exports': cs['exports'], 'perfect': cs['perfect'], 'errors': cs['errors'],
                'names': list(cs['names']),
            }
            for ch, cs in ctx['channel_stats'].items()
        },
    }
    with open(json_output, 'w') as f:
        json.dump(output, f, indent=2, default=str)
    print(f"\nJSON output: {json_output}")


def run_analysis(capture_file, json_output=None, summary_only=False):
    print(f"Loading {capture_file}...")
    packets = load_packets(capture_file)
    s2c = sum(1 for p in packets if p['dir'] == 'S>C')
    c2s = sum(1 for p in packets if p['dir'] == 'C>S')
    print(f"  Data packets: {len(packets)} ({s2c} S>C, {c2s} C>S)")
    reassembled_bunches, skip_set = reassemble_partial_bunches(packets)
    print(f"  Reassembled partial chains: {len(reassembled_bunches)} "
          f"(skipping {len(skip_set)} individual fragments)")
    ctx = _init_analysis_ctx(len(packets), skip_set)
    for pkt_idx, pkt in enumerate(packets):
        parsed = parse_packet(pkt['raw'], pkt['dir'])
        if parsed is None:
            continue
        ctx['stats']['parsed_packets'] += 1
        for b_idx in range(len(parsed['bunches'])):
            _process_packet_bunch(ctx, pkt_idx, pkt, parsed, b_idx)
    for synth_bunch, reassembled_data, direction in reassembled_bunches:
        _process_reassembled_bunch(ctx, synth_bunch, reassembled_data, direction)
    _print_stats_summary(ctx)
    _print_guid_summary(ctx['guid_cache'])
    _print_channel_summary(ctx['channel_stats'])
    _print_actor_catalog(ctx['actor_catalog'], ctx['stats']['dynamic_actors'])
    _print_error_summary(ctx['all_bunch_results'])
    if json_output:
        _write_json_output(ctx, json_output)
    print("\nDone.")
    return ctx['stats']


def _resolve_capture_path(arg_capture):
    if arg_capture:
        return arg_capture
    logs_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'logs')
    captures = sorted(
        [f for f in os.listdir(logs_dir) if f.startswith('capture-') and f.endswith('.jsonl')],
        key=lambda f: os.path.getsize(os.path.join(logs_dir, f)),
        reverse=True,
    )
    if captures:
        print(f"Using largest capture: {captures[0]}")
        return os.path.join(logs_dir, captures[0])
    print("No capture files found in logs/")
    sys.exit(1)


def main():
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description='Phase 1: UE5/AoC Packet Parser')
    parser.add_argument('capture', nargs='?', help='JSONL capture file')
    parser.add_argument('--json', help='Output JSON file')
    parser.add_argument('--summary', action='store_true', help='Summary only')
    args = parser.parse_args()
    capture = _resolve_capture_path(args.capture)
    json_out = args.json or os.path.join(os.path.dirname(__file__), 'phase1_results.json')
    run_analysis(capture, json_output=json_out, summary_only=args.summary)


if __name__ == '__main__':
    main()
