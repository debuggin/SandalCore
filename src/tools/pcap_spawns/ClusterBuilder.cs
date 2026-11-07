// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text;

namespace PcapSpawns;

public sealed record ResourceSpawnerRow(
    int SpawnerId,
    GatherDefEntry Def,
    ZoiEntry Zoi,
    double CenterX,
    double CenterY,
    double CenterZ,
    double RadiusCm,
    List<(double X, double Y, double Z)> Points);

public sealed class PaintedFoliageSection
{
    public string DefName { get; }
    public double MinX { get; }
    public double MaxX { get; }
    public double MinY { get; }
    public double MaxY { get; }
    public IReadOnlyList<(double X, double Y)> Hull { get; }

    public PaintedFoliageSection(
        string defName,
        double minX,
        double maxX,
        double minY,
        double maxY,
        IReadOnlyList<(double X, double Y)> hull)
    {
        DefName = defName;
        MinX = minX;
        MaxX = maxX;
        MinY = minY;
        MaxY = maxY;
        Hull = hull;
    }

    public bool Contains(double x, double y, double padCm)
    {
        if (x < MinX - padCm || x > MaxX + padCm || y < MinY - padCm || y > MaxY + padCm) return false;
        if (Hull.Count == 0) return false;
        if (Hull.Count == 1)
        {
            double dx = x - Hull[0].X;
            double dy = y - Hull[0].Y;
            return dx * dx + dy * dy <= padCm * padCm;
        }
        if (Hull.Count == 2)
        {
            return DistanceToSegmentSq(x, y, Hull[0], Hull[1]) <= padCm * padCm;
        }
        for (int i = 0; i < Hull.Count; i++)
        {
            var a = Hull[i];
            var b = Hull[(i + 1) % Hull.Count];
            double ex = b.X - a.X;
            double ey = b.Y - a.Y;
            double cross = ex * (y - a.Y) - ey * (x - a.X);
            if (cross < -padCm * Math.Sqrt(ex * ex + ey * ey)) return false;
        }
        return true;
    }

    private static double DistanceToSegmentSq(
        double px, double py, (double X, double Y) a, (double X, double Y) b)
    {
        double vx = b.X - a.X;
        double vy = b.Y - a.Y;
        double lenSq = vx * vx + vy * vy;
        double t = lenSq > 1e-9 ? Math.Clamp(((px - a.X) * vx + (py - a.Y) * vy) / lenSq, 0.0, 1.0) : 0.0;
        double dx = px - (a.X + t * vx);
        double dy = py - (a.Y + t * vy);
        return dx * dx + dy * dy;
    }
}

public sealed class PaintedFoliageIndex
{
    private readonly Dictionary<(int Cx, int Cy), List<PaintedFoliageSection>> _cells = new();

    public int SectionCount { get; private set; }
    public int InstanceCount { get; set; }

    public void AddSection(PaintedFoliageSection section, double cellCm, double padCm)
    {
        SectionCount++;
        int cx0 = (int)Math.Floor((section.MinX - padCm) / cellCm);
        int cx1 = (int)Math.Floor((section.MaxX + padCm) / cellCm);
        int cy0 = (int)Math.Floor((section.MinY - padCm) / cellCm);
        int cy1 = (int)Math.Floor((section.MaxY + padCm) / cellCm);
        for (int cx = cx0; cx <= cx1; cx++)
        {
            for (int cy = cy0; cy <= cy1; cy++)
            {
                if (!_cells.TryGetValue((cx, cy), out var list))
                {
                    list = new List<PaintedFoliageSection>();
                    _cells[(cx, cy)] = list;
                }
                list.Add(section);
            }
        }
    }

    public bool Contains(double x, double y, string defName, double cellCm, double padCm)
    {
        int cx = (int)Math.Floor(x / cellCm);
        int cy = (int)Math.Floor(y / cellCm);
        if (!_cells.TryGetValue((cx, cy), out var list)) return false;
        for (int i = 0; i < list.Count; i++)
        {
            var s = list[i];
            if (string.Equals(s.DefName, defName, StringComparison.Ordinal) && s.Contains(x, y, padCm))
            {
                return true;
            }
        }
        return false;
    }
}

