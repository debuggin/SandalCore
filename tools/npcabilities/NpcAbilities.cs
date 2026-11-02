// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.StateTree;
using CUE4Parse.UE4.Objects.StructUtils;
using CUE4Parse.UE4.Objects.UObject;
using CUE4Parse.Utils;
using Mapc;

namespace NpcAbilities;

/// <summary>Extracts creature StateTree ability rows (`npc_abilities.tsv`) and animation montage metadata (`npc_montages.tsv`).</summary>
public static partial class Reader
{
    public const string AbilitiesFile = "npc_abilities.tsv";
    public const string MontagesFile = "npc_montages.tsv";
    public const string AbilitiesHeader = "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival";
    public const string MontagesHeader = "montage\tguid\tpackage\tpackage_guid\tlength\trate_scale\tsections\thits\troot_motion\troot_unreadable\tanim_end\tfires";

    public sealed class AbilityRow
    {
        public string Archetype = "", Task = "", Gates = "", Conds = "", Why = "", Starts = "", Rotate = "";
        public long Ability;
        public float Cooldown;
        public bool StopMovement, Unknown, Fight;
        public int Order;
        public float MinDistance = -1f, MaxDistance = -1f, MaxAngle = -1f;
        public float Arrival = -1f;
    }

    public sealed class MontageRow
    {
        public string Montage = "", Package = "";
        public ulong Guid, PackageGuid;
        public float Length, RateScale = 1f;
        public float AnimEnd = -1f;
        public bool RootUnreadable;
        public List<(string Name, float Start, string Next)> Sections = new();
        public List<(float Time, int Key)> Hits = new();
        public List<(float Time, int Key)> Fires = new();
        public List<(float Time, float X, float Y, float Z)> RootMotion = new();
    }

    public sealed class CollectResult
    {
        public List<AbilityRow> Abilities = new();
        public List<MontageRow> Montages = new();
        public int Packages, Failed;
    }

    sealed class CollectState
    {
        public required Game Game;
        public required List<string> EndDirs;
        public Dictionary<string, List<AbilityRow>> TreeCache = new();
        public Dictionary<string, Parameters.Bag?> Bags = new();
        public CollectResult Output = new();

        public Parameters.Bag? BagOf(string containerPath, bool tree)
        {
            string key = (tree ? "T|" : "B|") + containerPath;
            if (!Bags.TryGetValue(key, out var bag)) Bags[key] = bag = Parameters.Read(Game, containerPath, tree);
            return bag;
        }
    }

    /// <summary>The id the network layer gives a static object: CityHash64 of "Class ObjectPath" (UTF-8), lowest bit set.</summary>
    public static ulong StaticId(string className, string path) => CityHash.CityHash64(Encoding.UTF8.GetBytes(className + " " + path)) | 1UL;

    static string PackagePathOf(string containerPath) => "/Game/" + containerPath.Substring("AOC/Content/".Length, containerPath.Length - "AOC/Content/".Length - ".uasset".Length);

    static string ContainerPathOf(string packageName) => "AOC/Content/" + packageName.Substring("/Game/".Length) + ".uasset";

    static List<UObject> CdoChain(UObject cdo)
    {
        var list = new List<UObject>();
        UObject? o = cdo;
        for (int i = 0; o != null && i < 12; i++)
        {
            list.Add(o);
            o = o.Template?.Object?.Value;
        }
        return list;
    }

    static string ClassName(UObject cdo) => cdo.Name.StartsWith("Default__", StringComparison.Ordinal) ? cdo.Name.Substring(9) : cdo.Name;

    static IEnumerable<FStructFallback> StructsOf(UObject o, string name)
    {
        var prop = o.Properties.FirstOrDefault(x => x.Name.Text == name);
        if (prop?.Tag?.GenericValue is UScriptArray arr)
            foreach (var item in arr.Properties)
                if (item.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback sf) yield return sf;
    }

    static long GuidOf(FStructFallback st)
    {
        if (st.TryGetValue(out long gl, "Guid")) return gl;
        if (st.TryGetValue(out ulong gu, "Guid")) return (long)gu;
        return 0;
    }

