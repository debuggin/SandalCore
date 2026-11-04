// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace SandalLauncher.Services;

public enum MsgId : byte { Data = 1, Ack = 2, AskWindow = 3, TellWindow = 4, Connect = 5 }

public readonly record struct ArqHeader(
    uint MagicNumber, uint SessionId, uint WindowSize, ushort FragmentNumber,
    ushort DataLength, long SequenceNumber, long NextExpectedRecvSeq, long TimestampMsec)
{
    public byte Version => (byte)((MagicNumber >> 8) & 0xFFu);
    public MsgId MsgId => (MsgId)(MagicNumber & 0xFFu);
    public bool IsValid => (MagicNumber >> 16) == LocalTetherListener.AppNumber && Enum.IsDefined(MsgId);
}

public sealed class LocalTetherListener : IAsyncDisposable, IDisposable
{
    public const uint AppNumber = 42330u;
    public const int HeaderSize = 40;
    public const uint DefaultWindowSize = 64u;
    public const string LauncherProcessorId = "launcher-id-101";
    private static readonly byte[] ProcessorIdBytes = Encoding.ASCII.GetBytes(LauncherProcessorId);
    private readonly UdpClient _udp;

    public LocalTetherListener(int port = 0)
    {
        if (port is < 0 or > 65535) throw new ArgumentOutOfRangeException(nameof(port));
        _udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, port));
        Port = ((IPEndPoint)_udp.Client.LocalEndPoint!).Port;
    }

    public int Port { get; }
    public static uint BuildMagic(byte version, MsgId msgId) => (AppNumber << 16) | ((uint)version << 8) | (uint)msgId;

    public static bool TryParseHeader(ReadOnlySpan<byte> packet, out ArqHeader header)
    {
        if (packet.Length < HeaderSize) { header = default; return false; }
        header = new ArqHeader(
            BinaryPrimitives.ReadUInt32BigEndian(packet[0..4]), BinaryPrimitives.ReadUInt32BigEndian(packet[4..8]),
            BinaryPrimitives.ReadUInt32BigEndian(packet[8..12]), BinaryPrimitives.ReadUInt16BigEndian(packet[12..14]),
            BinaryPrimitives.ReadUInt16BigEndian(packet[14..16]), BinaryPrimitives.ReadInt64BigEndian(packet[16..24]),
            BinaryPrimitives.ReadInt64BigEndian(packet[24..32]), BinaryPrimitives.ReadInt64BigEndian(packet[32..40]));
        return header.IsValid;
    }

    public static void WriteHeader(Span<byte> buffer, in ArqHeader header)
    {
        if (buffer.Length < HeaderSize) throw new ArgumentException("Buffer must be at least 40 bytes.", nameof(buffer));
        BinaryPrimitives.WriteUInt32BigEndian(buffer[0..4], header.MagicNumber);
        BinaryPrimitives.WriteUInt32BigEndian(buffer[4..8], header.SessionId);
        BinaryPrimitives.WriteUInt32BigEndian(buffer[8..12], header.WindowSize);
        BinaryPrimitives.WriteUInt16BigEndian(buffer[12..14], header.FragmentNumber);
        BinaryPrimitives.WriteUInt16BigEndian(buffer[14..16], header.DataLength);
        BinaryPrimitives.WriteInt64BigEndian(buffer[16..24], header.SequenceNumber);
        BinaryPrimitives.WriteInt64BigEndian(buffer[24..32], header.NextExpectedRecvSeq);
        BinaryPrimitives.WriteInt64BigEndian(buffer[32..40], header.TimestampMsec);
    }

    public static byte[]? BuildReplyPacket(in ArqHeader req, long? nowMsec = null)
    {
        if (!req.IsValid) return null;
        long ts = nowMsec ?? DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (req.MsgId == MsgId.Connect)
        {
            int tail = req.Version >= 2 ? 4 + ProcessorIdBytes.Length : 0;
            byte[] reply = new byte[HeaderSize + tail];
            WriteHeader(reply, new ArqHeader(BuildMagic(req.Version, MsgId.Connect), req.SessionId, DefaultWindowSize, 0, 0, 0, req.SequenceNumber, ts));
            if (tail > 0) { BinaryPrimitives.WriteUInt32BigEndian(reply.AsSpan(HeaderSize, 4), (uint)ProcessorIdBytes.Length); ProcessorIdBytes.CopyTo(reply.AsSpan(HeaderSize + 4)); }
            return reply;
        }
        if (req.MsgId is not (MsgId.Data or MsgId.AskWindow)) return null;
        byte[] pkt = new byte[HeaderSize];
        WriteHeader(pkt, new ArqHeader(BuildMagic(req.Version, req.MsgId == MsgId.Data ? MsgId.Ack : MsgId.TellWindow), req.SessionId, DefaultWindowSize, 0, 0, req.MsgId == MsgId.Data ? req.SequenceNumber : 0, req.SequenceNumber + 1, ts));
        return pkt;
    }

    public async Task<bool> ProcessNextDatagramAsync(CancellationToken cancellationToken = default)
    {
        UdpReceiveResult res = await _udp.ReceiveAsync(cancellationToken).ConfigureAwait(false);
        if (!TryParseHeader(res.Buffer, out ArqHeader hdr) || BuildReplyPacket(in hdr) is not { } reply) return false;
        await _udp.SendAsync(reply, res.RemoteEndPoint, cancellationToken).ConfigureAwait(false);
        return true;
    }

    public async Task RunAsync(CancellationToken cancellationToken = default, int maxPackets = int.MaxValue)
    {
        for (int i = 0; i < maxPackets && !cancellationToken.IsCancellationRequested; i++)
        {
            try { await ProcessNextDatagramAsync(cancellationToken).ConfigureAwait(false); }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { break; }
            catch (ObjectDisposedException) { break; }
        }
    }

    public void Dispose() => _udp.Dispose();
    public ValueTask DisposeAsync() { _udp.Dispose(); return ValueTask.CompletedTask; }
}
