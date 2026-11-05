# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:TooltipCSharpSource = @'
using System; using System.Collections.Generic; using System.Globalization; using System.IO; using System.IO.Compression;
using System.Linq; using System.Runtime.InteropServices; using System.Text; using System.Text.Json.Nodes; using System.Text.RegularExpressions;

namespace SandalCore.Tools.Tooltips {
    internal static class NativeSqlite {
        static NativeSqlite() => NativeLibrary.SetDllImportResolver(typeof(NativeSqlite).Assembly, ResolveLibrary);
        private static IntPtr ResolveLibrary(string name, System.Reflection.Assembly asm, DllImportSearchPath? path) {
            if (name != "sqlite3") return IntPtr.Zero;
            foreach (var c in new[] { "winsqlite3.dll", "sqlite3", "libsqlite3.so.0", "libsqlite3.so", "e_sqlite3" }) if (NativeLibrary.TryLoad(c, out var h)) return h;
            return IntPtr.Zero;
        }
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern int sqlite3_open_v2(byte[] filename, out IntPtr db, int flags, IntPtr zVfs);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern int sqlite3_close_v2(IntPtr db);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern int sqlite3_prepare_v2(IntPtr db, byte[] zSql, int nByte, out IntPtr stmt, IntPtr pzTail);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern int sqlite3_step(IntPtr stmt);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern int sqlite3_reset(IntPtr stmt);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern int sqlite3_finalize(IntPtr stmt);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern int sqlite3_bind_int64(IntPtr stmt, int idx, long val);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern long sqlite3_column_int64(IntPtr stmt, int iCol);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr sqlite3_column_text(IntPtr stmt, int iCol);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr sqlite3_column_blob(IntPtr stmt, int iCol);
        [DllImport("sqlite3", CallingConvention = CallingConvention.Cdecl)] internal static extern int sqlite3_column_bytes(IntPtr stmt, int iCol);
        internal static IntPtr Prepare(IntPtr db, string sql) {
            var b = Encoding.UTF8.GetBytes(sql + "\0");
            if (sqlite3_prepare_v2(db, b, b.Length - 1, out var stmt, IntPtr.Zero) != 0) throw new InvalidOperationException("SQL error: " + sql);
            return stmt;
        }
        internal static string GetText(IntPtr s, int c) => sqlite3_column_text(s, c) is IntPtr p && p != IntPtr.Zero ? (Marshal.PtrToStringUTF8(p) ?? "") : "";
    }
    internal sealed class UnresolvedException : Exception { public UnresolvedException(string msg) : base(msg) { } }
    internal sealed class RefVal : IEquatable<RefVal> {
        public readonly string Kind, RefSchema, RefName; public readonly long TypeId, Guid;
        public RefVal(string kind, long typeId, long guid, string refSchema = "", string refName = "") { Kind = kind; TypeId = typeId; Guid = guid; RefSchema = refSchema; RefName = refName; }
        public bool Equals(RefVal o) => o != null && Kind == o.Kind && TypeId == o.TypeId && Guid == o.Guid && RefSchema == o.RefSchema && RefName == o.RefName;
        public override bool Equals(object obj) => obj is RefVal r && Equals(r);
        public override int GetHashCode() => HashCode.Combine(Kind, TypeId, Guid, RefSchema, RefName);
    }
    internal sealed class LazyTerm { public readonly EvalEnv Env; public readonly string Src; public LazyTerm(EvalEnv e, string s) { Env = e; Src = s; } }
    internal sealed class EvalEnv {
        public readonly TooltipDb Db; public readonly int Level; public readonly string Skills; public readonly Dictionary<long, object> Terms; public int Depth;
        public EvalEnv(TooltipDb db, int level, string skills, Dictionary<long, object> terms = null, int depth = 0) {
            Db = db; Level = level; Skills = skills; Terms = terms != null ? new Dictionary<long, object>(terms) : new Dictionary<long, object>(); Depth = depth;
        }
    }
    internal static class JsonExt {
        public static string S(this JsonNode n, string k) => n?[k]?.GetValue<string>() ?? "";
        public static string SourceStr(this JsonNode n, string k) => n?[k] is JsonObject o ? (o["Source"]?.GetValue<string>() ?? "") : (n?[k] is JsonValue v && v.TryGetValue<string>(out var s) ? (s ?? "") : "");
        public static long L(this JsonNode n, string k) => n?[k] != null ? n[k].GetValue<long>() : 0L;
        public static double D(this JsonNode n, string k) => n?[k] != null ? n[k].GetValue<double>() : 0.0;
        public static bool B(this JsonNode n, string k) => n?[k] != null && n[k].GetValue<bool>();
        public static JsonArray Arr(this JsonNode n, string k) => n?[k] as JsonArray ?? new JsonArray();
    }
    internal sealed class TooltipDb : IDisposable {
        public const string TAbility = "AoCAbilityRecordBase", THit = "AbilityHitRecordBase", TEffect = "EffectRecordBase";
        public const string TStatMod = "StatModRecordBase", TFormula = "StatFormulaTypeRecordBase", TEquation = "StatEquationTypeRecordBase", TCurve = "StatCurveRecordBase", TTerm = "TermRecordBase";
        public const long TermBase = 6064631035951644675L, TermMin = 6064631120485154816L, TermMax = 6064631120485482497L, TermAdd = 6064631210309517324L;
        public static readonly Dictionary<long, string> TypeSchema = new() {
            [1841048597116038327L] = TAbility, [3672506661632452365L] = THit, [3377240744094973029L] = TEffect, [676208982154769454L] = TStatMod,
            [4547065559311335368L] = TFormula, [7180619218012719533L] = TEquation, [7037248826340794574L] = TCurve, [6137747221710071900L] = TTerm,
            [979581256175327151L] = "ProjectileRecordBase", [7793593157135267051L] = "LingeringEffectRecordBase", [3999912890578843546L] = "AbilityTargetingRecordBase",
            [4146438674247114715L] = "SkillRecordBase", [297887639323616934L] = "SkillTreeRecordBase", [5365104778484440215L] = "StatTypeDefRecordBase",
        };
        public static readonly Dictionary<string, long> SchemaType = TypeSchema.ToDictionary(kv => kv.Value, kv => kv.Key);
        internal IntPtr Handle; private IntPtr _recStmt;
        private readonly Dictionary<(string, long), JsonObject> _cache = new();
        private readonly Dictionary<string, Dictionary<string, long>> _names = new();
        public TooltipDb(string dbPath) {
            if (NativeSqlite.sqlite3_open_v2(Encoding.UTF8.GetBytes(dbPath + "\0"), out Handle, 1, IntPtr.Zero) != 0) throw new InvalidOperationException("Cannot open " + dbPath);
            _recStmt = NativeSqlite.Prepare(Handle, "select data,name from record where type_id=? and guid=?");
            var stmt = NativeSqlite.Prepare(Handle, "select schema,name,guid from record where schema in (" + string.Join(",", SchemaType.Keys.Select(s => "'" + s + "'")) + ")");
            try {
                while (NativeSqlite.sqlite3_step(stmt) == 100) {
                    string sch = NativeSqlite.GetText(stmt, 0), nm = NativeSqlite.GetText(stmt, 1); long g = NativeSqlite.sqlite3_column_int64(stmt, 2);
                    if (!_names.TryGetValue(sch, out var d)) _names[sch] = d = new Dictionary<string, long>(StringComparer.Ordinal);
                    d.TryAdd(nm, g); d.TryAdd(nm.ToLowerInvariant(), g);
                }
            } finally { NativeSqlite.sqlite3_finalize(stmt); }
        }
        public JsonObject Rec(string schema, long guid) {
            if (schema == null || !SchemaType.TryGetValue(schema, out var tid)) return null;
            if (_cache.TryGetValue((schema, guid), out var cached)) return cached;
            NativeSqlite.sqlite3_reset(_recStmt); NativeSqlite.sqlite3_bind_int64(_recStmt, 1, tid); NativeSqlite.sqlite3_bind_int64(_recStmt, 2, guid);
            return _cache[(schema, guid)] = NativeSqlite.sqlite3_step(_recStmt) == 100 ? DecodeBlob(_recStmt, guid) : null;
        }
        private static JsonObject DecodeBlob(IntPtr stmt, long guid) {
            var buf = new byte[NativeSqlite.sqlite3_column_bytes(stmt, 0)];
            Marshal.Copy(NativeSqlite.sqlite3_column_blob(stmt, 0), buf, 0, buf.Length);
            using var gz = new GZipStream(new MemoryStream(buf), CompressionMode.Decompress);
            var obj = JsonNode.Parse(gz).AsObject(); obj["__name"] = NativeSqlite.GetText(stmt, 1); obj["__guid"] = guid;
            return obj;
        }
        public JsonObject Ref(JsonNode r) => (r != null && r.L("Guid") is long g && g != 0 && TypeSchema.TryGetValue(r.L("TypeId"), out var sch)) ? Rec(sch, g) : null;
        public JsonObject ByName(string s, string n) => (n != null && _names.TryGetValue(s, out var d) && (d.TryGetValue(n, out var g) || d.TryGetValue(n.ToLowerInvariant(), out g))) ? Rec(s, g) : null;
        public string StatName(long g) => Rec("StatTypeDefRecordBase", g)?.S("__name") ?? g.ToString(CultureInfo.InvariantCulture);
        public string StatDisplay(long g) => Rec("StatTypeDefRecordBase", g) is JsonObject r ? (r.SourceStr("DisplayName") is string dn && dn.Length > 0 ? dn : r.S("__name")) : g.ToString(CultureInfo.InvariantCulture);
        public void Dispose() {
            if (_recStmt != IntPtr.Zero) { NativeSqlite.sqlite3_finalize(_recStmt); _recStmt = IntPtr.Zero; }
            if (Handle != IntPtr.Zero) { NativeSqlite.sqlite3_close_v2(Handle); Handle = IntPtr.Zero; }
        }
    }
    internal readonly struct Tok {
        public readonly string Kind, Text; public readonly double Num; public readonly RefVal Ref;
        public Tok(string k, string t = "", double n = 0, RefVal r = null) { Kind = k; Text = t; Num = n; Ref = r; }
    }
    internal static class ExprLexer {
        private static readonly string[] Punct = { "&&", "||", "==", "!=", "<=", ">=", "+", "-", "*", "/", "%", "<", ">", "!", "=", "(", ")", "{", "}", ",", ";" };
        public static List<Tok> Lex(string src) {
            var res = new List<Tok>(); int i = 0, n = src.Length;
            while (i < n) {
                int j = SkipWs(src, i, n); if (j > i) { i = j; continue; }
                var (t, next) = LexOne(src, i, n); res.Add(t); i = next;
            }
            res.Add(new Tok("eof")); return res;
        }
        private static int SkipWs(string s, int i, int n) {
            if (" \t\r\n".IndexOf(s[i]) >= 0) return i + 1;
            if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; return i; }
            if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') { int k = s.IndexOf("*/", i + 2, StringComparison.Ordinal); return k < 0 ? n : k + 2; }
            return i;
        }
        private static (Tok, int) LexOne(string s, int i, int n) {
            char c = s[i];
            if (char.IsDigit(c) || (c == '.' && i + 1 < n && char.IsDigit(s[i + 1]))) {
                int j = i; while (j < n && char.IsDigit(s[j])) j++;
                if (j < n && s[j] == '.') { j++; while (j < n && char.IsDigit(s[j])) j++; }
                return (new Tok("num", "", double.Parse(s.Substring(i, j - i), CultureInfo.InvariantCulture)), j);
            }
            if (char.IsLetter(c) || c == '_') return LexIdent(s, i, n);
            if (c == '$') return LexDollar(s, i);
            if (c == '"' || c == '\'') {
                int j = i + 1; var sb = new StringBuilder();
                while (j < n && s[j] != c) { if (s[j] == '\\') j++; sb.Append(s[j++]); }
                return (new Tok("str", sb.ToString()), j + 1);
            }
            foreach (var p in Punct) if (string.CompareOrdinal(s, i, p, 0, p.Length) == 0) return (new Tok("p", p), i + p.Length);
            throw new UnresolvedException("lex " + c);
        }
        private static (Tok, int) LexIdent(string s, int i, int n) {
            int j = i, last = i;
            while (j < n && (char.IsLetterOrDigit(s[j]) || s[j] == '_')) j++;
            while (j + 2 < n && s[j] == ':' && s[j + 1] == ':' && (char.IsLetter(s[j + 2]) || s[j + 2] == '_')) {
                last = j + 2; j += 2; while (j < n && (char.IsLetterOrDigit(s[j]) || s[j] == '_')) j++;
            }
            return last != i ? (new Tok("enum", s.Substring(i, last - 2 - i) + "::" + s.Substring(last, j - last)), j) : (new Tok("id", s.Substring(i, j - i)), j);
        }
        private static (Tok, int) LexDollar(string s, int i) {
            int j = s.IndexOf('$', i + 1); string b = s.Substring(i + 1, j - i - 1);
            if (b.StartsWith("#", StringComparison.Ordinal)) {
                var p = b.Substring(1).Split(':');
                return (new Tok("ref", "", 0, new RefVal("ref", long.Parse(p[0], CultureInfo.InvariantCulture), long.Parse(p[1], CultureInfo.InvariantCulture))), j + 1);
            }
            int col = b.IndexOf(':');
            return (new Tok("nref", "", 0, new RefVal("nref", 0, 0, b.Substring(0, col), b.Substring(col + 1))), j + 1);
        }
    }
    internal sealed class AstNode { public string Kind, Text; public object Val; public AstNode A, B; public List<AstNode> ListA, ListB; }
    internal sealed class ExprParser {
        private static readonly Dictionary<string, int> BinPrec = new() { ["||"] = 1, ["&&"] = 2, ["=="] = 3, ["!="] = 3, ["<"] = 4, ["<="] = 4, [">"] = 4, [">="] = 4, ["+"] = 5, ["-"] = 5, ["*"] = 6, ["/"] = 6, ["%"] = 6 };
        private readonly List<Tok> _t; private int _i;
        public ExprParser(List<Tok> toks) { _t = toks; }
        private Tok Peek() => _t[_i]; private Tok Nxt() => _t[_i++];
        private bool IsPunct(string p) => _t[_i].Kind == "p" && _t[_i].Text == p;
        private void Expect(string p) { if (!IsPunct(p)) throw new UnresolvedException("expected " + p); _i++; }
        public (List<AstNode>, AstNode) Block(bool braced) {
            var stmts = new List<AstNode>(); AstNode res = null;
            while (true) {
                if (Peek().Kind == "eof") return (stmts, res);
                if (braced && IsPunct("}")) { _i++; return (stmts, res); }
                if (res != null) throw new UnresolvedException("after final expr");
                var (s, e) = Statement(); if (e != null) res = e; else stmts.Add(s);
            }
        }
        private (AstNode, AstNode) Statement() {
            var t = Peek();
            if (t.Kind == "id" && (t.Text == "const" || t.Text == "var" || t.Text == "set")) {
                _i++; string n = Nxt().Text.ToLowerInvariant(); Expect("="); var v = Expr(0); Expect(";");
                return (new AstNode { Kind = t.Text == "set" ? "set" : "decl", Text = n, A = v }, null);
            }
            if (t.Kind == "id" && t.Text == "if") return (IfStmt(), null);
            if (t.Kind == "id" && t.Text == "return") { _i++; var rv = Expr(0); if (IsPunct(";")) _i++; return (null, rv); }
            var x = Expr(0); if (IsPunct(";")) { _i++; return (new AstNode { Kind = "expr", A = x }, null); }
            return (null, x);
        }
        private AstNode IfStmt() {
            _i++; Expect("("); var c = Expr(0); Expect(")"); Expect("{"); var (th, _) = Block(true); var el = new List<AstNode>();
            if (Peek().Kind == "id" && Peek().Text == "else") {
                _i++; if (Peek().Kind == "id" && Peek().Text == "if") el.Add(IfStmt()); else { Expect("{"); (el, _) = Block(true); }
            }
            return new AstNode { Kind = "if", A = c, ListA = th, ListB = el };
        }
        public AstNode Expr(int mn) {
            var left = (Peek().Kind == "p" && (Peek().Text == "-" || Peek().Text == "!")) ? new AstNode { Kind = "un", Text = Nxt().Text, A = Expr(6) } : Primary();
            while (Peek().Kind == "p" && BinPrec.TryGetValue(Peek().Text, out int prec) && prec > mn) {
                string op = Nxt().Text; left = new AstNode { Kind = "bin", Text = op, A = left, B = Expr(prec) };
            }
            return left;
        }
        private AstNode Primary() {
            if (Peek().Kind == "p" && (Peek().Text == "-" || Peek().Text == "!")) return new AstNode { Kind = "un", Text = Nxt().Text, A = Primary() };
            var t = Nxt();
            if (t.Kind == "num") return new AstNode { Kind = "lit", Val = t.Num };
            if (t.Kind == "str" || t.Kind == "enum") return new AstNode { Kind = "lit", Val = t.Text };
            if (t.Kind == "ref" || t.Kind == "nref") return new AstNode { Kind = "lit", Val = t.Ref };
            if (t.Kind == "p" && t.Text == "(") { var x = Expr(0); Expect(")"); return x; }
            if (t.Kind != "id") throw new UnresolvedException("unexpected " + t.Kind);
            if (t.Text == "true" || t.Text == "false") return new AstNode { Kind = "lit", Val = (t.Text == "true") };
            if (!IsPunct("(")) return new AstNode { Kind = "var", Text = t.Text.ToLowerInvariant() };
            _i++; var args = new List<AstNode>();
            while (!IsPunct(")")) { if (args.Count > 0) Expect(","); args.Add(Expr(0)); }
            _i++; return new AstNode { Kind = "call", Text = t.Text.ToLowerInvariant(), ListA = args };
        }
    }
    internal static class ExprEval {
        private static readonly object SelfObj = new(), TargetObj = new();
        private static readonly Dictionary<string, object> ConstCalls = new() {
            ["getinstigator"] = SelfObj, ["getowner"] = SelfObj, ["getsource"] = SelfObj, ["getplayer"] = SelfObj, ["getself"] = SelfObj, ["getcreatureowner"] = SelfObj,
            ["gettarget"] = TargetObj, ["getactivetarget"] = TargetObj, ["gethitinprogress"] = null, ["gethitrecord"] = null, ["gethitsourcerecord"] = null,
            ["getabilityprogressratio"] = 0.0, ["hithastag"] = false, ["getrecordtypeid"] = 0.0, ["getprojectiletypeid"] = 1.0,
            ["getcurrentweaponskilltree"] = new RefVal("ref", 297887639323616934L, 0L), ["geteffectcount"] = 0.0, ["geteffectcountbytag"] = 0.0, ["geteffectcountbyrecordandinstigator"] = 0.0,
        };
        public static double Num(object v) => v switch { double d => d, bool b => b ? 1.0 : 0.0, int i => i, long l => l, _ => throw new UnresolvedException("not a number") };
        private static bool Truthy(object v) => v switch { null => false, bool b => b, double d => d != 0.0, string s => s.Length > 0, _ => true };
        public static object Run(EvalEnv env, string src, bool lazy = false) {
            var e = lazy ? new EvalEnv(env.Db, env.Level, env.Skills, env.Terms, env.Depth) : env;
            var (stmts, res) = new ExprParser(ExprLexer.Lex(src)).Block(false);
            if (res == null && stmts.Count > 0 && stmts[stmts.Count - 1].Kind == "expr") { res = stmts[stmts.Count - 1].A; stmts.RemoveAt(stmts.Count - 1); }
            if (res == null) throw new UnresolvedException("no value");
            var sc = new List<Dictionary<string, object>> { new() }; ExecStmts(e, stmts, sc); return Ev(e, res, sc);
        }
        private static void ExecStmts(EvalEnv env, List<AstNode> lst, List<Dictionary<string, object>> sc) {
            foreach (var s in lst) {
                if (s.Kind == "decl") sc[sc.Count - 1][s.Text] = Ev(env, s.A, sc);
                else if (s.Kind == "set") { for (int i = sc.Count - 1; i >= 0; i--) if (sc[i].ContainsKey(s.Text)) { sc[i][s.Text] = Ev(env, s.A, sc); break; } }
                else if (s.Kind == "if") { sc.Add(new()); ExecStmts(env, Truthy(Ev(env, s.A, sc)) ? s.ListA : s.ListB, sc); sc.RemoveAt(sc.Count - 1); }
                else if (s.Kind == "expr") Ev(env, s.A, sc);
            }
        }
        private static object Ev(EvalEnv env, AstNode n, List<Dictionary<string, object>> sc) {
            if (n.Kind == "lit") return n.Val;
            if (n.Kind == "var") { for (int i = sc.Count - 1; i >= 0; i--) if (sc[i].TryGetValue(n.Text, out var v)) return v; throw new UnresolvedException("var " + n.Text); }
            if (n.Kind == "un") return n.Text == "-" ? (object)(-Num(Ev(env, n.A, sc))) : !Truthy(Ev(env, n.A, sc));
            if (n.Kind == "bin") return EvBin(env, n.Text, n.A, n.B, sc);
            if (n.Kind == "call") return EvCall(env, n.Text, n.ListA.Select(a => Ev(env, a, sc)).ToList());
            throw new UnresolvedException("expr " + n.Kind);
        }
        private static object EvBin(EvalEnv env, string op, AstNode l, AstNode r, List<Dictionary<string, object>> sc) {
            if (op == "&&") return Truthy(Ev(env, l, sc)) && Truthy(Ev(env, r, sc));
            if (op == "||") return Truthy(Ev(env, l, sc)) || Truthy(Ev(env, r, sc));
            var a = Ev(env, l, sc); var b = Ev(env, r, sc);
            if (op == "==" || op == "!=") return op == "==" ? Equals(a, b) : !Equals(a, b);
            double da = Num(a), db = Num(b);
            return op switch { "<" => da < db, "<=" => da <= db, ">" => da > db, ">=" => da >= db, "+" => da + db, "-" => da - db, "*" => da * db, "/" => db != 0.0 ? da / db : 0.0, "%" => db != 0.0 ? da % db : 0.0, _ => throw new UnresolvedException(op) };
        }
        private static object EvCall(EvalEnv env, string name, List<object> a) => ConstCalls.TryGetValue(name, out var c) ? c : name switch {
            "float" => Num(a[0]), "int" => Math.Truncate(Num(a[0])), "round" => Math.Floor(Num(a[0]) + 0.5), "abs" => Math.Abs(Num(a[0])),
            "max" => a.Max(Num), "min" => a.Min(Num), "clamp" => Math.Min(Math.Max(Num(a[0]), Num(a[1])), Num(a[2])), "lerp" => Num(a[0]) + (Num(a[1]) - Num(a[0])) * Num(a[2]),
            "selectfloat" or "selectint" or "select" => Truthy(a[0]) ? a[1] : a[2], "getcharacterlevel" or "getplayerlevel" => (double)env.Level,
            "getskillpurchased" => env.Skills == "all", "getterm" => ResolveTerm(env, ((RefVal)a[0]).Guid),
            "evalformula" or "evalequation" => EvalFormula(env, (RefVal)a[0]), "evalcurve" => EvalCurve(env.Db, (RefVal)a[0], Num(a[1])),
            _ => throw new UnresolvedException("fn " + name)
        };
        private static object ResolveTerm(EvalEnv env, long g) {
            if (!env.Terms.TryGetValue(g, out var v)) throw new UnresolvedException("term " + g);
            if (v is LazyTerm lt) {
                var rem = lt.Env.Terms.Where(kv => kv.Key != g).ToDictionary(kv => kv.Key, kv => kv.Value);
                env.Terms[g] = v = Run(new EvalEnv(lt.Env.Db, lt.Env.Level, lt.Env.Skills, rem, lt.Env.Depth), lt.Src);
            }
            return v;
        }
        private static double EvalCurve(TooltipDb db, RefVal r, double x) {
            var rec = db.Rec(TooltipDb.TCurve, r.Guid) ?? throw new UnresolvedException("curve " + r.Guid);
            var data = rec["Curve"]?["EditorCurveData"]; var keys = data.Arr("Keys");
            if (keys.Count == 0) return data.D("DefaultValue");
            if (x <= keys[0].D("Time")) return keys[0].D("Value");
            for (int i = 1; i < keys.Count; i++) {
                double t1 = keys[i].D("Time");
                if (x <= t1) { double t0 = keys[i - 1].D("Time"), v0 = keys[i - 1].D("Value"); return v0 + (keys[i].D("Value") - v0) * (t1 != t0 ? (x - t0) / (t1 - t0) : 0.0); }
            }
            return keys[keys.Count - 1].D("Value");
        }
        private static object EvalFormula(EvalEnv env, RefVal r) {
            if (r == null || r.Kind != "ref" || env.Depth > 12) throw new UnresolvedException("formula");
            var f = env.Db.Rec(TooltipDb.TFormula, r.Guid) ?? throw new UnresolvedException("formula " + r.Guid);
            var sub = new EvalEnv(env.Db, env.Level, env.Skills, env.Terms, env.Depth + 1);
            foreach (var t in f.Arr("InputTerms")) if (t["TermId"].L("Guid") is long g && !sub.Terms.ContainsKey(g)) sub.Terms[g] = new LazyTerm(sub, t.S("Value"));
            return Run(sub, env.Db.Ref(f["EquationId"]).S("Equation"), true);
        }
        public static (double? Val, string Why) TryEval(TooltipDb db, string src, int level, string skills = "none", Dictionary<long, object> terms = null) {
            if (string.IsNullOrWhiteSpace(src)) return (null, "empty");
            try { var v = Run(new EvalEnv(db, level, skills, terms), src, true); return v is bool b ? (b ? 1.0 : 0.0, null) : v is double d ? (d, null) : (null, "nan"); }
            catch (Exception ex) { return (null, ex.Message); }
        }
    }
    internal static class Fmt {
        public static string Num(double v, int maxfrac = 0) {
            string s = Math.Round(v, maxfrac, MidpointRounding.ToEven).ToString("N" + maxfrac, CultureInfo.InvariantCulture);
            if (s.Contains('.')) s = s.TrimEnd('0').TrimEnd('.');
            return s == "-0" ? "0" : s;
        }
        public static string Seconds(double v) { string s = Num(v < 60.0 ? v : v / 60.0, 1); return s + (v < 60.0 ? " second" : " minute") + (s == "1" ? "" : "s"); }
        public static long CostDisplay(double x) => -(((long)Math.Round(-0.5 - 2.0 * x, MidpointRounding.ToEven)) >> 1);
        public static int ChargesDisplay(double x) => (int)(((long)Math.Round(2.0 * x + 0.5, MidpointRounding.ToEven)) >> 1);
        public static string PyReprFloat(double v) {
            if (v == 0.0 && double.IsNegativeInfinity(1.0 / v)) v = 0.0;
            string s = v.ToString("R", CultureInfo.InvariantCulture);
            return (s.Contains('E') || s.Contains('e')) ? s.Replace("E", "e") : (s.Contains('.') ? s : s + ".0");
        }
        public static string CNum(double? v, double none = -1.0) => PyReprFloat(v ?? none);
        public static string CStr(string s) {
            var sb = new StringBuilder("\"");
            foreach (char ch in s ?? "") {
                if (ch == '\\') sb.Append("\\\\"); else if (ch == '"') sb.Append("\\\""); else if (ch == '\n') sb.Append("\\n"); else if (ch == '\r') continue;
                else if (ch == '\t') sb.Append("\\t"); else if (ch >= 32 && ch < 127) sb.Append(ch);
                else foreach (byte b in Encoding.UTF8.GetBytes(new[] { ch })) sb.Append('\\').Append(Convert.ToString(b, 8).PadLeft(3, '0'));
            }
            return sb.Append('"').ToString();
        }
        public static string Plain(string s) {
            s = Regex.Replace(s, @"<img id=""([^""]+)""\s*/>", "[$1]");
            s = Regex.Replace(s, @"<a [^>]*>|</>|<(?:Highlight|Bold|Flavor|Physical|Magical|Healing|NodeBuffStat|TooltipRegular|Red)>", "");
            return s.Replace("\r\n", "\n");
        }
    }
    internal sealed class TokenInfo {
        public string Raw = "", Kind = "", Hit, Effect, ValueKind, WholeValueExpr;
        public List<double?> Values; public Dictionary<long, string> SlotExprs; public List<string> Applies = new();
    }
    internal sealed class TooltipRenderer {
        private readonly TooltipDb _db; private readonly int _level; private readonly string _skills;
        public readonly List<TokenInfo> Tokens = new();
        public TooltipRenderer(TooltipDb db, int level, string skills) { _db = db; _level = level; _skills = skills; }
        private (double? Val, string Why) Ev(string src, Dictionary<long, object> terms = null) => ExprEval.TryEval(_db, src, _level, _skills, terms);
        public static List<string> SplitTemplate(string text) {
            var segs = new List<string>(); int depth = 0, lit0 = 0, tok0 = 0;
            for (int i = 0; i < text.Length; i++) {
                char c = text[i];
                if (c == '$') {
                    if (depth == 0) { segs.Add(text.Substring(lit0, i - lit0)); depth = 1; tok0 = i + 1; }
                    else if (--depth == 0) { segs.Add(text.Substring(tok0, i - tok0)); lit0 = i + 1; }
                } else if (c == '{') { if (depth < 1) { segs.Add(text.Substring(lit0, i - lit0)); tok0 = i + 1; } depth++; }
                else if (c == '}' && depth > 0 && --depth == 0) { segs.Add(text.Substring(tok0, i - tok0)); lit0 = i + 1; }
            }
            segs.Add(text.Substring(lit0)); return segs;
        }
        private static int NumAfter(string s, string key) {
            int i = s.IndexOf(key, StringComparison.OrdinalIgnoreCase); if (i < 0) return -1;
            int j = i + key.Length, k = j; while (k < s.Length && char.IsDigit(s[k])) k++;
            return k > j ? int.Parse(s.Substring(j, k - j), CultureInfo.InvariantCulture) : -1;
        }
        private static JsonNode KeyMapGet(JsonArray arr, int idx) => arr.FirstOrDefault(x => x != null && (int)x.L("Key") == idx)?["Value"];
        public string Render(string text, JsonObject ab, JsonObject frame = null) {
            var segs = SplitTemplate(text); var sb = new StringBuilder();
            for (int i = 0; i < segs.Count; i++) sb.Append(i % 2 == 0 ? segs[i] : EvalToken(segs[i], ab, frame));
            return sb.ToString();
        }
        private string EvalToken(string raw, JsonObject ab, JsonObject frame) {
            var tok = new TokenInfo { Raw = raw }; int col = raw.IndexOf(':');
            string head = col >= 0 ? raw.Substring(0, col) : raw, rest = col >= 0 ? raw.Substring(col + 1) : "", low = head.ToLowerInvariant();
            if (low.StartsWith("!skill") || low.StartsWith("skill")) return TokenSkill(raw, tok, ab, frame);
            if (low.StartsWith("formula") || low.StartsWith("!formula")) return TokenFormula(raw, tok, ab, frame);
            if (low == "charges" && col < 0) return TokenCharges(tok, ab);
            if ((low == "duration" || low == "maxduration" || low == "tick") && frame != null && col < 0) return FrameValue(low, frame, tok);
            if (low.StartsWith("flavor")) { tok.Kind = "flavor"; Tokens.Add(tok); return "<Flavor>" + rest + "</>"; }
            string kind = new[] { "hit", "effect", "linger", "proj", "statmod", "equation", "target", "tick", "apply" }.FirstOrDefault(k => low.StartsWith(k));
            if (kind == null) return raw;
            tok.Kind = kind; Tokens.Add(tok);
            try { return ResolveToken(kind, ParseRefParts(head, rest, kind), tok, ab, frame); } catch (UnresolvedException) { return "?"; }
        }
        private static (int? Idx, string Name, string Sub) ParseRefParts(string head, string rest, string kind) {
            int dot = head.IndexOf('.'); string hMain = dot >= 0 ? head.Substring(0, dot) : head, hSuf = dot >= 0 ? head.Substring(dot + 1) : "", idxS = hMain.Substring(kind.Length);
            int? idx = idxS.Length > 0 && idxS.All(char.IsDigit) ? int.Parse(idxS, CultureInfo.InvariantCulture) : null;
            string sub = (idx == null && rest.Length == 0 && dot >= 0) || (idx != null && rest.Length == 0) ? hSuf : (idx != null && rest.Length > 0) ? (hSuf.Length == 0 ? rest : hSuf + rest) : rest;
            string name = null;
            if (idx == null && rest.Length > 0) { var m = Regex.Match(rest, @"^([^:.]+)[:.]?(.*)$", RegexOptions.Singleline); name = m.Groups[1].Value; sub = m.Groups[2].Value; }
            return (idx, name, sub ?? "");
        }
        private string TokenSkill(string raw, TokenInfo tok, JsonObject ab, JsonObject frame) {
            var p = raw.Split(':', 4); if (p.Length < 4) return raw;
            tok.Kind = "skill"; Tokens.Add(tok);
            return ((_skills == "all") != raw.StartsWith("!", StringComparison.Ordinal)) ? Render(p[3], ab, frame) : "";
        }
        private string TokenFormula(string raw, TokenInfo tok, JsonObject ab, JsonObject frame) {
            var m = Regex.Match(raw, @"^!?formula:([^:.]+)(?:\.([^:]*))?(?::(.*))?$", RegexOptions.Singleline); if (!m.Success) return raw;
            string name = m.Groups[1].Value, suf = m.Groups[2].Success ? m.Groups[2].Value : "", body = m.Groups[3].Success ? m.Groups[3].Value : null;
            var f = _db.ByName(TooltipDb.TFormula, name);
            var (v, _) = f != null ? Ev("EvalFormula($#4547065559311335368:" + f.L("__guid") + "$)") : (null, "no formula");
            tok.Kind = "formula"; Tokens.Add(tok);
            if (body == null) return v == null ? "?" : Fmt.Num(suf.Contains('%') ? v.Value * 100.0 : v.Value, suf.Contains('F') ? 1 : 0) + (suf.Contains('%') ? "%" : "");
            return v == null ? "[if " + name + ": " + Render(body, ab, frame) + "]" : (((v.Value != 0.0) != raw.StartsWith("!", StringComparison.Ordinal)) ? Render(body, ab, frame) : "");
        }
        private string TokenCharges(TokenInfo tok, JsonObject ab) {
            var (v, why) = Ev(ab.S("CooldownCharges")); if (v == null && why == "empty") v = 1.0;
            tok.Kind = "charges"; Tokens.Add(tok); return v == null ? "?" : Fmt.ChargesDisplay(v.Value) + " charges";
        }
        private string FrameValue(string what, JsonObject eff, TokenInfo tok) {
            tok.Kind = what; Tokens.Add(tok);
            double? v = what == "tick" ? eff.D("TickTimer") : what == "maxduration" ? (Ev(eff["ExtendedMaximum"]?.ToString() ?? "").Val ?? 0.0) : Ev(eff.S("EffectDuration")).Val;
            return v == null ? "?" : Fmt.Seconds(v.Value);
        }
        private string ResolveToken(string kind, (int? Idx, string Name, string Sub) spec, TokenInfo tok, JsonObject ab, JsonObject frame) {
            if (kind == "hit") {
                JsonObject h = spec.Idx != null ? ((frame == null || frame.S("__kind") == "ability") ? _db.Ref(KeyMapGet(ab.Arr("HitsIds"), spec.Idx.Value)) : null) : _db.ByName(TooltipDb.THit, spec.Name);
                return h == null ? "?" : HitText(h, spec.Sub, ab, tok);
            }
            if (kind == "effect") {
                JsonObject eff = spec.Idx != null ? _db.Ref(KeyMapGet(ab.Arr("EffectsIds"), spec.Idx.Value)) : _db.ByName(TooltipDb.TEffect, spec.Name);
                if (eff == null) return "?";
                tok.Effect = eff.S("__name"); string nm = EffectName(eff), low = spec.Sub.ToLowerInvariant();
                return low.Contains("description") ? EffectDesc(eff, ab) : low.Contains("inline") ? nm + ": " + EffectDesc(eff, ab) : nm;
            }
            if (kind == "linger" || kind == "proj") return ResolveLingerOrProj(kind, spec, tok, ab);
            if ((kind == "statmod" || kind == "tick" || kind == "apply") && frame != null) return ResolveFrameMember(kind, spec, tok, ab, frame);
            return "?";
        }
        private string ResolveLingerOrProj(string kind, (int? Idx, string Name, string Sub) spec, TokenInfo tok, JsonObject ab) {
            JsonObject z = kind == "linger" ? (spec.Idx != null ? _db.Ref(KeyMapGet(ab.Arr("LingeringEffectsIds"), spec.Idx.Value)) : _db.ByName("LingeringEffectRecordBase", spec.Name))
                : (spec.Idx != null ? _db.Ref(KeyMapGet(ab.Arr("ProjectilesIds"), spec.Idx.Value)) : null);
            if (z == null) return "?";
            if (kind == "linger") {
                string s2 = spec.Sub.ToLowerInvariant();
                if (s2.StartsWith("duration")) return Fmt.Seconds(z.D("LifeTime"));
                if (s2.StartsWith("tick")) return Fmt.Seconds(z.D("TickRate"));
                var m = Regex.Match(spec.Sub, @"^(init|linger|beginoverlap|endoverlap|endhit)(\d+)(?:\.(.*))?$", RegexOptions.IgnoreCase | RegexOptions.Singleline);
                if (!m.Success) return "";
                string key = m.Groups[1].Value.ToLowerInvariant() switch { "init" => "InitialHitsIds", "linger" => "LingeringHitsIds", "beginoverlap" => "OnBeginOverlapHitsIds", "endoverlap" => "OnEndOverlapHitsIds", _ => "EndHitsIds" };
                var lst = z.Arr(key); int n = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
                return n < lst.Count ? HitText(_db.Ref(lst[n]), m.Groups[3].Value, ab, tok) : "";
            }
            var mp = Regex.Match(spec.Sub, @"^hit(\d+)\.?(.*)$", RegexOptions.Singleline); var pHits = z.Arr("HitsIds");
            return (mp.Success && int.Parse(mp.Groups[1].Value, CultureInfo.InvariantCulture) is int hi && hi < pHits.Count) ? HitText(_db.Ref(pHits[hi]), mp.Groups[2].Value, ab, tok) : "?";
        }
        private string ResolveFrameMember(string kind, (int? Idx, string Name, string Sub) spec, TokenInfo tok, JsonObject ab, JsonObject frame) {
            int n = spec.Idx ?? 0;
            if (kind == "statmod") { var sms = frame.Arr("StatModsIds"); return n < sms.Count ? (StatModValue(_db.Ref(sms[n]), spec.Sub, tok) ?? "?") : ""; }
            var lst = frame.Arr(kind == "tick" ? "TickHitsIds" : "ApplyHitsIds");
            return n < lst.Count ? HitText(_db.Ref(lst[n]), spec.Sub, ab, tok) : "";
        }
        private static string EffectName(JsonObject eff) => eff.SourceStr("EffectName") is string s && s.Length > 0 ? s : eff.S("__name");
        private string EffectDesc(JsonObject eff, JsonObject ab) => Render(eff.SourceStr("EffectDescription"), ab, eff);
        private string HitText(JsonObject h, string suffix, JsonObject ab, TokenInfo tok) {
            string s = suffix ?? "", low = s.ToLowerInvariant(); tok.Hit = h.S("__name");
            if (low.StartsWith("statmod")) {
                int n = NumAfter(s, "statmod"); var sms = h.Arr("StatModsIds");
                return (n >= 0 && n < sms.Count) ? (StatModValue(_db.Ref(sms[n]), s.Substring("statmod".Length + n.ToString(CultureInfo.InvariantCulture).Length), tok) ?? "?") : "<ParseFail>hit statmod index error</>";
            }
            return (low.StartsWith("apply") || low.StartsWith("hideapply") || low.StartsWith("applyself")) ? HitApplyText(h, s, ab, tok) : HitDefaultText(h, s, tok);
        }
        private string HitApplyText(JsonObject h, string s, JsonObject ab, TokenInfo tok) {
            string low = s.ToLowerInvariant(); bool selfMode = low.Contains("applyself");
            int n = NumAfter(s, selfMode ? "applyself" : "apply"); var lst = h.Arr(selfMode ? "EffectsAppliedToSelf" : "ApplyEffects");
            if (n < 0 || n >= lst.Count) return "";
            var ap = lst[n].AsObject(); var eff = _db.Ref(ap["EffectId"] ?? ap["Effect"]); string name = EffectName(eff);
            tok.Applies.Add(eff.S("__name"));
            string res = low.Contains("inline") ? name + ": " + EffectDesc(eff, ab) : name;
            if (low.Contains("hide")) return "";
            if (low.Contains("dur") || low.Contains("for")) {
                var (d, _) = Ev(ap.S("EffectDuration")); if (d == null || d <= 0) (d, _) = Ev(eff.S("EffectDuration"));
                if (d != null && d > 0 && !eff.B("bInfiniteDuration")) res = low.Contains("for") ? res + " for " + Fmt.Seconds(d.Value) : Fmt.Seconds(d.Value);
            }
            var (st, _) = Ev(ap.S("StackCount"));
            return (st != null && st >= 1.5) ? ((int)st.Value) + " stacks of " + res : res;
        }
        private string HitDefaultText(JsonObject h, string s, TokenInfo tok) {
            var sms = h.Arr("StatModsIds"); if (sms.Count == 0) return "";
            var sm = _db.Ref(sms[0]); string txt = StatModValue(sm, s + "%nostat", tok);
            var tags = h.Arr("EventTags").Select(t => t.S("TagName")).ToList();
            string kind = tags.Any(t => t.StartsWith("Element.Healing")) ? "healing" : tags.Any(t => t.StartsWith("Element.Physical")) ? "physical" : tags.Any(t => t.StartsWith("Element.Magical")) ? "magical" : "";
            string leaf = (tags.Count > 0 && kind.Length > 0 && tags[0].Count(c => c == '.') > 1) ? tags[0].Split('.').Last() : "";
            string label = leaf.Length > 0 ? leaf : (kind == "healing" ? "Healing" : kind == "physical" ? "Physical" : kind == "magical" ? "Magical" : "");
            if (txt != null) return txt + (label.Length > 0 ? " " + label : "");
            var terms = sm.Arr("ValueInputTerms").ToDictionary(t => t["TermId"].L("Guid"), t => t.S("Value"));
            var (coef, _) = terms.TryGetValue(TooltipDb.TermBase, out var bSrc) ? Ev(bSrc) : (null, "empty");
            string lab = kind == "healing" ? "healing" : kind == "physical" ? "physical damage" : kind == "magical" ? "magical damage" : "effect";
            string prefix = leaf.Length > 0 ? leaf + " " : "";
            return coef != null ? "[" + Fmt.Num(coef.Value, 3) + "x " + prefix + lab + "]" : "[" + prefix + lab + "]";
        }
        private string StatModValue(JsonObject sm, string flags, TokenInfo tok) {
            string f = flags.ToLowerInvariant(), stat = _db.StatDisplay(sm["StatRefId"].L("Guid"));
            if (f.Contains("onlystat")) { tok.ValueKind = "stat name"; return stat; }
            var terms = sm.Arr("ValueInputTerms").ToDictionary(t => t["TermId"].L("Guid"), t => t.S("Value"));
            var (vals, kind) = EvalStatModSlots(sm, terms, f);
            tok.ValueKind = kind; tok.WholeValueExpr = kind.StartsWith("Value") ? sm.S("Value") : null; tok.SlotExprs = kind.StartsWith("Value") ? null : terms;
            if (vals.Any(x => x == null)) return null;
            tok.Values = vals; string txt = FormatStatModVals(vals, f);
            if (f.Contains("nostat")) return txt;
            return f.Replace("nostat", "").Contains("by") ? stat + (txt.StartsWith("-") ? " decreased by " : " increased by ") + txt.TrimStart('-') : txt + " " + stat;
        }
        private (List<double?>, string) EvalStatModSlots(JsonObject sm, Dictionary<long, string> terms, string f) {
            if (f.Contains("minmax")) return (new List<double?> { EvalSlot(terms, TooltipDb.TermMin), EvalSlot(terms, TooltipDb.TermMax) }, "BaseValueMin-BaseValueMax");
            var (slot, kName) = f.Contains("max") ? (TooltipDb.TermMax, "BaseValueMax") : f.Contains("min") ? (TooltipDb.TermMin, "BaseValueMin") : f.Contains("add") ? (TooltipDb.TermAdd, "BaseValueAdd") : (TooltipDb.TermBase, "BaseValue");
            double? v = EvalSlot(terms, slot);
            if (v == 0.0 && slot == TooltipDb.TermBase) {
                var lazy = terms.ToDictionary(kv => kv.Key, kv => (object)new LazyTerm(new EvalEnv(_db, _level, _skills), kv.Value));
                return (new List<double?> { Ev(sm.S("Value"), lazy).Val }, "Value (the whole stat mod expression, evaluated for the player)");
            }
            return (new List<double?> { v }, kName);
        }
        private double? EvalSlot(Dictionary<long, string> terms, long g) => terms.TryGetValue(g, out var src) ? Ev(src).Val : 0.0;
        private static string FormatStatModVals(List<double?> vals, string f) {
            bool pct = f.Contains('%'); int digits = f.Contains("ff") ? 2 : (f.Contains('f') ? 1 : 0);
            return string.Join("-", vals.Select(v => {
                double x = v.Value - (f.Contains("-1") ? 1.0 : 0.0); if (f.Contains("abs")) x = Math.Abs(x);
                return Fmt.Num(pct ? x * 100.0 : x, digits) + (pct ? "%" : "");
            }));
        }
    }
    public static class TooltipGenerator {
        private static readonly string[] Classes = { "Bard", "Cleric", "Fighter", "Mage", "Ranger", "Rogue", "Summoner", "Tank" };
        private static readonly string[] EffectHitKeys = { "ApplyHitsIds", "TickHitsIds", "ExpireHitsIds", "EndHitsIds", "RemoveHitsIds", "PreApplyHitsIds", "ConsumeHitsIds" };
        private static readonly Dictionary<string, long> TermSlot = new() { ["BaseValue"] = TooltipDb.TermBase, ["BaseValueMin"] = TooltipDb.TermMin, ["BaseValueMax"] = TooltipDb.TermMax, ["BaseValueAdd"] = TooltipDb.TermAdd };
        public static (string HeaderText, int AbilityCount, int GroupCount) GenerateHeader(string dbPath, string repoRoot, int level = 25) {
            using var db = new TooltipDb(dbPath); var groups = LoadRoster(db, repoRoot);
            var sb = new StringBuilder(HeaderBanner(level)); int count = 0;
            foreach (var grp in groups) foreach (var name in grp.Value.Keys.OrderBy(k => k, StringComparer.Ordinal)) {
                var ab = db.ByName(TooltipDb.TAbility, name); if (ab == null) continue;
                sb.Append(FormatAbilityRow(db, ab, grp.Key, name, level)); count++;
            }
            sb.Append("};\n\nconstexpr int kKeyCount = ").Append(count).Append(";\n\n}  // namespace tooltip_key\n");
            return (sb.ToString(), count, groups.Count);
        }
        private static Dictionary<string, Dictionary<string, string>> LoadRoster(TooltipDb db, string repoRoot) {
            var groups = Classes.ToDictionary(c => c, _ => new Dictionary<string, string>(StringComparer.Ordinal));
            var prof = new Dictionary<string, string>(StringComparer.Ordinal); var weapon = new Dictionary<string, string>(StringComparer.Ordinal);
            var general = new Dictionary<string, string>(StringComparer.Ordinal); var items = new Dictionary<string, string>(StringComparer.Ordinal);
            QueryTrees(db, groups, prof, weapon);
            string tsv = Path.Combine(repoRoot, "src", "server", "services", "cmd", "contentc", "ability_names.tsv");
            if (File.Exists(tsv)) foreach (var line in File.ReadAllLines(tsv, Encoding.UTF8)) {
                if (line.StartsWith("#") || string.IsNullOrWhiteSpace(line)) continue;
                var sp = line.Split('\t');
                if (sp[1] == "weapon_combo") weapon[sp[0]] = "ability_names.tsv weapon_combo";
                else if (sp[1] == "general") general[sp[0]] = "ability_names.tsv general";
                else if (sp[1] == "item") items[sp[0]] = "ability_names.tsv item";
            }
            groups["Weapons"] = weapon; groups["General"] = general; groups["Consumables"] = items; groups["Professions"] = prof;
            return groups;
        }
        private static void QueryTrees(TooltipDb db, Dictionary<string, Dictionary<string, string>> groups, Dictionary<string, string> prof, Dictionary<string, string> weapon) {
            var s1 = NativeSqlite.Prepare(db.Handle, "select distinct t.name, t.kind, r.ability_name from skill_tree t join skill_node n on n.tree_guid=t.guid join skill_rank r on r.skill_guid=n.skill_guid where r.ability_guid<>0 and t.kind in ('class','other') order by 1,3");
            while (NativeSqlite.sqlite3_step(s1) == 100) if (groups.TryGetValue(NativeSqlite.GetText(s1, 0).Split('_')[0], out var g)) g.TryAdd(NativeSqlite.GetText(s1, 2), NativeSqlite.GetText(s1, 1) == "class" ? "class tree" : "Stage tree");
            NativeSqlite.sqlite3_finalize(s1);
            var s2 = NativeSqlite.Prepare(db.Handle, "select distinct t.name, r.ability_name from skill_tree t join skill_node n on n.tree_guid=t.guid join skill_rank r on r.skill_guid=n.skill_guid where r.ability_guid<>0 and t.kind='profession'");
            while (NativeSqlite.sqlite3_step(s2) == 100) prof[NativeSqlite.GetText(s2, 1)] = NativeSqlite.GetText(s2, 0);
            NativeSqlite.sqlite3_finalize(s2);
            var s3 = NativeSqlite.Prepare(db.Handle, "select a.name from ab_ability a join item_weapon w on w.attack_ability_guid=a.guid group by a.guid");
            while (NativeSqlite.sqlite3_step(s3) == 100) weapon[NativeSqlite.GetText(s3, 0)] = "item_weapon attack ability";
            NativeSqlite.sqlite3_finalize(s3);
        }
        private static string FormatAbilityRow(TooltipDb db, JsonObject ab, string grp, string name, int level) {
            ab["__kind"] = "ability"; string desc = ab.SourceStr("AbilityDescription");
            var r = new TooltipRenderer(db, level, "none");
            string text = string.IsNullOrEmpty(desc) ? "" : Fmt.Plain(r.Render(desc, ab, null)).Trim();
            var (ownHits, effMap) = BuildClosure(db, ab, level);
            var (cdNone, cdWhy0) = ExprEval.TryEval(db, ab.S("Cooldown"), level, "none");
            var (cdAll, cdWhy1) = ExprEval.TryEval(db, ab.S("Cooldown"), level, "all");
            double? cd0 = cdNone ?? (cdWhy0 == "empty" ? 0.0 : null), cd1 = cdAll ?? (cdWhy1 == "empty" ? 0.0 : null);
            var (castS, _) = ExprEval.TryEval(db, ab.S("DynamicCastDuration"), level, "none");
            var (rngCm, rngSrc) = EvalRange(db, ab, level);
            return string.Format(CultureInfo.InvariantCulture, "    {{{0}, {1}, {2}ll, {3}, {4}, {5}, {6}, {7}, {8}, {9}, {10}, {11}, {12}, {13}, {14}, {15}, {16}, {17}, {18}}},\n",
                Fmt.CStr(grp), Fmt.CStr(name), ab.L("__guid"), Fmt.CNum(cd0), Fmt.CNum(cd1), Fmt.CStr(ab.S("SharedCooldownTag")),
                EvalCharges(db, ab, level, "none"), EvalCharges(db, ab, level, "all"), Fmt.CStr(KeyCosts(db, ab, level)), Fmt.CNum(castS), Fmt.CNum(rngCm), Fmt.CStr(rngSrc),
                (int)ab.L("ApplicableTargets"), Fmt.CStr(ab.S("TargetActivationRequirement").Split("::").Last()), Fmt.CStr(ab.S("HeldAbilityBehavior").Split("::").Last()),
                Fmt.CStr(ab.S("WeaponRequirement").Split("::").Last()), Fmt.CStr(KeyHits(r.Tokens, ownHits)), Fmt.CStr(KeyApplies(r.Tokens, ownHits, effMap)), Fmt.CStr(text));
        }
        private static int EvalCharges(TooltipDb db, JsonObject ab, int level, string sk) => ExprEval.TryEval(db, ab.S("CooldownCharges"), level, sk) is (double v, _) ? Fmt.ChargesDisplay(v) : (ExprEval.TryEval(db, ab.S("CooldownCharges"), level, sk).Why == "empty" ? 1 : -1);
        private static (double? Cm, string Src) EvalRange(TooltipDb db, JsonObject ab, int level) {
            if (ab.B("bOverrideRangeInTooltip")) return (ExprEval.TryEval(db, ab.S("CustomTooltipRange"), level, "none").Val, "CustomTooltipRange");
            if (ab.B("bIsAOEClickedAbility")) return (ab.D("AreaTargetRange"), "AreaTargetRange");
            if (ab.B("bUseTargetingRecord")) return (db.Ref(ab["TargetingRecordId"])?["LockOnInfo"]?.D("ValidDistance"), "targeting record ValidDistance");
            if (ab.B("bCheckTargetValidity") && ab.B("bPerformConeCheck")) return (ExprEval.TryEval(db, ab.S("ValidDistanceExp"), level, "none").Val, "ValidDistanceExp");
            return (0.0, "none");
        }
        private static (HashSet<string>, Dictionary<string, (bool Inf, double? Dur)>) BuildClosure(TooltipDb db, JsonObject ab, int level) {
            var ownHits = new HashSet<string>(StringComparer.Ordinal); var seenHits = new HashSet<long>();
            var effMap = new Dictionary<string, (bool Inf, double? Dur)>(StringComparer.Ordinal); var seenEffs = new HashSet<long>();
            void AddHit(JsonNode rn) {
                var h = db.Ref(rn); if (h == null || !seenHits.Add(h.L("__guid"))) return;
                ownHits.Add(h.S("__name"));
                foreach (var ap in h.Arr("ApplyEffects")) AddEff(ap["EffectId"]);
                foreach (var x in h.Arr("EffectsAppliedToSelf")) AddEff(x["Effect"]);
            }
            void AddEff(JsonNode rn) {
                var e = db.Ref(rn); if (e == null || !seenEffs.Add(e.L("__guid"))) return;
                effMap[e.S("__name")] = (e.B("bInfiniteDuration"), ExprEval.TryEval(db, e.S("EffectDuration"), level, "none").Val);
                foreach (var k in EffectHitKeys) foreach (var h in e.Arr(k)) AddHit(h);
            }
            foreach (var x in ab.Arr("HitsIds")) AddHit(x["Value"]);
            foreach (var x in ab.Arr("EffectsIds")) AddEff(x["Value"]);
            foreach (var x in ab.Arr("ProjectilesIds")) if (db.Ref(x["Value"]) is JsonObject p) foreach (var h in p.Arr("HitsIds")) AddHit(h);
            foreach (var x in ab.Arr("LingeringEffectsIds")) if (db.Ref(x["Value"]) is JsonObject z)
                foreach (var k in new[] { "InitialHitsIds", "LingeringHitsIds", "OnBeginOverlapHitsIds", "OnEndOverlapHitsIds", "EndHitsIds" }) foreach (var h in z.Arr(k)) AddHit(h);
            return (ownHits, effMap);
        }
        private static string KeyCosts(TooltipDb db, JsonObject ab, int level) {
            var res = new List<string>();
            foreach (var g in ab.Arr("Costs")) {
                string enExpr = g.S("Enabled");
                foreach (var sc in g.Arr("StatCosts")) {
                    string stat = db.StatName(sc["Stat"].L("Guid")); if (stat == "0") continue;
                    var (v, why) = ExprEval.TryEval(db, sc.S("Value"), level, "none"); if (v == null && why == "empty") v = 0.0;
                    string shown = (v != null && Fmt.CostDisplay(v.Value) > 0) ? Fmt.CostDisplay(v.Value).ToString(CultureInfo.InvariantCulture) : "";
                    string en = string.IsNullOrEmpty(enExpr) ? "1" : (ExprEval.TryEval(db, enExpr, level, "none").Val is double ev ? (ev != 0.0 ? "1" : "0") : "?");
                    res.Add(stat + "=" + (v == null ? "?" : Fmt.PyReprFloat(v.Value)) + "|" + shown + "|" + en);
                }
            }
            return string.Join(";", res);
        }
        private static string KeyHits(List<TokenInfo> tokens, HashSet<string> ownHits) {
            var res = new List<string>(); var seen = new HashSet<(string, string)>();
            foreach (var t in tokens) {
                if (t.Hit == null || t.ValueKind == null || t.ValueKind == "stat name") continue;
                var kinds = t.ValueKind.Contains('-') ? t.ValueKind.Split('-') : new[] { t.ValueKind };
                var vals = t.Values ?? Enumerable.Repeat<double?>(null, kinds.Length).ToList();
                for (int i = 0; i < kinds.Length; i++) {
                    string k = kinds[i]; if (!seen.Add((t.Hit, k))) continue;
                    double? v = i < vals.Count ? vals[i] : null;
                    string term = t.SlotExprs != null ? (TermSlot.TryGetValue(k, out var slot) && t.SlotExprs.TryGetValue(slot, out var s) ? s : "") : (t.WholeValueExpr ?? "");
                    res.Add(t.Hit + "|" + (k.StartsWith("Value") ? "Value" : k) + "|" + (v == null ? "?" : Fmt.PyReprFloat(v.Value)) + "|" + (ownHits.Contains(t.Hit) ? "1" : "0") + "|" + Regex.Replace(term.Trim(), @"\s+", " ").Replace(';', ',').Replace('|', '/'));
                }
            }
            return string.Join(";", res);
        }
        private static string KeyApplies(List<TokenInfo> tokens, HashSet<string> ownHits, Dictionary<string, (bool Inf, double? Dur)> effMap) {
            var names = new List<string>();
            foreach (var t in tokens) {
                var cand = new List<string>();
                if (t.Kind == "effect" && t.Effect != null && effMap.ContainsKey(t.Effect)) cand.Add(t.Effect);
                foreach (var a in t.Applies) cand.Add((t.Hit != null && !ownHits.Contains(t.Hit) ? "!" : "") + a);
                foreach (var n in cand) if (!string.IsNullOrEmpty(n) && !names.Contains(n) && !names.Contains(n.TrimStart('!'))) names.Add(n);
            }
            return string.Join(";", names.Select(n => (!effMap.TryGetValue(n.TrimStart('!'), out var x) || (!x.Inf && x.Dur == null)) ? n + "=?" : n + "=" + (x.Inf ? "inf" : Fmt.PyReprFloat(x.Dur.Value))));
        }
        private static string HeaderBanner(int level) => string.Join("\n", new[] {
            "// ============================================================================\n//  tooltip_key.h\n//\n//  GENERATED by tools/capture/gen_tooltips.py; do not edit.\n//",
            "//  What the game client's ability tooltip states, for every ability of every class, the weapon combos and the general, consumable and profession",
            "//  abilities (plan E1, stage 3 fix, Part C: the tooltip is the truth; test_worldd_tooltips.cpp holds the runtime to it).  The client builds the tooltip",
            "//  in USkillToolTipWidget::BuildFinalTooltip from the ability record's AbilityDescription",
            "//  template and the record's expressions evaluated for the PLAYER; this table is that evaluation for a level-" + level + " character with no gear, no skill",
            "//  bought and no effects (research: the scratchpad's research/tooltips/evidence.md).  One Key per ability:\n//",
            "//    cooldown_s          the data's Cooldown (the skill tooltip prints no cooldown line); -1 when the generator could not evaluate it",
            "//    charges             CooldownCharges rounded half up (the line is printed when it is over 1); charges_all: with every skill bought",
            "//    costs               \"stat=value|shown|enabled;...\" each stat cost: the value, the number the line prints (about ceil) and whether the cost",
            "//                        group is enabled for the player (1/0/?); \"?\" needs the stat engine",
            "//    cast_s              DynamicCastDuration; -1: the cast time is the montage's notify time over the cast speed (client-side, not in the data)",
            "//    range_cm, range_source   CustomTooltipRange, else AreaTargetRange (a clicked AoE), else the targeting record's ValidDistance, else ValidDistanceExp;",
            "//                        the line prints floor(cm / 100) meters when over 0",
            "//    target_mask         ApplicableTargets (Self 1, Enemy 2, Ally 4, Party 8, Raid 16); target_requirement: Always (a target is needed) / Never",
            "//    hits                \"hit|term|value|closure|term text;...\" every hit the text states a number for: the value is the stat mod's term (BaseValue,",
            "//                        BaseValueMin/Max/Add, or the whole Value when BaseValue is 0), which the tooltip prints x 100 with a percent sign; closure 0",
            "//                        when the text describes another ability's hit (a pet's)",
            "//    applies             \"effect=duration;...\" the effects the text names that the ability applies (the applyK tokens, and the effect: tokens the data's",
            "//                        closure reaches; the rest are conditions or descriptions the text links to), with the record's duration (inf, ?); \"!effect\" is",
            "//                        applied by a hit the ability itself does not fire (a reaction's hit, another ability's)",
            "//    text                the tooltip's description as the client prints it (no skill bought)\n// ============================================================================",
            "#pragma once\n\n#include <cstdint>\n\nnamespace tooltip_key {\n\nconstexpr int kLevel = " + level + ";\n\nstruct Key {",
            "    const char* group;\n    const char* name;\n    int64_t     guid;\n    double      cooldown_s;\n    double      cooldown_all_s;",
            "    const char* shared_tag;\n    int         charges;\n    int         charges_all;\n    const char* costs;\n    double      cast_s;",
            "    double      range_cm;\n    const char* range_source;\n    int         target_mask;\n    const char* target_requirement;",
            "    const char* held;\n    const char* weapon_requirement;\n    const char* hits;\n    const char* applies;\n    const char* text;\n};\n\nconst Key kKeys[] = {\n"
        });
    }
}
'@

function Initialize-TooltipEngine {
    [CmdletBinding()]
    param()
    if (-not ('SandalCore.Tools.Tooltips.TooltipGenerator' -as [type])) {
        Add-Type -TypeDefinition $script:TooltipCSharpSource -Language CSharp
    }
}

function Invoke-TooltipHeaderBuild {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$DbPath,
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [int]$Level = 25
    )
    Initialize-TooltipEngine
    $res = [SandalCore.Tools.Tooltips.TooltipGenerator]::GenerateHeader($DbPath, $RepoRoot, $Level)
    return [pscustomobject]@{
        HeaderText   = $res.Item1
        AbilityCount = $res.Item2
        GroupCount   = $res.Item3
    }
}

Export-ModuleMember -Function Initialize-TooltipEngine, Invoke-TooltipHeaderBuild