    static IPackage? StateTreeOf(Game g, List<UObject> chain, out string referencePackage)
    {
        referencePackage = "";
        foreach (var c in chain)
        {
            if (c.Owner is not IPackage pkg) continue;
            foreach (var e in pkg.GetExports())
            {
                if (e.ExportType != "StateTreeComponent") continue;
                if (!e.TryGetValue(out FStructFallback tree, "StateTreeRef") || tree == null) continue;
                if (!tree.TryGetValue(out FPackageIndex idx, "StateTree") || idx == null || idx.IsNull) continue;
                var pkgName = idx.ResolvedObject?.Package?.Name;
                if (string.IsNullOrEmpty(pkgName)) continue;
                try
                {
                    var loaded = g.Provider.LoadPackage(pkgName);
                    referencePackage = pkg.Name;
                    return loaded;
                }
                catch { }
            }
        }
        return null;
    }

    public sealed record TreeKind(string Tree, bool Wander, bool Patrol);

    public static TreeKind? TreeKindOf(Game g, UObject cdo, Dictionary<string, TreeKind?> cache)
    {
        var tree = StateTreeOf(g, CdoChain(cdo), out _);
        if (tree == null) return null;
        if (cache.TryGetValue(tree.Name, out var hit)) return hit;
        TreeKind? kind = null;
        try
        {
            var found = TreeTasks(g, tree, new HashSet<string>(), 0);
            if (found != null) kind = new TreeKind(tree.Name.Substring(tree.Name.LastIndexOf('/') + 1), found.Value.Wander, found.Value.Patrol);
        }
        catch { }
        cache[tree.Name] = kind;
        return kind;
    }

    static (bool Wander, bool Patrol)? TreeTasks(Game g, IPackage tree, HashSet<string> seen, int depth)
    {
        if (depth > 4 || !seen.Add(tree.Name)) return (false, false);
        var treeObject = tree.GetExports().FirstOrDefault(x => x.ExportType == "StateTree");
        if (treeObject == null) return null;
        bool wander = false, patrol = false;
        foreach (var node in Container(treeObject.Properties, "Nodes"))
        {
            var task = ClassOf(node, "TaskClass");
            if (task.Length == 0) continue;
            if (task.Contains("Wander", StringComparison.OrdinalIgnoreCase)) wander = true;
            if (task.Contains("Patrol", StringComparison.OrdinalIgnoreCase)) patrol = true;
        }
        foreach (var state in StructsOf(treeObject, "States"))
        {
            if (!state.TryGetValue(out FPackageIndex linked, "LinkedAsset") || linked == null || linked.IsNull) continue;
            var name = linked.ResolvedObject?.Package?.Name;
            if (string.IsNullOrEmpty(name)) continue;
            IPackage? other;
            try { other = g.Provider.LoadPackage(name); } catch { return null; }
            if (other == null) return null;
            var sub = TreeTasks(g, other, seen, depth + 1);
            if (sub == null) return null;
            wander |= sub.Value.Wander;
            patrol |= sub.Value.Patrol;
        }
        return (wander, patrol);
    }

    static string ParametersPackageOf(List<UObject> chain)
    {
        foreach (var c in chain)
        {
            if (c.Owner is not IPackage pkg) continue;
            foreach (var e in pkg.GetExports())
            {
                if (e.ExportType != "StateTreeComponent") continue;
                if (!e.TryGetValue(out FStructFallback reference, "StateTreeRef") || reference == null) continue;
                if (reference.Properties.Any(p => p.Name.Text == "Parameters")) return pkg.Name;
            }
        }
        return "";
    }

    static FStructFallback[] Container(List<FPropertyTag> props, string name)
    {
        var prop = props.FirstOrDefault(x => x.Name.Text == name);
        if (prop?.Tag?.GenericValue is not FScriptStruct ss) return Array.Empty<FStructFallback>();
        if (ss.StructType is FInstancedStructContainer c) return c.Structs ?? Array.Empty<FStructFallback>();
        return Array.Empty<FStructFallback>();
    }

