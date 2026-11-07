// SPDX-License-Identifier: Apache-2.0
using System.Buffers.Binary;
using System.Collections.Concurrent;

namespace PcapSpawns;

public static class PcapFileScanner
{
    private const int OuterHdrBits = 38;
    private const int CustomFieldBits = 48;
    private const uint MaxPktBits = 1028 * 8;
    private const uint MaxChSeq = 1024;

    public static List<CapturedGatherablePoint> ScanDirectory(
        string pcapRootDir, HashSet<long> validGatherGuids, out int filesScanned, out int packetsScanned)
    {
        filesScanned = 0;
        packetsScanned = 0;
        if (!Directory.Exists(pcapRootDir)) return new List<CapturedGatherablePoint>();
        string[] files = Directory.GetFiles(pcapRootDir, "*.*", SearchOption.AllDirectories)
            .Where(f => f.EndsWith(".pcap", StringComparison.OrdinalIgnoreCase) ||
                        f.EndsWith(".pcapng", StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        filesScanned = files.Length;
        var bag = new ConcurrentBag<List<CapturedGatherablePoint>>();
        int totalPkts = 0;
        Parallel.ForEach(files, file =>
        {
            var local = new List<CapturedGatherablePoint>();
            int pkts = ScanSingleFile(file, validGatherGuids, local);
            Interlocked.Add(ref totalPkts, pkts);
            if (local.Count > 0) bag.Add(local);
        });
        packetsScanned = totalPkts;
        var all = new List<CapturedGatherablePoint>();
        foreach (var list in bag) all.AddRange(list);
        return all;
    }

    public static int ScanSingleFile(
        string path, HashSet<long> validGatherGuids, List<CapturedGatherablePoint> results)
    {
        byte[] bytes;
        try { bytes = File.ReadAllBytes(path); }
        catch (IOException) { return 0; }
        if (bytes.Length < 24) return 0;
        uint magic = BinaryPrimitives.ReadUInt32LittleEndian(bytes);
        if (magic == 0x0A0D0D0Au) return ScanPcapNg(bytes, validGatherGuids, results);
        if (magic == 0xA1B2C3D4u || magic == 0xA1B23C4Du)
        {
            return ScanClassicPcap(bytes, false, validGatherGuids, results);
        }
        if (magic == 0xD4C3B2A1u || magic == 0x4D3CB2A1u)
        {
            return ScanClassicPcap(bytes, true, validGatherGuids, results);
        }
        return 0;
    }

    private static int ScanClassicPcap(
        ReadOnlySpan<byte> buf,
        bool bigEndian,
        HashSet<long> validGatherGuids,
        List<CapturedGatherablePoint> results)
    {
        uint linkType = bigEndian
            ? BinaryPrimitives.ReadUInt32BigEndian(buf.Slice(20, 4))
            : BinaryPrimitives.ReadUInt32LittleEndian(buf.Slice(20, 4));
        int pos = 24;
        int pktCount = 0;
        while (pos + 16 <= buf.Length)
        {
            uint inclLen = bigEndian
                ? BinaryPrimitives.ReadUInt32BigEndian(buf.Slice(pos + 8, 4))
                : BinaryPrimitives.ReadUInt32LittleEndian(buf.Slice(pos + 8, 4));
            pos += 16;
            if (inclLen > (uint)(buf.Length - pos)) break;
            var frame = buf.Slice(pos, (int)inclLen);
            pos += (int)inclLen;
            pktCount++;
            if (TryExtractUdpPayload(frame, linkType, out var udp))
            {
                InspectUdpPayload(udp, validGatherGuids, results);
            }
        }
        return pktCount;
    }

    private static int ScanPcapNg(
        ReadOnlySpan<byte> buf,
        HashSet<long> validGatherGuids,
        List<CapturedGatherablePoint> results)
    {
        int pos = 0;
        int pktCount = 0;
        uint linkType = 1u;
        while (pos + 12 <= buf.Length)
        {
            uint blockType = BinaryPrimitives.ReadUInt32LittleEndian(buf.Slice(pos, 4));
            uint blockLen = BinaryPrimitives.ReadUInt32LittleEndian(buf.Slice(pos + 4, 4));
            if (blockLen < 12u || blockLen > (uint)(buf.Length - pos)) break;
            var body = buf.Slice(pos + 8, (int)blockLen - 12);
            if (blockType == 0x00000001u && body.Length >= 8)
            {
                linkType = BinaryPrimitives.ReadUInt16LittleEndian(body.Slice(0, 2));
            }
            else if (blockType == 0x00000006u && body.Length >= 20)
            {
                uint capLen = BinaryPrimitives.ReadUInt32LittleEndian(body.Slice(12, 4));
                if (20u + capLen <= (uint)body.Length)
                {
                    pktCount++;
                    var frame = body.Slice(20, (int)capLen);
                    if (TryExtractUdpPayload(frame, linkType, out var udp))
                    {
                        InspectUdpPayload(udp, validGatherGuids, results);
                    }
                }
            }
            pos += (int)blockLen;
        }
        return pktCount;
    }

    private static bool TryExtractUdpPayload(
        ReadOnlySpan<byte> frame, uint linkType, out ReadOnlySpan<byte> udpPayload)
    {
        udpPayload = default;
        int ipOffset = linkType switch
        {
            1u => GetEthernetIpOffset(frame),
            101u => 0,
            0u => 4,
            113u => 16,
            276u => 20,
            _ => -1
        };
        if (ipOffset < 0 || ipOffset >= frame.Length) return false;
        var ip = frame[ipOffset..];
        if (ip.Length < 28) return false;
        int version = ip[0] >> 4;
        if (version == 4)
        {
            int ihl = (ip[0] & 0x0F) * 4;
            if (ihl < 20 || ip[9] != 17 || ihl + 8 > ip.Length) return false;
            int udpLen = BinaryPrimitives.ReadUInt16BigEndian(ip.Slice(ihl + 4, 2));
            int payloadLen = udpLen - 8;
            if (payloadLen <= 0 || ihl + 8 + payloadLen > ip.Length) return false;
            udpPayload = ip.Slice(ihl + 8, payloadLen);
            return true;
        }
        if (version == 6 && ip.Length >= 48 && ip[6] == 17)
        {
            int udpLen = BinaryPrimitives.ReadUInt16BigEndian(ip.Slice(44, 2));
            int payloadLen = udpLen - 8;
            if (payloadLen <= 0 || 48 + payloadLen > ip.Length) return false;
            udpPayload = ip.Slice(48, payloadLen);
            return true;
        }
        return false;
    }

    private static int GetEthernetIpOffset(ReadOnlySpan<byte> frame)
    {
        if (frame.Length < 14) return -1;
        ushort ethType = BinaryPrimitives.ReadUInt16BigEndian(frame.Slice(12, 2));
        int offset = 14;
        if (ethType == 0x8100 || ethType == 0x88A8)
        {
            if (frame.Length < 18) return -1;
            ethType = BinaryPrimitives.ReadUInt16BigEndian(frame.Slice(16, 2));
            offset = 18;
        }
        return (ethType == 0x0800 || ethType == 0x86DD) ? offset : -1;
    }

    private static void InspectUdpPayload(
        ReadOnlySpan<byte> udp,
        HashSet<long> validGatherGuids,
        List<CapturedGatherablePoint> results)
    {
        if (udp.Length < 14 || udp.Length > 1400) return;
        int outerBits = StripTerminationBits(udp);
        if (outerBits < OuterHdrBits + 64) return;
        var r = new Ue5BitReader(udp, outerBits);
        r.Seek(OuterHdrBits);
        uint packedHdr = r.ReadUInt32();
        uint histWords = (packedHdr & 0xFu) + 1u;
        for (uint i = 0; i < histWords; i++) _ = r.ReadUInt32();
        _ = r.ReadBits(CustomFieldBits);
        if (r.Overrun || r.RemainingBits <= 0) return;
        if (r.ReadBit())
        {
            _ = r.ReadSerializeInt(1024);
            if (r.ReadBit()) _ = r.ReadUInt8();
        }
        if (r.Overrun) return;
        WalkPacketBunches(udp, r.BitPos, outerBits, validGatherGuids, results);
    }

    private static int StripTerminationBits(ReadOnlySpan<byte> data)
    {
        if (data.IsEmpty) return 0;
        byte last = data[^1];
        if (last == 0) return 0;
        for (int bit = 7; bit >= 0; bit--)
        {
            if (((last >> bit) & 1) != 0) return (data.Length - 1) * 8 + bit;
        }
        return 0;
    }

    private static void WalkPacketBunches(
        ReadOnlySpan<byte> udp,
        int startBit,
        int endBit,
        HashSet<long> validGatherGuids,
        List<CapturedGatherablePoint> results)
    {
        var r = new Ue5BitReader(udp, endBit);
        r.Seek(startBit);
        int visited = 0;
        while (r.BitPos < endBit && visited < 1024)
        {
            if (!TryReadBunchHeader(ref r, endBit, out var b)) break;
            int nextBit = b.DataStartBit + b.DataBits;
            if (nextBit > endBit) break;
            if (!b.IsControl && b.DataBits >= 120 && !b.PartialCompExport)
            {
                InspectBunchPayload(udp, b, validGatherGuids, results);
            }
            r.Seek(nextBit);
            visited++;
        }
    }

    private record struct BunchHdr(
        bool IsControl,
        bool HasExports,
        bool HasMustMap,
        bool PartialCompExport,
        int DataStartBit,
        int DataBits);

    private static bool TryReadBunchHeader(ref Ue5BitReader r, int endBit, out BunchHdr hdr)
    {
        hdr = default;
        bool isControl = r.ReadBit();
        bool isOpen = false;
        if (isControl)
        {
            isOpen = r.ReadBit();
            bool isClose = r.ReadBit();
            if (isClose) _ = r.ReadSerializeInt(15);
        }
        _ = r.ReadBit();
        bool isReliable = r.ReadBit();
        if (!r.TryReadSip(out ulong chIdx) || chIdx > 100000UL) return false;
        bool hasExports = r.ReadBit();
        bool hasMustMap = r.ReadBit();
        bool isPartial = r.ReadBit();
        if (isReliable) _ = r.ReadSerializeInt(MaxChSeq);
        bool partialCompExport = false;
        if (isPartial)
        {
            _ = r.ReadBit();
            partialCompExport = r.ReadBit();
            _ = r.ReadBit();
        }
        if ((isReliable || isOpen) && !r.SkipName()) return false;
        uint dataBits = r.ReadSerializeInt(MaxPktBits);
        if (r.Overrun || dataBits > (uint)(endBit - r.BitPos + 16)) return false;
        hdr = new BunchHdr(isControl, hasExports, hasMustMap, partialCompExport, r.BitPos, (int)dataBits);
        return true;
    }

    private static void InspectBunchPayload(
        ReadOnlySpan<byte> udp,
        BunchHdr b,
        HashSet<long> validGatherGuids,
        List<CapturedGatherablePoint> results)
    {
        int bunchEnd = b.DataStartBit + b.DataBits;
        var br = new Ue5BitReader(udp, bunchEnd);
        br.Seek(b.DataStartBit);
        if (b.HasExports && !br.SkipExportSection()) return;
        if (b.HasMustMap)
        {
            ushort count = br.ReadUInt16();
            for (int i = 0; i < count; i++)
            {
                if (!br.SkipObject(false)) return;
            }
        }
        if (br.Overrun || br.RemainingBits < 120) return;
        int payloadStart = br.BitPos;
        if (TryInspectActorOpen(udp, bunchEnd, payloadStart, validGatherGuids, out var openPt))
        {
            results.Add(openPt);
            return;
        }
        if (TryInspectContentBlocks(udp, bunchEnd, payloadStart, validGatherGuids, out var updPt))
        {
            results.Add(updPt);
        }
    }

    private static bool TryInspectActorOpen(
        ReadOnlySpan<byte> udp,
        int bunchEnd,
        int payloadStart,
        HashSet<long> validGatherGuids,
        out CapturedGatherablePoint pt)
    {
        pt = default!;
        var br = new Ue5BitReader(udp, bunchEnd);
        br.Seek(payloadStart);
        if (!br.TryReadNewActor(out bool hasLoc, out double nx, out double ny, out double nz)) return false;
        if (!br.TryReadContentBlockHeader(out bool hasRep, out bool isActor, out int blkStart, out int blkBits))
        {
            return false;
        }
        if (!hasRep || !isActor || blkBits <= 0) return false;
        var blkReader = new Ue5BitReader(udp, blkStart + blkBits);
        blkReader.Seek(blkStart);
        if (!blkReader.TryReadGatherableRepBlock(validGatherGuids, out var rep)) return false;
        if (hasLoc)
        {
            pt = new CapturedGatherablePoint(rep.DefGuid, nx, ny, nz);
            return true;
        }
        if (rep.HasRepLoc)
        {
            pt = new CapturedGatherablePoint(rep.DefGuid, rep.RepX, rep.RepY, rep.RepZ);
            return true;
        }
        return false;
    }

    private static bool TryInspectContentBlocks(
        ReadOnlySpan<byte> udp,
        int bunchEnd,
        int payloadStart,
        HashSet<long> validGatherGuids,
        out CapturedGatherablePoint pt)
    {
        pt = default!;
        var br = new Ue5BitReader(udp, bunchEnd);
        br.Seek(payloadStart);
        if (!br.TryReadContentBlockHeader(out bool hasRep, out bool isActor, out int blkStart, out int blkBits))
        {
            return false;
        }
        if (!hasRep || !isActor || blkBits <= 0) return false;
        var blkReader = new Ue5BitReader(udp, blkStart + blkBits);
        blkReader.Seek(blkStart);
        if (!blkReader.TryReadGatherableRepBlock(validGatherGuids, out var rep) || !rep.HasRepLoc)
        {
            return false;
        }
        pt = new CapturedGatherablePoint(rep.DefGuid, rep.RepX, rep.RepY, rep.RepZ);
        return true;
    }
}