public static class ClusterBuilder
{
    public const double MaxClusterRadiusCm = 3500.0;
    public const double FoliageCellCm = 25600.0;
    public const double FoliageBrushPadCm = 500.0;
    public const double AreaEpsilonCm = 1.0e-3;
    private const double DedupDistanceCm = 10.0;
    private const int MaxPointsPerCluster = 12;

    public static PaintedFoliageIndex LoadPaintedFoliageIndex(string foliageTsvPath)
    {
        var index = new PaintedFoliageIndex();
        if (string.IsNullOrEmpty(foliageTsvPath) || !File.Exists(foliageTsvPath)) return index;
        var rawSections = new Dictionary<(string Owner, string Comp, string Def), List<(double X, double Y)>>();
        int instances = ReadFoliageRows(foliageTsvPath, rawSections);
        index.InstanceCount = instances;
        double pad = FoliageBrushPadCm + AreaEpsilonCm;
        foreach (var kv in rawSections)
        {
            var section = BuildSection(kv.Key.Def, kv.Value);
            index.AddSection(section, FoliageCellCm, pad);
        }
        return index;
    }

    private static int ReadFoliageRows(
        string foliageTsvPath,
        Dictionary<(string Owner, string Comp, string Def), List<(double X, double Y)>> rawSections)
    {
        int instances = 0;
        using var sr = new StreamReader(foliageTsvPath);
        string? line;
        while ((line = sr.ReadLine()) != null)
        {
            if (line.Length == 0 || line[0] == '#') continue;
            string[] cols = line.Split('\t');
            if (cols.Length < 8) continue;
            if (!double.TryParse(cols[6], NumberStyles.Float, CultureInfo.InvariantCulture, out double x)) continue;
            if (!double.TryParse(cols[7], NumberStyles.Float, CultureInfo.InvariantCulture, out double y)) continue;
            var key = (cols[3], cols[4], cols[1]);
            if (!rawSections.TryGetValue(key, out var pts))
            {
                pts = new List<(double X, double Y)>();
                rawSections[key] = pts;
            }
            pts.Add((x, y));
            instances++;
        }
        return instances;
    }

    private static PaintedFoliageSection BuildSection(string defName, List<(double X, double Y)> pts)
    {
        double minX = double.PositiveInfinity, maxX = double.NegativeInfinity;
        double minY = double.PositiveInfinity, maxY = double.NegativeInfinity;
        for (int i = 0; i < pts.Count; i++)
        {
            if (pts[i].X < minX) minX = pts[i].X;
            if (pts[i].X > maxX) maxX = pts[i].X;
            if (pts[i].Y < minY) minY = pts[i].Y;
            if (pts[i].Y > maxY) maxY = pts[i].Y;
        }
        var hull = ComputeConvexHull(pts);
        return new PaintedFoliageSection(defName, minX, maxX, minY, maxY, hull);
    }

    private static List<(double X, double Y)> ComputeConvexHull(List<(double X, double Y)> pts)
    {
        pts.Sort((a, b) => a.X != b.X ? a.X.CompareTo(b.X) : a.Y.CompareTo(b.Y));
        var unique = new List<(double X, double Y)>(pts.Count);
        for (int i = 0; i < pts.Count; i++)
        {
            if (i == 0 || pts[i].X != pts[i - 1].X || pts[i].Y != pts[i - 1].Y) unique.Add(pts[i]);
        }
        if (unique.Count <= 2) return unique;
        var hull = new List<(double X, double Y)>(unique.Count * 2);
        for (int i = 0; i < unique.Count; i++)
        {
            while (hull.Count >= 2 && Cross(hull[hull.Count - 2], hull[hull.Count - 1], unique[i]) <= 0.0)
                hull.RemoveAt(hull.Count - 1);
            hull.Add(unique[i]);
        }
        int lowerCount = hull.Count + 1;
        for (int i = unique.Count - 2; i >= 0; i--)
        {
            while (hull.Count >= lowerCount && Cross(hull[hull.Count - 2], hull[hull.Count - 1], unique[i]) <= 0.0)
                hull.RemoveAt(hull.Count - 1);
            hull.Add(unique[i]);
        }
        hull.RemoveAt(hull.Count - 1);
        return hull;
    }