    static FStructFallback[] InstanceStructs(UObject tree, string name)
    {
        var prop = tree.Properties.FirstOrDefault(x => x.Name.Text == name);
        if (prop?.Tag?.GenericValue is not FScriptStruct ss || ss.StructType is not FStateTreeInstanceData data || data.Data == null) return Array.Empty<FStructFallback>();
        return Container(data.Data.Properties, "InstanceStructs");
    }

    static int IntOf(FStructFallback? s, string name, int fallback = -1)
    {
        if (s == null) return fallback;
        if (s.TryGetValue(out int i, name)) return i;
        if (s.TryGetValue(out ushort u, name)) return u;
        if (s.TryGetValue(out byte by, name)) return by;
        if (s.TryGetValue(out short sh, name)) return sh;
        if (s.TryGetValue(out uint ui, name)) return (int)ui;
        if (s.TryGetValue(out sbyte sb, name)) return sb;
        return fallback;
    }

    static string ClassOf(FStructFallback node, string property) => node.TryGetValue(out FPackageIndex idx, property) && idx != null && !idx.IsNull ? idx.Name : "";

    static UObject? InstanceOf(FStructFallback node, FStructFallback[] instances)
    {
        if (!node.TryGetValue(out FStructFallback at, "InstanceTemplateIndex")) return null;
        int i = IntOf(at, "Value");
        if (i < 0 || i >= instances.Length || instances[i] == null) return null;
        return instances[i].TryGetValue(out FPackageIndex obj, "InstanceObject") && obj != null && !obj.IsNull ? Lv.Resolve(obj) : null;
    }

    static string NameOf(UObject o, string property) => o.TryGetValue(out FName n, property) ? n.Text : o.TryGetValue(out string s, property) ? s : "";

    static float CooldownOf(TreeInfo info, FStructFallback node, UObject task, Dictionary<string, object> prm)
    {
        if (BindingsOf(info, node).TryGetValue("Cooldown", out var source) && source.Kind == "Parameter" && prm.TryGetValue(source.Path, out var value) && NumberOf(value) is { } seconds) return (float)seconds;
        return task.TryGetValue(out float own, "Cooldown") ? own : 0f;
    }

    static readonly Dictionary<long, string> Waiting = new()
    {
        [6064631901149396992] = "waits for the spray hit shape (Flame Burst)",
        [6064629308658155610] = "waits for channelled attacks aimed by score (Flame Throw)",
        [6064629308660187227] = "waits for ground zones of a creature (Flame Ring)",
    };

    static float ArrivalOf(TreeInfo info, List<int> path)
    {
        int combat = path.FindIndex(p => string.Equals(TextOf(info.States[p], "Name"), "Combat", StringComparison.OrdinalIgnoreCase));
        if (combat < 0) return -1f;
        int combatState = path[combat];
        for (int si = 0; si < info.States.Length; si++)
        {
            if (!(info.States[si].TryGetValue(out FStructFallback par, "Parent") && IntOf(par, "Index") == combatState)) continue;
            if (info.States[si].TryGetValue(out bool stateEnabled, "bEnabled") && !stateEnabled) continue;
            int tb = IntOf(info.States[si], "TasksBegin", 0), tn = IntOf(info.States[si], "TasksNum", 0);
            for (int k = tb; k < tb + tn && k < info.Nodes.Length; k++)
            {
                if (info.Nodes[k] == null || ClassOf(info.Nodes[k], "TaskClass") != "AoCSTT_MoveToTarget") continue;
                if (info.Nodes[k].TryGetValue(out bool taskEnabled, "bTaskEnabled") && !taskEnabled) continue;
                var task = InstanceOf(info.Nodes[k], info.Defaults);
                if (task != null && NumberOf(task.Properties.FirstOrDefault(p => p.Name.Text == "ArrivalDistance")?.Tag?.GenericValue) is { } distance && distance > 0) return (float)distance;
            }
        }
        return -1f;
    }

    static List<int> StatePathOf(FStructFallback[] states, int si)
    {
        var path = new List<int>();
        for (int p = si, guard = 0; p >= 0 && p < states.Length && guard < 64; guard++)
        {
            path.Insert(0, p);
            p = states[p].TryGetValue(out FStructFallback par, "Parent") ? IntOf(par, "Index") : -1;
        }
        return path;
    }

