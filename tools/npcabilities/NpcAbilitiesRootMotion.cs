// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Animation;
using CUE4Parse.UE4.Assets.Exports.Animation.ACL;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Readers;
using CUE4Parse.UE4.Objects.UObject;
using CUE4Parse.Utils;
using Mapc;

namespace NpcAbilities;

public static partial class Reader
{
    static IEnumerable<FStructFallback> StructsIn(FStructFallback owner, string name)
    {
        var prop = owner.Properties.FirstOrDefault(x => x.Name.Text == name);
        if (prop?.Tag?.GenericValue is UScriptArray arr)
            foreach (var item in arr.Properties)
                if (item.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback sf) yield return sf;
    }

    static int RootBoneTrack(UAnimSequence seq)
    {
        var map = seq.CompressedTrackToSkeletonMapTable;
        for (int i = 0; i < map.Length; i++)
            if (map[i].BoneTreeIndex == 0) return i;
        return -1;
    }

    static (FVector Mins, FVector Ranges) ReadIntervalFixed32Bounds(FByteArchive ar, int mask)
    {
        var mins = FVector.ZeroVector;
        var ranges = FVector.ZeroVector;
        if ((mask & 1) != 0) { mins.X = ar.Read<float>(); ranges.X = ar.Read<float>(); }
        if ((mask & 2) != 0) { mins.Y = ar.Read<float>(); ranges.Y = ar.Read<float>(); }
        if ((mask & 4) != 0) { mins.Z = ar.Read<float>(); ranges.Z = ar.Read<float>(); }
        return (mins, ranges);
    }

    static FVector? ReadPerTrackKey(FByteArchive ar, AnimationCompressionFormat format, int mask, FVector mins, FVector ranges)
    {
        switch (format)
        {
            case AnimationCompressionFormat.ACF_None:
            case AnimationCompressionFormat.ACF_Float96NoW:
                return (mask & 7) != 0
                    ? new FVector((mask & 1) != 0 ? ar.Read<float>() : 0, (mask & 2) != 0 ? ar.Read<float>() : 0, (mask & 4) != 0 ? ar.Read<float>() : 0)
                    : ar.Read<FVector>();
            case AnimationCompressionFormat.ACF_IntervalFixed32NoW:
                return ar.ReadVectorIntervalFixed32(mins, ranges);
            case AnimationCompressionFormat.ACF_Fixed48NoW:
                return new FVector(
                    (mask & 1) != 0 ? AnimationCompressionUtils.DecodeFixed48_PerTrackComponent(ar.Read<ushort>(), 7) : 0,
                    (mask & 2) != 0 ? AnimationCompressionUtils.DecodeFixed48_PerTrackComponent(ar.Read<ushort>(), 7) : 0,
                    (mask & 4) != 0 ? AnimationCompressionUtils.DecodeFixed48_PerTrackComponent(ar.Read<ushort>(), 7) : 0);
            case AnimationCompressionFormat.ACF_Identity:
                return FVector.ZeroVector;
            default:
                return null;
        }
    }

    static int[] ReadPerTrackFrames(FByteArchive ar, UAnimSequence seq, int count, bool timed)
    {
        var frames = new int[count];
        if (timed && count > 1)
            for (int k = 0; k < count; k++) frames[k] = seq.NumFrames < 256 ? ar.Read<byte>() : ar.Read<ushort>();
        else
            for (int k = 0; k < count; k++) frames[k] = k;
        return frames;
    }

    static List<(float Time, FVector Pos)>? PerTrackRootTranslation(UAnimSequence seq, FUECompressedAnimData cd, out string why)
    {
        why = "";
        int track = RootBoneTrack(seq);
        if (track < 0 || track * 2 >= cd.CompressedTrackOffsets.Length) { why = "no root bone track"; return null; }
        int offset = cd.CompressedTrackOffsets[track * 2];
        if (offset < 0) return null;
        var ar = new FByteArchive("animation", cd.CompressedByteStream) { Position = offset };
        uint packed = ar.Read<uint>();
        var format = (AnimationCompressionFormat)(packed >> 28);
        int mask = (int)((packed >> 24) & 0xF), count = (int)(packed & 0xFFFFFF);
        bool timed = (mask & 8) != 0;
        var (mins, ranges) = format == AnimationCompressionFormat.ACF_IntervalFixed32NoW
            ? ReadIntervalFixed32Bounds(ar, mask)
            : (FVector.ZeroVector, FVector.ZeroVector);
        var keys = new FVector[count];
        for (int k = 0; k < count; k++)
        {
            var key = ReadPerTrackKey(ar, format, mask, mins, ranges);
            if (key == null) { why = "translation keys in format " + format; return null; }
            keys[k] = key.Value;
        }
        ar.Position = (ar.Position + 3) & ~3L;
        var frames = ReadPerTrackFrames(ar, seq, count, timed);
        double perFrame = seq.NumFrames > 1 ? seq.SequenceLength / (double)(seq.NumFrames - 1) : 0;
        var list = new List<(float, FVector)>(count);
        for (int k = 0; k < count; k++) list.Add(((float)(frames[k] * perFrame), keys[k]));
        return list;
    }