    private static double Cross((double X, double Y) o, (double X, double Y) a, (double X, double Y) b) =>
        (a.X - o.X) * (b.Y - o.Y) - (a.Y - o.Y) * (b.X - o.X);

    public static bool IsInPaintedFoliageSection(
        PaintedFoliageIndex? foliageIndex,
        double x,
        double y,
        GatherDefEntry def) =>
        foliageIndex != null && foliageIndex.Contains(x, y, def.Name, FoliageCellCm, FoliageBrushPadCm + AreaEpsilonCm);

    public static List<ResourceSpawnerRow> BuildClusters(
        IEnumerable<CapturedGatherablePoint> rawPoints,
        ContentDbCatalog catalog,
        PaintedFoliageIndex? foliageIndex,
        out int strippedPoints)
    {
        var grouped = GroupAndDeduplicate(rawPoints, catalog, foliageIndex, out strippedPoints);
        var rows = new List<ResourceSpawnerRow>();
        foreach (var kvp in grouped.OrderBy(k => k.Key.zoiGuid).ThenBy(k => k.Key.defGuid))
        {
            var def = catalog.GatherDefs[kvp.Key.defGuid];
            var zoi = catalog.Zois[kvp.Key.zoiGuid];
            var pts = kvp.Value
                .OrderBy(p => p.X)
                .ThenBy(p => p.Y)
                .ThenBy(p => p.Z)
                .ToList();
            ClusterGroup(def, zoi, pts, foliageIndex, rows);
        }
        return AssignIdsAndSort(rows);
    }

    private static Dictionary<(long defGuid, ulong zoiGuid), List<(double X, double Y, double Z)>> GroupAndDeduplicate(
        IEnumerable<CapturedGatherablePoint> rawPoints,
        ContentDbCatalog catalog,
        PaintedFoliageIndex? foliageIndex,
        out int strippedPoints)
    {
        strippedPoints = 0;
        var groups = new Dictionary<(long defGuid, ulong zoiGuid), List<(double X, double Y, double Z)>>();
        foreach (var pt in rawPoints)
        {
            if (!double.IsFinite(pt.X) || !double.IsFinite(pt.Y) || !double.IsFinite(pt.Z)) continue;
            if (!catalog.GatherDefs.TryGetValue(pt.DefGuid, out var def)) continue;
            if (!catalog.TryResolveZoi(pt.X, pt.Y, out ulong zoiGuid, out _)) continue;
            if (IsInPaintedFoliageSection(foliageIndex, pt.X, pt.Y, def))
            {
                strippedPoints++;
                continue;
            }
            var key = (pt.DefGuid, zoiGuid);
            if (!groups.TryGetValue(key, out var list))
            {
                list = new List<(double X, double Y, double Z)>();
                groups[key] = list;
            }
            if (!IsDuplicate(list, pt.X, pt.Y, pt.Z))
            {
                list.Add((Math.Round(pt.X, 1), Math.Round(pt.Y, 1), Math.Round(pt.Z, 1)));
            }
        }
        return groups;
    }

    private static bool IsDuplicate(List<(double X, double Y, double Z)> list, double x, double y, double z)
    {
        double thr = DedupDistanceCm + AreaEpsilonCm;
        double thrSq = thr * thr;
        for (int i = 0; i < list.Count; i++)
        {
            double dx = list[i].X - x;
            double dy = list[i].Y - y;
            double dz = list[i].Z - z;
            if (dx * dx + dy * dy + dz * dz <= thrSq) return true;
        }
        return false;
    }