    static void CollectPathTaskStarts(TreeInfo info, List<int> path, int taskNodeIdx, Dictionary<string, object> prm, AbilityRow row)
    {
        var starts = new List<string>();
        foreach (int ps in path)
        {
            int tb = IntOf(info.States[ps], "TasksBegin", 0), tn = IntOf(info.States[ps], "TasksNum", 0);
            for (int j = tb; j < tb + tn && j < info.Nodes.Length; j++)
            {
                if (j == taskNodeIdx || info.Nodes[j] == null) continue;
                if (info.Nodes[j].TryGetValue(out bool otherEnabled, "bTaskEnabled") && !otherEnabled) continue;
                var other = InstanceOf(info.Nodes[j], info.Defaults);
                if (other == null) continue;
                if (string.Equals(ClassOf(info.Nodes[j], "TaskClass"), "AocSTT_RotateTo", StringComparison.OrdinalIgnoreCase)
                    && (!other.TryGetValue(out FName rotationType, "RotationType") || rotationType.Text.EndsWith("RotateToTarget", StringComparison.Ordinal)))
                {
                    float speed = other.TryGetValue(out float rotationSpeed, "RotationSpeed") ? rotationSpeed : -1f;
                    float duration = other.TryGetValue(out float rotationDuration, "RotationDuration") ? rotationDuration : 0f;
                    row.Rotate = F(speed) + ":" + F(duration);
                }
                string identifier = NameOf(other, "TaskIdentifier");
                float seconds = CooldownOf(info, info.Nodes[j], other, prm);
                if (identifier.Length > 0 && seconds > 0f && identifier != row.Task && !identifier.Contains(';') && !identifier.Contains('='))
                    starts.Add(identifier + "=" + F(seconds));
            }
        }
        if (row.Fight)
        {
            foreach (var (accumulator, low, high) in ConsumedAfter(info, path))
            {
                string entry = "acc." + accumulator + "=" + (low == high ? Num(low) : Num(low) + "~" + Num(high));
                if (!starts.Contains(entry)) starts.Add(entry);
            }
            row.Arrival = ArrivalOf(info, path);
        }
        row.Starts = string.Join(";", starts.Distinct());
    }

    static void ApplyPlainConditions(
        List<(string Operand, int Delta, Atom Atom)> tokens,
        AbilityRow row,
        List<string> gates,
        List<string> groups,
        ref bool off)
    {
        var rest = new List<string>();
        foreach (var (_, _, atom) in tokens)
        {
            if (atom.Unknown) continue;
            switch (atom.Kind)
            {
                case "dist":
                    if (atom.Inverse) row.MinDistance = Math.Max(row.MinDistance, (float)atom.Value);
                    else row.MaxDistance = row.MaxDistance < 0 ? (float)atom.Value : Math.Min(row.MaxDistance, (float)atom.Value);
                    break;
                case "ang":
                    row.MaxAngle = row.MaxAngle < 0 ? (float)atom.Value : Math.Min(row.MaxAngle, (float)atom.Value);
                    break;
                case "cd":
                    gates.Add(atom.Name);
                    break;
                default:
                    if (atom.Constant == "F") off = true;
                    else if (atom.Constant != "T") rest.Add(atom.Text);
                    break;
            }
        }
        if (rest.Count > 0)
            groups.Add(string.Join(" ", rest.Select((a, i) => (i == 0 ? "c0:" : "a0:") + a)));
    }