    static List<(float Time, FVector Pos)>? RootTranslation(UAnimSequence seq, out string why)
    {
        why = "";
        if (!(seq.TryGetValue(out bool enabled, "bEnableRootMotion") && enabled)) return null;
        if (seq.CompressedDataStructure is FACLCompressedAnimData acl) return AclRootTranslation(seq, acl, out why);
        if (seq.CompressedDataStructure is FUECompressedAnimData lerp && lerp.KeyEncodingFormat == AnimationKeyFormat.AKF_VariableKeyLerp)
            return KeyLerpRootTranslation(seq, lerp, out why);
        if (seq.CompressedDataStructure is not FUECompressedAnimData cd || cd.KeyEncodingFormat != AnimationKeyFormat.AKF_PerTrackCompression)
        {
            why = "the animation is stored in a form this tool does not read";
            return null;
        }
        return PerTrackRootTranslation(seq, cd, out why);
    }

    static List<(float Time, FVector Pos)>? AclRootTranslation(UAnimSequence seq, FACLCompressedAnimData acl, out string why)
    {
        why = "";
        int track = RootBoneTrack(seq);
        if (track < 0) { why = "no root bone track"; return null; }
        var samples = Acl.ReadTranslation(acl.CompressedByteStream, track, out float rate, out why);
        if (samples == null) return null;
        var list = new List<(float, FVector)>(samples.Length);
        for (int k = 0; k < samples.Length; k++) list.Add((k / rate, new FVector(samples[k].X, samples[k].Y, samples[k].Z)));
        return list;
    }

    static List<(float Time, FVector Pos)>? KeyLerpRootTranslation(UAnimSequence seq, FUECompressedAnimData cd, out string why)
    {
        why = "";
        int track = RootBoneTrack(seq);
        if (track < 0 || track * 4 + 3 >= cd.CompressedTrackOffsets.Length) { why = "no root bone track"; return null; }
        int offset = cd.CompressedTrackOffsets[track * 4], count = cd.CompressedTrackOffsets[track * 4 + 1];
        if (offset < 0 || count <= 1) return null;
        if (cd.TranslationCompressionFormat != AnimationCompressionFormat.ACF_None && cd.TranslationCompressionFormat != AnimationCompressionFormat.ACF_Float96NoW)
        {
            why = "translation keys in format " + cd.TranslationCompressionFormat;
            return null;
        }
        var ar = new FByteArchive("animation", cd.CompressedByteStream) { Position = offset };
        var keys = new FVector[count];
        for (int k = 0; k < count; k++) keys[k] = new FVector(ar.Read<float>(), ar.Read<float>(), ar.Read<float>());
        ar.Position = (ar.Position + 3) & ~3L;
        double perFrame = seq.NumFrames > 1 ? seq.SequenceLength / (double)(seq.NumFrames - 1) : 0;
        var list = new List<(float, FVector)>(count);
        for (int k = 0; k < count; k++) list.Add(((float)((seq.NumFrames < 256 ? ar.Read<byte>() : ar.Read<ushort>()) * perFrame), keys[k]));
        return list;
    }

    static FVector Interpolate(List<(float Time, FVector Pos)> keys, float time)
    {
        if (time <= keys[0].Time) return keys[0].Pos;
        for (int i = 1; i < keys.Count; i++)
        {
            if (time > keys[i].Time) continue;
            float span = keys[i].Time - keys[i - 1].Time;
            float f = span > 0 ? (time - keys[i - 1].Time) / span : 1f;
            return keys[i - 1].Pos + (keys[i].Pos - keys[i - 1].Pos) * f;
        }
        return keys[^1].Pos;
    }

    static void CheckFurtherSlotSegment(UObject? resolved, ref bool unreadable)
    {
        if (resolved is UAnimSequence further && further.TryGetValue(out bool furtherMoves, "bEnableRootMotion") && furtherMoves)
            unreadable = true;
        else if (resolved != null && resolved is not UAnimSequence)
            unreadable = true;
    }

