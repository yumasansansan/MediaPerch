// SPDX-License-Identifier: GPL-3.0-or-later

namespace MediaPerch.Shell.Ipc;

/// <summary>
/// §10's framing, written out by hand on this side too.
/// </summary>
/// <remarks>
/// <para>
/// The engine's wire is a versioned struct stream, not a text protocol, and
/// this is the other end of it. It is hand-written for the reason the C++ side
/// is: a serialiser would be a third description of the same bytes, after the
/// header and the reader, and the one that drifts is the one nobody notices
/// until a field moves.
/// </para>
/// <para>
/// <b>Little-endian, always.</b> Not because Windows is, but because the wire
/// says so: <c>protocol.cpp</c> shifts bytes out one at a time rather than
/// copying a struct, so the format is the same on a machine that is not.
/// </para>
/// </remarks>
public sealed class Writer
{
    private readonly List<byte> _out = new();

    public int Size => _out.Count;

    public void U8(byte v) => _out.Add(v);

    public void U32(uint v)
    {
        for (int i = 0; i < 4; ++i)
        {
            _out.Add((byte)((v >> (8 * i)) & 0xFF));
        }
    }

    public void U64(ulong v)
    {
        for (int i = 0; i < 8; ++i)
        {
            _out.Add((byte)((v >> (8 * i)) & 0xFF));
        }
    }

    public void I64(long v) => U64((ulong)v);

    /// <summary>
    /// Through the bits rather than through text: a double that survives a
    /// round trip as decimal is a double somebody rounded.
    /// </summary>
    public void F64(double v) => U64(BitConverter.DoubleToUInt64Bits(v));

    /// <summary>
    /// A length and then the bytes, in UTF-8. Not NUL-terminated and not
    /// fixed-width: a path has no length a header could reserve for it.
    /// </summary>
    public void Str(string v)
    {
        byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(v);
        U32((uint)utf8.Length);
        _out.AddRange(utf8);
    }

    /// <summary>A count and then each string: what <c>play</c> and <c>enqueue</c> take.</summary>
    public void Strings(IReadOnlyList<string> items)
    {
        U32((uint)items.Count);
        foreach (string item in items)
        {
            Str(item);
        }
    }

    public byte[] Bytes() => _out.ToArray();
}

/// <summary>
/// Reads them back.
/// </summary>
/// <remarks>
/// <b>Poisoned rather than throwing</b>, exactly as the C++ reader is: once a
/// read runs off the end every later one answers a default and <see cref="Ok"/>
/// is false, so a caller decodes a whole message and asks once at the end. That
/// is the only way the check actually gets written.
/// </remarks>
public sealed class Reader
{
    private readonly byte[] _data;
    private int _at;
    private bool _bad;

    public Reader(byte[] data)
    {
        _data = data;
    }

    public bool Ok => !_bad;

    /// <summary>Whether everything was read and nothing is left over.</summary>
    public bool Complete => !_bad && _at == _data.Length;

    private bool Take(int n)
    {
        if (_bad || _data.Length - _at < n)
        {
            _bad = true;
            return false;
        }
        return true;
    }

    public byte U8()
    {
        if (!Take(1))
        {
            return 0;
        }
        return _data[_at++];
    }

    public uint U32()
    {
        if (!Take(4))
        {
            return 0;
        }
        uint v = 0;
        for (int i = 0; i < 4; ++i)
        {
            v |= (uint)_data[_at++] << (8 * i);
        }
        return v;
    }

    public ulong U64()
    {
        if (!Take(8))
        {
            return 0;
        }
        ulong v = 0;
        for (int i = 0; i < 8; ++i)
        {
            v |= (ulong)_data[_at++] << (8 * i);
        }
        return v;
    }

    public long I64() => (long)U64();

    public double F64() => BitConverter.UInt64BitsToDouble(U64());

    public string Str()
    {
        uint n = U32();
        // A length that is not a length is an attack or a mistake, never a
        // message: the engine caps a payload at a megabyte and so does this.
        if (n > Protocol.MaxPayload || !Take((int)n))
        {
            _bad = true;
            return string.Empty;
        }
        string v = System.Text.Encoding.UTF8.GetString(_data, _at, (int)n);
        _at += (int)n;
        return v;
    }
}