    static void CollectPathConditions(TreeInfo info, List<int> path, Dictionary<string, object> prm, AbilityRow row)
    {
        var onPath = new HashSet<int>(path);
        var gates = new List<string>();
        var groups = new List<string>();
        var why = new SortedSet<string>(StringComparer.Ordinal);
        bool off = false;
        foreach (int ps in path)
        {
            int cb = IntOf(info.States[ps], "EnterConditionsBegin", 0), cn = IntOf(info.States[ps], "EnterConditionsNum", 0);
            if (cn <= 0) continue;
            var tokens = new List<(string Operand, int Delta, Atom Atom)>();
            for (int c = cb; c < cb + cn && c < info.Nodes.Length; c++)
            {
                string operand = tokens.Count == 0 ? "Copy" : TextOf(info.Nodes[c], "Operand");
                tokens.Add((operand, IntOf(info.Nodes[c], "DeltaIndent", 0), Encode(info, info.Nodes[c], onPath, prm)));
            }
            foreach (var tk in tokens)
                if (tk.Atom.Unknown) { row.Unknown = true; why.Add(Uri.UnescapeDataString(tk.Atom.Text.Substring(1))); }
            bool plain = tokens.Skip(1).All(x => x.Operand == "And") && tokens.All(x => x.Delta == 0);
            if (plain)
                ApplyPlainConditions(tokens, row, gates, groups, ref off);
            else
                groups.Add(string.Join(" ", tokens.Select(x => (x.Operand == "Or" ? "o" : x.Operand == "And" ? "a" : "c") + (x.Delta > 0 ? "+" : "") + x.Delta.ToString(CultureInfo.InvariantCulture) + ":" + x.Atom.Text)));
        }
        if (Waiting.TryGetValue(row.Ability, out var waits)) { row.Unknown = true; why.Add(waits); }
        row.Gates = string.Join(";", gates);
        row.Conds = string.Join("|", groups);
        row.Why = row.Unknown ? string.Join(",", why) : off ? "off" : "";
    }

    public static List<AbilityRow> UseAbilityRows(UObject tree, Dictionary<string, object> prm)
    {
        var rows = new List<AbilityRow>();
        var info = LoadTree(tree);
        for (int si = 0; si < info.States.Length; si++)
        {
            int tasksBegin = IntOf(info.States[si], "TasksBegin", 0), tasksNum = IntOf(info.States[si], "TasksNum", 0);
            for (int k = tasksBegin; k < tasksBegin + tasksNum && k < info.Nodes.Length; k++)
            {
                if (info.Nodes[k] == null || ClassOf(info.Nodes[k], "TaskClass") != "AoCSTT_UseAbility") continue;
                if (info.Nodes[k].TryGetValue(out bool taskEnabled, "bTaskEnabled") && !taskEnabled) continue;
                var t = InstanceOf(info.Nodes[k], info.Defaults);
                if (t == null || !t.TryGetValue(out FStructFallback id, "AbilityId") || id == null || GuidOf(id) == 0) continue;
                var path = StatePathOf(info.States, si);
                if (path.Any(ps => info.States[ps].TryGetValue(out bool enabled, "bEnabled") && !enabled)) continue;
                var row = new AbilityRow
                {
                    Ability = GuidOf(id), Order = si, Task = NameOf(t, "TaskIdentifier"),
                    Cooldown = CooldownOf(info, info.Nodes[k], t, prm),
                    StopMovement = t.TryGetValue(out bool sm, "bShouldStopMovement") && sm,
                    Fight = InFight(info, path),
                };
                CollectPathTaskStarts(info, path, k, prm, row);
                CollectPathConditions(info, path, prm, row);
                rows.Add(row);
            }
        }
        return rows;
    }