    private static void ClusterGroup(
        GatherDefEntry def,
        ZoiEntry zoi,
        List<(double X, double Y, double Z)> pts,
        PaintedFoliageIndex? foliageIndex,
        List<ResourceSpawnerRow> outRows)
    {
        var clusters = new List<List<(double X, double Y, double Z)>>();
        foreach (var pt in pts)
        {
            int bestIdx = FindCompatibleCluster(clusters, pt);
            if (bestIdx >= 0)
            {
                clusters[bestIdx].Add(pt);
            }
            else
            {
                clusters.Add(new List<(double X, double Y, double Z)> { pt });
            }
        }
        foreach (var c in clusters)
        {
            var (cx, cy, cz, radius) = ComputeBounds(c);
            if (IsInPaintedFoliageSection(foliageIndex, cx, cy, def)) continue;
            outRows.Add(new ResourceSpawnerRow(0, def, zoi, cx, cy, cz, radius, c));
        }
    }

    private static int FindCompatibleCluster(
        List<List<(double X, double Y, double Z)>> clusters,
        (double X, double Y, double Z) pt)
    {
        int bestIdx = -1;
        double limit = MaxClusterRadiusCm + AreaEpsilonCm;
        double bestDistSq = limit * limit;
        for (int i = 0; i < clusters.Count; i++)
        {
            var c = clusters[i];
            if (c.Count >= MaxPointsPerCluster) continue;
            double minMemberDistSq = double.MaxValue;
            for (int j = 0; j < c.Count; j++)
            {
                double dx = c[j].X - pt.X;
                double dy = c[j].Y - pt.Y;
                double dSq = dx * dx + dy * dy;
                if (dSq < minMemberDistSq) minMemberDistSq = dSq;
            }
            if (minMemberDistSq > bestDistSq) continue;
            if (WouldFitWithinRadius(c, pt))
            {
                bestDistSq = minMemberDistSq;
                bestIdx = i;
            }
        }
        return bestIdx;
    }

    private static bool WouldFitWithinRadius(
        List<(double X, double Y, double Z)> cluster,
        (double X, double Y, double Z) candidate)
    {
        int n = cluster.Count + 1;
        double sumX = candidate.X, sumY = candidate.Y;
        for (int i = 0; i < cluster.Count; i++)
        {
            sumX += cluster[i].X;
            sumY += cluster[i].Y;
        }
        double meanX = sumX / n, meanY = sumY / n;
        double cx = candidate.X, cy = candidate.Y;
        double bestAnchorSq = (cx - meanX) * (cx - meanX) + (cy - meanY) * (cy - meanY);
        for (int i = 0; i < cluster.Count; i++)
        {
            double dSq = (cluster[i].X - meanX) * (cluster[i].X - meanX) + (cluster[i].Y - meanY) * (cluster[i].Y - meanY);
            if (dSq < bestAnchorSq)
            {
                bestAnchorSq = dSq;
                cx = cluster[i].X;
                cy = cluster[i].Y;
            }
        }
        double limit = MaxClusterRadiusCm + AreaEpsilonCm;
        double limitSq = limit * limit;
        if ((candidate.X - cx) * (candidate.X - cx) + (candidate.Y - cy) * (candidate.Y - cy) > limitSq) return false;
        for (int i = 0; i < cluster.Count; i++)
        {
            double dx = cluster[i].X - cx;
            double dy = cluster[i].Y - cy;
            if (dx * dx + dy * dy > limitSq) return false;
        }
        return true;
    }

