// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.RegularExpressions;

namespace PcapSpawns;

public sealed record CapturedGatherablePoint(
    long DefGuid,
    double X,
    double Y,
    double Z);

public static class AreaSpawnsParser
{
    private static readonly Regex PayloadRegex = new(
        @"\{\s*(\d+)\s*,\s*((?:""[0-9a-fA-F]*""\s*)+)\}",
        RegexOptions.Compiled);

    private static readonly Regex BlockRegex = new(
        @"\{\s*""([^""]+)""\s*,\s*(\d+)\s*,\s*(true|false)\s*,\s*(\d+)",
        RegexOptions.Compiled);

    private static readonly Regex OpenRegex = new(
        @"\{\s*""([^""]+)""\s*,\s*""([^""]+)""\s*,\s*""([^""]+)""\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\{\s*(true|false)\s*,\s*(\d+)\s*,\s*(-?\d+)(?:ll|ull|u)?\s*,\s*(-?\d+)(?:ll|ull|u)?\s*,\s*(-?\d+)(?:ll|ull|u)?\s*\}\s*,\s*\{[^}]*\}\s*,\s*\{[^}]*\}\s*,\s*(\d+)\s*,\s*(\d+)\s*\}",
        RegexOptions.Compiled);

    private static readonly Regex SpawnRegex = new(
        @"\{\s*(\d+)\s*,\s*(-?[0-9.]+)\s*,\s*(-?[0-9.]+)\s*,\s*(-?[0-9.]+)\s*\}",
        RegexOptions.Compiled);

    public static List<CapturedGatherablePoint> ParseFile(
        string areaSpawnsHeaderPath, HashSet<long> validGatherGuids)
    {
        var results = new List<CapturedGatherablePoint>();
        if (!File.Exists(areaSpawnsHeaderPath)) return results;
        string text = File.ReadAllText(areaSpawnsHeaderPath);
        var payloads = ParsePayloads(SliceSection(text, "kPayloads[]", "};"));
        var blocks = ParseBlocks(SliceSection(text, "kBlocks[]", "};"));
        var openDefs = ParseOpens(
            SliceSection(text, "kOpens[]", "};"), payloads, blocks, validGatherGuids, results);
        ParseSpawns(SliceSection(text, "kSpawns[]", "};"), openDefs, results);
        return results;
    }

    private static string SliceSection(string text, string startMarker, string endMarker)
    {
        int start = text.IndexOf(startMarker, StringComparison.Ordinal);
        if (start < 0) return string.Empty;
        int end = text.IndexOf(endMarker, start, StringComparison.Ordinal);
        if (end < 0) return text[start..];
        return text[start..end];
    }

    private static List<(int bits, byte[] bytes)> ParsePayloads(string section)
    {
        var list = new List<(int bits, byte[] bytes)>();
        foreach (Match m in PayloadRegex.Matches(section))
        {
            int bits = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
            string hex = m.Groups[2].Value.Replace("\"", "").Replace(" ", "").Replace("\r", "").Replace("\n", "");
            byte[] bytes = Convert.FromHexString(hex);
            list.Add((bits, bytes));
        }
        return list;
    }

    private static List<(string cls, int kind, bool rep, int payloadIdx)> ParseBlocks(string section)
    {
        var list = new List<(string cls, int kind, bool rep, int payloadIdx)>();
        foreach (Match m in BlockRegex.Matches(section))
        {
            string cls = m.Groups[1].Value;
            int kind = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
            bool rep = string.Equals(m.Groups[3].Value, "true", StringComparison.Ordinal);
            int payloadIdx = int.Parse(m.Groups[4].Value, CultureInfo.InvariantCulture);
            list.Add((cls, kind, rep, payloadIdx));
        }
        return list;
    }