    static void ReadMontage(UObject e, string containerPath, List<MontageRow> rows, bool exportAnimEnd)
    {
        var pkgPath = PackagePathOf(containerPath);
        var r = new MontageRow { Package = pkgPath, Montage = pkgPath + "." + e.Name };
        r.Guid = StaticId("AnimMontage", r.Montage);
        r.PackageGuid = StaticId("Package", pkgPath);
        if (e.TryGetValue(out float len, "SequenceLength")) r.Length = len;
        if (e.TryGetValue(out float rate, "RateScale") && rate != 0f) r.RateScale = rate;
        foreach (var s in StructsOf(e, "CompositeSections"))
        {
            string name = s.TryGetValue(out FName n, "SectionName") ? n.Text : "";
            string next = s.TryGetValue(out FName nx, "NextSectionName") ? nx.Text : "None";
            float start = s.TryGetValue(out float lv, "LinkValue") ? lv : 0f;
            r.Sections.Add((name, start, next));
        }
        foreach (var nt in StructsOf(e, "Notifies"))
        {
            if (!nt.TryGetValue(out FPackageIndex idx, "Notify") || idx == null || idx.IsNull) continue;
            var obj = Lv.Resolve(idx);
            if (obj == null) continue;
            float time = nt.TryGetValue(out float lv, "LinkValue") ? lv : 0f;
            if (nt.TryGetValue(out float off, "TriggerTimeOffset")) time += off;
            if (obj.ExportType == "AoCAnimNotify_AnimEnd")
            {
                if (exportAnimEnd && (r.AnimEnd < 0f || time < r.AnimEnd)) r.AnimEnd = time;
                continue;
            }
            if (obj.ExportType == "AoCAnimNotify_FireProjectile")
            {
                r.Fires.Add((time, obj.TryGetValue(out int pk, "ProjectileKey") ? pk : 0));
                continue;
            }
            if (obj.ExportType != "AoCAnimNotify_AbilityHit") continue;
            r.Hits.Add((time, obj.TryGetValue(out int hk, "HitKey") ? hk : 0));
        }
        r.Hits.Sort((a, b) => a.Time != b.Time ? a.Time.CompareTo(b.Time) : a.Key.CompareTo(b.Key));
        r.Fires.Sort((a, b) => a.Time != b.Time ? a.Time.CompareTo(b.Time) : a.Key.CompareTo(b.Key));
        r.RootMotion = ReadRootMotion(e, r.Montage, out bool unreadable);
        r.RootUnreadable = unreadable;
        rows.Add(r);
    }

    static void CollectPackage(CollectState state, string path)
    {
        state.Output.Packages++;
        IPackage? pkg;
        try { pkg = state.Game.Provider.LoadPackage(path); } catch { state.Output.Failed++; return; }
        if (pkg == null) { state.Output.Failed++; return; }
        var exports = pkg.GetExports().ToList();
        bool endHere = state.EndDirs.Any(d => path.StartsWith(d, StringComparison.OrdinalIgnoreCase));
        foreach (var e in exports.Where(x => x.ExportType == "AnimMontage"))
            ReadMontage(e, path, state.Output.Montages, endHere);
        var cdo = exports.FirstOrDefault(x => x.Name.StartsWith("Default__", StringComparison.Ordinal) && x.Name.EndsWith("_C", StringComparison.Ordinal));
        if (cdo == null) return;
        var chain = CdoChain(cdo);
        if (!chain.Any(c => ClassName(c) == "NPCPawn_C")) return;
        var tree = StateTreeOf(state.Game, chain, out _);
        var treeObject = tree?.GetExports().FirstOrDefault(x => x.ExportType == "StateTree");
        if (tree == null || treeObject == null) return;
        string archetype = PackagePathOf(path) + "." + cdo.Name;
        string parametersPackage = ParametersPackageOf(chain);
        var prm = Parameters.Effective(
            state.BagOf(ContainerPathOf(tree.Name), true),
            parametersPackage.Length > 0 ? state.BagOf(ContainerPathOf(parametersPackage), false) : null);
        string signature = tree.Name + "|" + string.Join(";", prm.OrderBy(x => x.Key, StringComparer.Ordinal).Select(x => x.Key + "=" + (x.Value is double d ? d.ToString("R", CultureInfo.InvariantCulture) : x.Value.ToString())));
        if (!state.TreeCache.TryGetValue(signature, out var treeRows))
            state.TreeCache[signature] = treeRows = UseAbilityRows(treeObject, prm);
        foreach (var r in treeRows)
        {
            state.Output.Abilities.Add(new AbilityRow
            {
                Archetype = archetype, Ability = r.Ability, Task = r.Task, Gates = r.Gates, Cooldown = r.Cooldown, StopMovement = r.StopMovement, Unknown = r.Unknown, Order = r.Order,
                MinDistance = r.MinDistance, MaxDistance = r.MaxDistance, MaxAngle = r.MaxAngle, Fight = r.Fight, Conds = r.Conds, Why = r.Why, Starts = r.Starts, Rotate = r.Rotate, Arrival = r.Arrival,
            });
        }
    }