    static List<(float Time, FVector Pos)> SampleSegmentMotion(List<(float Time, FVector Pos)> keys, float start, float end, float signedRate, float begins)
    {
        float rate = Math.Abs(signedRate);
        var times = new List<float> { start };
        foreach (var k in keys)
            if (k.Time > start && k.Time < end) times.Add(k.Time);
        times.Add(end);
        if (signedRate < 0f) times.Reverse();
        float from = times[0];
        var origin = Interpolate(keys, from);
        var samples = new List<(float Time, FVector Pos)>();
        foreach (float at in times)
        {
            var d = Interpolate(keys, at) - origin;
            float elapsed = signedRate < 0f ? (from - at) / rate : (at - from) / rate;
            samples.Add((begins + elapsed, d));
        }
        return samples;
    }

    static void AppendSegmentSamples(
        List<(float Time, float X, float Y, float Z)> result,
        List<(float Time, FVector Pos)> samples,
        ref (FVector Travelled, bool Moved, bool Unreadable) state)
    {
        float most = samples.Max(x => Math.Max(Math.Abs(x.Pos.X), Math.Max(Math.Abs(x.Pos.Y), Math.Abs(x.Pos.Z))));
        if (most < 1f) return;
        foreach (var (time, d) in samples)
        {
            var p = state.Moved ? state.Travelled + d : d;
            if (result.Count > 0)
            {
                var last = result[^1];
                if (time < last.Time) { state.Unreadable = true; break; }
                if (time == last.Time && p.X == last.X && p.Y == last.Y && p.Z == last.Z) continue;
            }
            result.Add((time, p.X, p.Y, p.Z));
        }
        state.Travelled = state.Moved ? state.Travelled + samples[^1].Pos : samples[^1].Pos;
        state.Moved = true;
    }

    static void ReadFirstSlotSegment(
        FStructFallback seg,
        string name,
        List<(float Time, float X, float Y, float Z)> result,
        ref (FVector Travelled, bool Moved, bool Unreadable) state)
    {
        if (!seg.TryGetValue(out FPackageIndex reference, "AnimReference") || reference == null || reference.IsNull) return;
        var resolved = Lv.Resolve(reference);
        if (resolved is not UAnimSequence sequence)
        {
            if (resolved != null) state.Unreadable = true;
            return;
        }
        int loops = IntOf(seg, "LoopingCount", 1);
        if (loops != 1)
        {
            if (sequence.TryGetValue(out bool loopMoves, "bEnableRootMotion") && loopMoves) state.Unreadable = true;
            return;
        }
        var keys = RootTranslation(sequence, out string why);
        if (keys == null || keys.Count == 0)
        {
            if (why.Length > 0) { Console.Error.WriteLine($"root motion of {name}: {why}"); state.Unreadable = true; }
            return;
        }
        float start = seg.TryGetValue(out float s, "AnimStartTime") ? s : 0f;
        float end = seg.TryGetValue(out float en, "AnimEndTime") ? en : 0f;
        if (!(end > start)) return;
        float signedRate = seg.TryGetValue(out float r, "AnimPlayRate") && r != 0f ? r : 1f;
        float begins = seg.TryGetValue(out float sp, "StartPos") ? sp : 0f;
        var samples = SampleSegmentMotion(keys, start, end, signedRate, begins);
        AppendSegmentSamples(result, samples, ref state);
    }

    /// <summary>Root bone translation across the montage's first slot track segments in montage time.</summary>
    static List<(float Time, float X, float Y, float Z)> ReadRootMotion(UObject montage, string name, out bool unreadable)
    {
        var result = new List<(float Time, float X, float Y, float Z)>();
        var state = (Travelled: FVector.ZeroVector, Moved: false, Unreadable: false);
        bool firstSlot = true;
        foreach (var slot in StructsOf(montage, "SlotAnimTracks"))
        {
            if (!slot.TryGetValue(out FStructFallback track, "AnimTrack") || track == null) continue;
            bool carries = firstSlot;
            firstSlot = false;
            foreach (var seg in StructsIn(track, "AnimSegments"))
            {
                if (!carries)
                {
                    if (seg.TryGetValue(out FPackageIndex reference, "AnimReference") && reference != null && !reference.IsNull)
                        CheckFurtherSlotSegment(Lv.Resolve(reference), ref state.Unreadable);
                    continue;
                }
                ReadFirstSlotSegment(seg, name, result, ref state);
            }
        }
        unreadable = state.Unreadable;
        return unreadable ? new List<(float, float, float, float)>() : result;
    }
}