    private static Dictionary<int, long> ParseOpens(
        string section,
        List<(int bits, byte[] bytes)> payloads,
        List<(string cls, int kind, bool rep, int payloadIdx)> blocks,
        HashSet<long> validGatherGuids,
        List<CapturedGatherablePoint> results)
    {
        var openDefByIdx = new Dictionary<int, long>();
        int openIdx = 0;
        foreach (Match m in OpenRegex.Matches(section))
        {
            int idx = openIdx++;
            string actorClass = m.Groups[2].Value;
            if (!string.Equals(actorClass, "AGatherableActor", StringComparison.Ordinal)) continue;
            bool locPresent = string.Equals(m.Groups[4].Value, "true", StringComparison.Ordinal);
            int mode = int.Parse(m.Groups[5].Value, CultureInfo.InvariantCulture);
            long qx = long.Parse(m.Groups[6].Value, CultureInfo.InvariantCulture);
            long qy = long.Parse(m.Groups[7].Value, CultureInfo.InvariantCulture);
            long qz = long.Parse(m.Groups[8].Value, CultureInfo.InvariantCulture);
            int firstBlock = int.Parse(m.Groups[9].Value, CultureInfo.InvariantCulture);
            int blockCount = int.Parse(m.Groups[10].Value, CultureInfo.InvariantCulture);
            if (!TryDecodeOpenGatherable(
                (firstBlock, blockCount), payloads, blocks, validGatherGuids, out var rep))
            {
                continue;
            }
            openDefByIdx[idx] = rep.DefGuid;
            if (locPresent && TryConvertVec(mode, (qx, qy, qz), out var pt))
            {
                results.Add(new CapturedGatherablePoint(rep.DefGuid, pt.x, pt.y, pt.z));
            }
            else if (rep.HasRepLoc)
            {
                results.Add(new CapturedGatherablePoint(rep.DefGuid, rep.RepX, rep.RepY, rep.RepZ));
            }
        }
        return openDefByIdx;
    }

    private static bool TryDecodeOpenGatherable(
        (int firstBlock, int blockCount) span,
        List<(int bits, byte[] bytes)> payloads,
        List<(string cls, int kind, bool rep, int payloadIdx)> blocks,
        HashSet<long> validGatherGuids,
        out GatherableRepResult rep)
    {
        rep = default;
        for (int b = 0; b < span.blockCount; b++)
        {
            int bi = span.firstBlock + b;
            if (bi < 0 || bi >= blocks.Count) continue;
            var blk = blocks[bi];
            if (!blk.rep || !string.Equals(blk.cls, "AGatherableActor", StringComparison.Ordinal)) continue;
            if (blk.payloadIdx < 0 || blk.payloadIdx >= payloads.Count) continue;
            var p = payloads[blk.payloadIdx];
            var reader = new Ue5BitReader(p.bytes, p.bits);
            if (reader.TryReadGatherableRepBlock(validGatherGuids, out rep)) return true;
        }
        return false;
    }

    private static bool TryConvertVec(
        int mode, (long qx, long qy, long qz) q, out (double x, double y, double z) pt)
    {
        switch (mode)
        {
            case 0:
                pt = (q.qx / 10.0, q.qy / 10.0, q.qz / 10.0);
                return true;
            case 1:
                pt = (q.qx, q.qy, q.qz);
                return true;
            case 2:
                pt = (
                    BitConverter.UInt32BitsToSingle(unchecked((uint)q.qx)),
                    BitConverter.UInt32BitsToSingle(unchecked((uint)q.qy)),
                    BitConverter.UInt32BitsToSingle(unchecked((uint)q.qz)));
                return true;
            default:
                pt = (
                    BitConverter.UInt64BitsToDouble(unchecked((ulong)q.qx)),
                    BitConverter.UInt64BitsToDouble(unchecked((ulong)q.qy)),
                    BitConverter.UInt64BitsToDouble(unchecked((ulong)q.qz)));
                return true;
        }
    }

    private static void ParseSpawns(
        string section,
        Dictionary<int, long> openDefByIdx,
        List<CapturedGatherablePoint> results)
    {
        foreach (Match m in SpawnRegex.Matches(section))
        {
            int openIdx = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
            if (!openDefByIdx.TryGetValue(openIdx, out long defGuid)) continue;
            double x = double.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
            double y = double.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture);
            double z = double.Parse(m.Groups[4].Value, CultureInfo.InvariantCulture);
            results.Add(new CapturedGatherablePoint(defGuid, x, y, z));
        }
    }
}