    public static CollectResult Collect(Game g, IEnumerable<string> folders, IEnumerable<string> animEndFolders)
    {
        var dirs = folders.Select(f => f.Replace('\\', '/').TrimEnd('/') + "/").ToList();
        var endDirs = animEndFolders.Select(f => f.Replace('\\', '/').TrimEnd('/') + "/").ToList();
        var paths = g.Provider.Files.Keys
            .Where(k => k.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) && dirs.Any(d => k.StartsWith(d, StringComparison.OrdinalIgnoreCase)))
            .OrderBy(k => k, StringComparer.Ordinal)
            .ToList();
        var state = new CollectState { Game = g, EndDirs = endDirs };
        foreach (var path in paths)
            CollectPackage(state, path);
        state.Output.Abilities = state.Output.Abilities.OrderBy(a => a.Archetype, StringComparer.Ordinal).ThenBy(a => a.Order).ThenBy(a => a.Ability).ToList();
        state.Output.Montages = state.Output.Montages.OrderBy(m => m.Montage, StringComparer.Ordinal).ToList();
        return state.Output;
    }

    public static void Collect(Game g, IEnumerable<string> folders, IEnumerable<string> animEndFolders, out List<AbilityRow> abilities, out List<MontageRow> montages, out int packages, out int failed)
    {
        var res = Collect(g, folders, animEndFolders);
        abilities = res.Abilities;
        montages = res.Montages;
        packages = res.Packages;
        failed = res.Failed;
    }

    static string F(float v) => v.ToString("R", CultureInfo.InvariantCulture);

    public static string AbilitiesTsv(IEnumerable<AbilityRow> rows)
    {
        var sb = new StringBuilder();
        sb.Append(AbilitiesHeader).Append('\n');
        foreach (var r in rows)
            sb.Append(r.Archetype).Append('\t').Append(r.Ability.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(r.Task).Append('\t').Append(F(r.Cooldown)).Append('\t').Append(r.StopMovement ? "1" : "0").Append('\t')
              .Append(r.Order.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(F(r.MinDistance)).Append('\t').Append(F(r.MaxDistance)).Append('\t').Append(F(r.MaxAngle)).Append('\t')
              .Append(r.Gates).Append('\t').Append(r.Unknown ? "1" : "0").Append('\t').Append(r.Fight ? "1" : "0").Append('\t').Append(r.Conds).Append('\t').Append(r.Why).Append('\t').Append(r.Starts).Append('\t').Append(r.Rotate).Append('\t').Append(F(r.Arrival)).Append('\n');
        return sb.ToString();
    }

    public static string MontagesTsv(IEnumerable<MontageRow> rows)
    {
        var sb = new StringBuilder();
        sb.Append(MontagesHeader).Append('\n');
        foreach (var r in rows)
        {
            sb.Append(r.Montage).Append('\t').Append(r.Guid.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(r.Package).Append('\t').Append(r.PackageGuid.ToString(CultureInfo.InvariantCulture)).Append('\t')
              .Append(F(r.Length)).Append('\t').Append(F(r.RateScale)).Append('\t')
              .Append(string.Join(";", r.Sections.Select(s => s.Name + ":" + F(s.Start) + ":" + s.Next))).Append('\t')
              .Append(string.Join(";", r.Hits.Select(h => F(h.Time) + ":" + h.Key.ToString(CultureInfo.InvariantCulture)))).Append('\t')
              .Append(string.Join(";", r.RootMotion.Select(m => F(m.Time) + ":" + F(m.X) + ":" + F(m.Y) + ":" + F(m.Z)))).Append('\t')
              .Append(r.RootUnreadable ? "1" : "0").Append('\t')
              .Append(r.AnimEnd >= 0f ? F(r.AnimEnd) : "").Append('\t')
              .Append(string.Join(";", r.Fires.Select(f => F(f.Time) + ":" + f.Key.ToString(CultureInfo.InvariantCulture)))).Append('\n');
        }
        return sb.ToString();
    }
}