    private static (double cx, double cy, double cz, double radius) ComputeBounds(
        List<(double X, double Y, double Z)> cluster)
    {
        double sumX = 0.0, sumY = 0.0;
        for (int i = 0; i < cluster.Count; i++)
        {
            sumX += cluster[i].X;
            sumY += cluster[i].Y;
        }
        double meanX = sumX / cluster.Count;
        double meanY = sumY / cluster.Count;
        int bestIdx = 0;
        double bestAnchorSq = double.MaxValue;
        for (int i = 0; i < cluster.Count; i++)
        {
            double dx = cluster[i].X - meanX;
            double dy = cluster[i].Y - meanY;
            double dSq = dx * dx + dy * dy;
            if (dSq < bestAnchorSq)
            {
                bestAnchorSq = dSq;
                bestIdx = i;
            }
        }
        var anchor = cluster[bestIdx];
        double maxR = 0.0;
        for (int i = 0; i < cluster.Count; i++)
        {
            double dx = cluster[i].X - anchor.X;
            double dy = cluster[i].Y - anchor.Y;
            double r = Math.Sqrt(dx * dx + dy * dy);
            if (r > maxR) maxR = r;
        }
        double clampedR = Math.Min(MaxClusterRadiusCm, Math.Round(maxR, 1));
        return (anchor.X, anchor.Y, anchor.Z, clampedR);
    }

    private static List<ResourceSpawnerRow> AssignIdsAndSort(List<ResourceSpawnerRow> rows)
    {
        var sorted = rows
            .OrderBy(r => r.Zoi.Biome, StringComparer.Ordinal)
            .ThenBy(r => r.Def.Profession, StringComparer.Ordinal)
            .ThenBy(r => r.Def.Name, StringComparer.Ordinal)
            .ThenBy(r => r.CenterX)
            .ThenBy(r => r.CenterY)
            .ThenBy(r => r.CenterZ)
            .ToList();
        var finalRows = new List<ResourceSpawnerRow>(sorted.Count);
        for (int i = 0; i < sorted.Count; i++)
        {
            finalRows.Add(sorted[i] with { SpawnerId = i + 1 });
        }
        return finalRows;
    }

    public static void WriteTsv(string outputPath, IReadOnlyList<ResourceSpawnerRow> rows)
    {
        var sb = new StringBuilder(rows.Count * 256 + 256);
        sb.Append("spawner_id\tdef_guid\tdef_name\tprofession\tresource_type\ttier\ttier_name\tregion\tzoi_guid\tcenter_x\tcenter_y\tcenter_z\tradius_cm\tinstance_count\tpoints_json\n");
        var inv = CultureInfo.InvariantCulture;
        foreach (var r in rows)
        {
            sb.Append(r.SpawnerId.ToString(inv)).Append('\t')
              .Append(r.Def.Guid.ToString(inv)).Append('\t')
              .Append(r.Def.Name).Append('\t')
              .Append(r.Def.Profession).Append('\t')
              .Append(r.Def.ResourceType).Append('\t')
              .Append(r.Def.Tier.ToString(inv)).Append('\t')
              .Append(r.Def.TierName).Append('\t')
              .Append(r.Zoi.Biome).Append('\t')
              .Append(r.Zoi.Guid.ToString(inv)).Append('\t')
              .Append(r.CenterX.ToString("0.0", inv)).Append('\t')
              .Append(r.CenterY.ToString("0.0", inv)).Append('\t')
              .Append(r.CenterZ.ToString("0.0", inv)).Append('\t')
              .Append(r.RadiusCm.ToString("0.0", inv)).Append('\t')
              .Append(r.Points.Count.ToString(inv)).Append('\t')
              .Append(FormatPointsJson(r.Points, inv)).Append('\n');
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outputPath))!);
        File.WriteAllText(outputPath, sb.ToString(), new UTF8Encoding(false));
    }

    private static string FormatPointsJson(
        List<(double X, double Y, double Z)> pts, CultureInfo inv)
    {
        var sb = new StringBuilder(pts.Count * 32 + 4);
        sb.Append('[');
        for (int i = 0; i < pts.Count; i++)
        {
            if (i > 0) sb.Append(',');
            sb.Append('[')
              .Append(pts[i].X.ToString("0.0", inv)).Append(',')
              .Append(pts[i].Y.ToString("0.0", inv)).Append(',')
              .Append(pts[i].Z.ToString("0.0", inv)).Append(']');
        }
        sb.Append(']');
        return sb.ToString();
    }
}
