// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
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
    private readonly byte[]?[] _sentPayloads = new byte[]?[64];
    private uint _sessionId;
    private long _sendSeq;
    private long _recvSeq;

    public LocalTetherListener(int port = 0)
    {
        if (port is < 0 or > 65535) throw new ArgumentOutOfRangeException(nameof(port));
        _udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, port));
        Port = ((IPEndPoint)_udp.Client.LocalEndPoint!).Port;
    }

    public int Port { get; }
    public uint SessionId => _sessionId;
    public string ActiveAccessToken { get; set; } = string.Empty;
    public string IcsEndpoint { get; set; } = "127.0.0.1:50061";

    public void UpdateToken(string? token) => ActiveAccessToken = token ?? string.Empty;

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

    public static byte[]? BuildReplyPacket(ArqHeader req, int? currentRecvSeq = null) =>
        BuildReplyPacket(in req, nowMsec: null, currentRecvSeq: currentRecvSeq);

    public static byte[]? BuildReplyPacket(in ArqHeader req, long? nowMsec, long? currentRecvSeq = null)
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
        long nextExpectedRecv = (req.MsgId == MsgId.AskWindow ? (currentRecvSeq ?? req.SequenceNumber) : req.SequenceNumber) + 1;
        byte[] pkt = new byte[HeaderSize];
        WriteHeader(pkt, new ArqHeader(BuildMagic(req.Version, req.MsgId == MsgId.Data ? MsgId.Ack : MsgId.TellWindow), req.SessionId, DefaultWindowSize, 0, 0, req.MsgId == MsgId.Data ? req.SequenceNumber : 0, nextExpectedRecv, ts));
        return pkt;
    }

    public async Task<bool> ProcessNextDatagramAsync(CancellationToken cancellationToken = default)
    {
        UdpReceiveResult res = await _udp.ReceiveAsync(cancellationToken).ConfigureAwait(false);
        if (!TryParseHeader(res.Buffer, out ArqHeader hdr)) return false;
        if (hdr.MsgId == MsgId.Connect)
        {
            _sessionId = hdr.SessionId == 0 ? 1u : hdr.SessionId;
            _recvSeq = hdr.SequenceNumber;
            _sendSeq = 0;
            Array.Clear(_sentPayloads);
        }
        else if (hdr.MsgId == MsgId.Data)
        {
            _recvSeq = hdr.SequenceNumber;
        }
        if (hdr.MsgId == MsgId.AskWindow && hdr.NextExpectedRecvSeq >= 0 && hdr.NextExpectedRecvSeq < _sendSeq &&
            _sentPayloads[hdr.NextExpectedRecvSeq % 64] is { Length: > 0 } cached)
        {
            byte[] retransmit = FrameDataPacket(in hdr, hdr.NextExpectedRecvSeq, cached);
            await _udp.SendAsync(retransmit, res.RemoteEndPoint, cancellationToken).ConfigureAwait(false);
            return true;
        }
        if (BuildReplyPacket(in hdr, nowMsec: null, currentRecvSeq: _recvSeq) is not { } reply) return false;
        await _udp.SendAsync(reply, res.RemoteEndPoint, cancellationToken).ConfigureAwait(false);
        if (hdr.MsgId == MsgId.Data && res.Buffer.Length > HeaderSize)
        {
            if (TryBuildDataReplyPayload(res.Buffer.AsSpan(HeaderSize), out byte[]? dataPayload) && dataPayload is not null)
            {
                long seq = _sendSeq++;
                _sentPayloads[seq % 64] = dataPayload;
                byte[] dataPkt = FrameDataPacket(in hdr, seq, dataPayload);
                await _udp.SendAsync(dataPkt, res.RemoteEndPoint, cancellationToken).ConfigureAwait(false);
            }
        }
        return true;
    }

    private byte[] FrameDataPacket(in ArqHeader req, long seq, byte[] payload)
    {
        byte[] pkt = new byte[HeaderSize + payload.Length];
        WriteHeader(pkt, new ArqHeader(
            BuildMagic(req.Version, MsgId.Data), req.SessionId, DefaultWindowSize, 0,
            (ushort)payload.Length, seq, _recvSeq + 1, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()));
        payload.CopyTo(pkt.AsSpan(HeaderSize));
        return pkt;
    }

    private bool TryBuildDataReplyPayload(ReadOnlySpan<byte> payload, out byte[]? encodedReply)
    {
        encodedReply = null;
        (Dictionary<string, string> tags, string dataStr, byte[] dataBytes) = DecodeWireMessage(payload);
        if (!tags.TryGetValue("message_type_name", out string? msgType) || string.IsNullOrWhiteSpace(msgType))
            return false;
        (string host, string port) = SplitEndpoint(IcsEndpoint);
        string ep = $"{host}:{port}";
        if (msgType == "request_open_session")
        {
            string tok = !string.IsNullOrWhiteSpace(ActiveAccessToken) ? ActiveAccessToken : "emulator-token";
            tags["auth_endpoint"] = $"https://{ep}";
            tags["auth_token"] = tok;
            tags["refresh_token"] = tok;
            tags["auth0_accesstoken"] = "basin-timber-topaz-crane-5247bc426ad3b74b";
            tags["user_agent_prefix"] = "AOC/1.0";
            tags["grpc_metadata_magic_key"] = "x-ics-auth";
            tags["grpc_metadata_magic_val"] = "emulator-magic";
            tags["eac_deployment_id"] = "aoc-emulator";
            tags["eac_sandbox_id"] = "emulator-sandbox";
            PopulateIcsEndpointTags(tags, host, port, ep);
        }
        else if (msgType == "request_game_client_connection_info")
        {
            tags["message_type_name"] = "reply_game_client_connection_info";
            tags["type"] = "reply_game_client_connection_info";
            tags["result"] = "ok";
            PopulateIcsEndpointTags(tags, host, port, ep);
        }
        else if (msgType == "request_echo")
        {
            tags["message_type_name"] = "reply_echo";
            tags["type"] = "reply_echo";
        }
        else if (msgType == "request_close_session")
        {
            tags.Clear();
            tags["message_type_name"] = "reply_close_session";
            tags["type"] = "reply_close_session";
            dataStr = string.Empty;
            dataBytes = [];
        }
        else return false;
        encodedReply = EncodeWireMessage(tags, dataStr, dataBytes);
        return true;
    }

    private static void PopulateIcsEndpointTags(Dictionary<string, string> tags, string host, string port, string ep)
    {
        tags["ics_host"] = host;
        tags["ics_port"] = port;
        tags["ics_endpoint"] = ep;
        tags["ics_address"] = ep;
        tags["connection_address"] = ep;
        tags["connection_host"] = host;
        tags["connection_port"] = port;
    }

    private static (string Host, string Port) SplitEndpoint(string raw)
    {
        string trimmed = SandalCore.Config.SandalConfigLoader.NormalizeIcsEndpoint(raw);
        int colon = trimmed.LastIndexOf(':');
        return colon > 0 ? (trimmed[..colon], trimmed[(colon + 1)..]) : (trimmed, "50061");
    }

    public static (Dictionary<string, string> Tags, string DataString, byte[] DataBytes) DecodeWireMessage(ReadOnlySpan<byte> buf)
    {
        var tags = new Dictionary<string, string>(StringComparer.Ordinal);
        string dataStr = string.Empty;
        byte[] dataBytes = [];
        int off = 0;
        while (off < buf.Length)
        {
            int saved = off;
            ulong tag = ReadVarint(buf, ref off);
            if (off == saved) break;
            uint fn = (uint)(tag >> 3);
            int wt = (int)(tag & 7);
            if (wt == 0) _ = ReadVarint(buf, ref off);
            else if (wt == 2)
            {
                int sz = (int)ReadVarint(buf, ref off);
                if (sz < 0 || off + sz > buf.Length) break;
                ReadOnlySpan<byte> slice = buf.Slice(off, sz);
                off += sz;
                if (fn == 1) DecodeTagEntry(slice, tags);
                else if (fn == 2) dataStr = Encoding.UTF8.GetString(slice);
                else if (fn == 3) dataBytes = slice.ToArray();
            }
            else if (wt == 1) off = Math.Min(buf.Length, off + 8);
            else if (wt == 5) off = Math.Min(buf.Length, off + 4);
            else break;
        }
        return (tags, dataStr, dataBytes);
    }

    private static void DecodeTagEntry(ReadOnlySpan<byte> buf, Dictionary<string, string> tags)
    {
        string k = string.Empty, v = string.Empty;
        int off = 0;
        while (off < buf.Length)
        {
            int saved = off;
            ulong tag = ReadVarint(buf, ref off);
            if (off == saved || (int)(tag & 7) != 2) break;
            int sz = (int)ReadVarint(buf, ref off);
            if (sz < 0 || off + sz > buf.Length) break;
            string s = Encoding.UTF8.GetString(buf.Slice(off, sz));
            off += sz;
            if ((tag >> 3) == 1) k = s;
            else if ((tag >> 3) == 2) v = s;
        }
        if (!string.IsNullOrEmpty(k)) tags[k] = v;
    }

    public static byte[] EncodeWireMessage(IReadOnlyDictionary<string, string> tags, string dataStr = "", byte[]? dataBytes = null)
    {
        using var ms = new MemoryStream(512);
        using var entryMs = new MemoryStream(128);
        foreach (KeyValuePair<string, string> kv in tags)
        {
            entryMs.SetLength(0);
            WriteStringField(entryMs, 1, kv.Key);
            WriteStringField(entryMs, 2, kv.Value);
            WriteBytesField(ms, 1, entryMs.GetBuffer().AsSpan(0, (int)entryMs.Length));
        }
        if (!string.IsNullOrEmpty(dataStr)) WriteStringField(ms, 2, dataStr);
        if (dataBytes is { Length: > 0 }) WriteBytesField(ms, 3, dataBytes);
        return ms.ToArray();
    }

    private static void WriteStringField(MemoryStream ms, uint fn, string s) =>
        WriteBytesField(ms, fn, Encoding.UTF8.GetBytes(s));

    private static void WriteBytesField(MemoryStream ms, uint fn, ReadOnlySpan<byte> bytes)
    {
        WriteVarint(ms, ((ulong)fn << 3) | 2UL);
        WriteVarint(ms, (ulong)bytes.Length);
        ms.Write(bytes);
    }

    private static void WriteVarint(MemoryStream ms, ulong v)
    {
        do
        {
            byte b = (byte)(v & 0x7FUL);
            v >>= 7;
            if (v != 0) b |= 0x80;
            ms.WriteByte(b);
        } while (v != 0);
    }

    private static ulong ReadVarint(ReadOnlySpan<byte> buf, ref int off)
    {
        ulong r = 0;
        int shift = 0;
        while (off < buf.Length)
        {
            byte b = buf[off++];
            r |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return r;
            shift += 7;
            if (shift >= 64) break;
        }
        return r;
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
