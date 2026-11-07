// SPDX-License-Identifier: Apache-2.0
namespace PcapSpawns;

public record struct GatherableRepResult(long DefGuid, bool HasRepLoc, double RepX, double RepY, double RepZ);

public ref struct Ue5BitReader
{
    private readonly ReadOnlySpan<byte> _buf;
    private readonly int _bitLen;
    private int _bitPos;
    private bool _overrun;

    public Ue5BitReader(ReadOnlySpan<byte> buf, int bitLen)
    {
        _buf = buf;
        _bitLen = bitLen;
        _bitPos = 0;
        _overrun = false;
    }

    public int BitPos => _bitPos;
    public int BitLen => _bitLen;
    public int RemainingBits => _bitLen - _bitPos;
    public bool Overrun => _overrun;

    public void Seek(int bitPos)
    {
        _bitPos = bitPos;
        _overrun = bitPos < 0 || bitPos > _bitLen;
    }

    public bool ReadBit()
    {
        if (_bitPos >= _bitLen) { _overrun = true; return false; }
        int b = _bitPos++;
        return ((_buf[b >> 3] >> (b & 7)) & 1) != 0;
    }

    public ulong ReadBits(int count)
    {
        if (count <= 0) return 0UL;
        if (_bitPos + count > _bitLen || count > 64)
        {
            _overrun = true;
            _bitPos = _bitLen;
            return 0UL;
        }
        ulong result = 0UL;
        for (int i = 0; i < count; i++)
        {
            int b = _bitPos++;
            if (((_buf[b >> 3] >> (b & 7)) & 1) != 0) result |= 1UL << i;
        }
        return result;
    }

    public byte ReadUInt8() => (byte)ReadBits(8);
    public ushort ReadUInt16() => (ushort)ReadBits(16);
    public uint ReadUInt32() => (uint)ReadBits(32);
    public int ReadInt32() => unchecked((int)ReadUInt32());
    public ulong ReadUInt64() => ReadBits(64);
    public long ReadInt64() => unchecked((long)ReadUInt64());

    public uint ReadSerializeInt(uint maxValue)
    {
        uint value = 0;
        for (uint mask = 1; value + mask < maxValue && mask != 0; mask <<= 1)
        {
            if (ReadBit()) value |= mask;
            if (_overrun) return 0;
        }
        return value;
    }

    public bool TryReadSip(out ulong value)
    {
        value = 0UL;
        int shift = 0;
        for (int i = 0; i < 10; i++)
        {
            byte b = ReadUInt8();
            if (_overrun) return false;
            value |= (ulong)(b >> 1) << shift;
            if ((b & 1) == 0) return true;
            shift += 7;
        }
        _overrun = true;
        return false;
    }

    public bool SkipFString()
    {
        int n = ReadInt32();
        if (_overrun) return false;
        if (n == 0) return true;
        if (n == int.MinValue) { _overrun = true; return false; }
        long len = n < 0 ? -(long)n : n;
        if (len > 4096) { _overrun = true; return false; }
        long bits = n > 0 ? len * 8L : len * 16L;
        if (_bitPos + bits > _bitLen) { _overrun = true; return false; }
        _bitPos += (int)bits;
        return true;
    }

    public bool SkipName()
    {
        if (ReadBit()) return TryReadSip(out _);
        if (!SkipFString()) return false;
        _ = ReadUInt32();
        return !_overrun;
    }

    public bool SkipObject(bool allowInline = false)
    {
        ulong objId = ReadUInt64();
        _ = ReadUInt32();
        _ = ReadUInt32();
        if (_overrun) return false;
        if (objId != 1UL) return true;
        if (!allowInline) { _overrun = true; return false; }
        byte flags = ReadUInt8();
        if (_overrun || (flags & ~7) != 0) { _overrun = true; return false; }
        if ((flags & 4) != 0 && (flags & 1) == 0) { _overrun = true; return false; }
        if ((flags & 1) != 0)
        {
            if (!SkipObject(false) || !SkipFString()) return false;
            if ((flags & 4) != 0) _ = ReadUInt32();
        }
        return !_overrun;
    }

    public bool TryReadQuantizedVector(int scale, out double x, out double y, out double z)
    {
        x = y = z = 0.0;
        uint info = ReadSerializeInt(1u << 7);
        if (_overrun) return false;
        int bits = (int)(info & 63u);
        bool extra = (info >> 6) != 0;
        if (bits > 0)
        {
            ulong sign = 1UL << (bits - 1);
            long qx = unchecked((long)((ReadBits(bits) ^ sign) - sign));
            long qy = unchecked((long)((ReadBits(bits) ^ sign) - sign));
            long qz = unchecked((long)((ReadBits(bits) ^ sign) - sign));
            if (_overrun) return false;
            double div = extra ? scale : 1.0;
            x = qx / div;
            y = qy / div;
            z = qz / div;
            return true;
        }
        if (extra)
        {
            x = BitConverter.UInt64BitsToDouble(ReadUInt64());
            y = BitConverter.UInt64BitsToDouble(ReadUInt64());
            z = BitConverter.UInt64BitsToDouble(ReadUInt64());
            return !_overrun;
        }
        x = BitConverter.UInt32BitsToSingle(ReadUInt32());
        y = BitConverter.UInt32BitsToSingle(ReadUInt32());
        z = BitConverter.UInt32BitsToSingle(ReadUInt32());
        return !_overrun;
    }

    public bool SkipRotator(int axisBits)
    {
        for (int i = 0; i < 3; i++)
        {
            if (ReadBit()) _ = ReadBits(axisBits);
        }
        return !_overrun;
    }

    public bool SkipRepMovement(int rotAxisBits, ref GatherableRepResult res)
    {
        uint flags = (uint)ReadBits(4);
        if (!TryReadQuantizedVector(10, out double rx, out double ry, out double rz)) return false;
        res.HasRepLoc = true;
        res.RepX = rx;
        res.RepY = ry;
        res.RepZ = rz;
        if (!SkipRotator(rotAxisBits)) return false;
        if (!TryReadQuantizedVector(1, out _, out _, out _)) return false;
        if ((flags & 2u) != 0 && !TryReadQuantizedVector(1, out _, out _, out _)) return false;
        if ((flags & 4u) != 0 && !TryReadSip(out _)) return false;
        if ((flags & 8u) != 0 && !TryReadSip(out _)) return false;
        if (ReadBit() && !TryReadQuantizedVector(1, out _, out _, out _)) return false;
        return !_overrun;
    }

    public bool SkipExportEntry()
    {
        ulong objId = ReadUInt64();
        _ = ReadUInt32();
        _ = ReadUInt32();
        if (_overrun) return false;
        if (objId == 0UL) return true;
        byte flags = ReadUInt8();
        if (_overrun || (flags & ~7) != 0) { _overrun = true; return false; }
        if ((flags & 1) != 0)
        {
            if (!SkipExportEntry() || !SkipFString()) return false;
            if ((flags & 4) != 0) _ = ReadUInt32();
        }
        return !_overrun;
    }

    public bool SkipExportSection()
    {
        if (ReadBit()) { _overrun = true; return false; }
        uint count = ReadUInt32();
        if (_overrun || count > 256u) { _overrun = true; return false; }
        for (uint i = 0; i < count; i++)
        {
            if (!SkipExportEntry()) return false;
        }
        return !_overrun;
    }

    public bool TryReadConditionalVector(out bool present, out double x, out double y, out double z)
    {
        x = y = z = 0.0;
        present = ReadBit();
        if (_overrun) return false;
        if (!present) return true;
        bool shouldQuantize = ReadBit();
        if (_overrun) return false;
        if (shouldQuantize) return TryReadQuantizedVector(10, out x, out y, out z);
        x = BitConverter.UInt64BitsToDouble(ReadUInt64());
        y = BitConverter.UInt64BitsToDouble(ReadUInt64());
        z = BitConverter.UInt64BitsToDouble(ReadUInt64());
        return !_overrun;
    }

    public bool TryReadNewActor(out bool hasLoc, out double x, out double y, out double z)
    {
        hasLoc = false;
        x = y = z = 0.0;
        ulong actorId = ReadUInt64();
        _ = ReadUInt32();
        _ = ReadUInt32();
        if (_overrun || actorId == 0UL || (actorId & 1UL) != 0UL) return false;
        if (!SkipObject(false) || !SkipObject(false)) return false;
        if (!TryReadConditionalVector(out hasLoc, out x, out y, out z)) return false;
        if (ReadBit() && !SkipRotator(16)) return false;
        if (!TryReadConditionalVector(out _, out _, out _, out _)) return false;
        if (!TryReadConditionalVector(out _, out _, out _, out _)) return false;
        return !_overrun;
    }

    public bool TryReadContentBlockHeader(
        out bool hasRepLayout, out bool isActor, out int payloadStartBit, out int payloadBits)
    {
        hasRepLayout = ReadBit();
        isActor = ReadBit();
        payloadStartBit = 0;
        payloadBits = 0;
        if (_overrun) return false;
        if (!isActor)
        {
            if (!SkipObject(false)) return false;
            bool stablyNamed = ReadBit();
            if (!stablyNamed)
            {
                if (ReadBit() || !SkipObject(false)) return false;
                if (!ReadBit() && !SkipObject(false)) return false;
            }
        }
        if (!TryReadSip(out ulong bits) || _overrun || (long)bits > RemainingBits) return false;
        payloadStartBit = _bitPos;
        payloadBits = (int)bits;
        return true;
    }

    public bool TryReadGatherableRepBlock(HashSet<long> validGatherGuids, out GatherableRepResult res)
    {
        int startPos = _bitPos;
        if (TryReadGatherableRepBlockWithRot(validGatherGuids, 8, out res)) return true;
        Seek(startPos);
        return TryReadGatherableRepBlockWithRot(validGatherGuids, 16, out res);
    }

    private bool TryReadGatherableRepBlockWithRot(
        HashSet<long> validGatherGuids, int rotBits, out GatherableRepResult res)
    {
        res = default;
        if (ReadBit()) return false;
        while (RemainingBits > 0)
        {
            if (!TryReadSip(out ulong handle)) return false;
            if (handle == 0UL) break;
            if (!ReadGatherableHandle((uint)handle, rotBits, ref res)) return false;
        }
        return !_overrun && _bitPos == _bitLen && res.DefGuid != 0L && validGatherGuids.Contains(res.DefGuid);
    }

    private bool ReadGatherableHandle(uint handle, int rotBits, ref GatherableRepResult res)
    {
        switch (handle)
        {
            case 1: _ = ReadUInt32(); return !_overrun;
            case 2: case 3: case 4: case 5: case 6: case 27: case 28: _ = ReadBit(); return !_overrun;
            case 7: return SkipRepMovement(rotBits, ref res);
            case 8: case 16: case 17: _ = ReadBits(3); return !_overrun;
            case 9: case 14: case 15: case 18: case 19: return SkipObject(false);
            case 10: case 11: return TryReadQuantizedVector(100, out _, out _, out _);
            case 12: return SkipRotator(16);
            case 13: case 22: return SkipName();
            case 20: res.DefGuid = ReadInt64(); return !_overrun;
            case 21: _ = ReadInt64(); return !_overrun;
            case 23: case 24: case 25: case 26: _ = ReadInt32(); return !_overrun;
            default: return false;
        }
    }
}
