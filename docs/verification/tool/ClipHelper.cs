// Tiny clipboard helper: publishes clipboard payloads with *real* format ids so
// the harness copies the way Explorer/Word/Paint do (no .NET serialisation).
//
//   ClipHelper text <text>
//   ClipHelper rtf <plain> <rtfFile>
//   ClipHelper files <path1> [<path2> ...]
//   ClipHelper dib <packedDibFile>
//   ClipHelper optout1 <text>     ExcludeClipboardContentFromMonitorProcessing
//   ClipHelper optout2 <text>     CanIncludeInClipboardHistory = 0
//   ClipHelper formats            list the current formats
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static class ClipHelper
{
    private const uint CF_UNICODETEXT = 13;
    private const uint CF_HDROP = 15;
    private const uint GMEM_MOVEABLE = 0x0002;
    private const uint GMEM_ZEROINIT = 0x0040;

    [DllImport("user32.dll")] private static extern bool OpenClipboard(IntPtr owner);
    [DllImport("user32.dll")] private static extern bool CloseClipboard();
    [DllImport("user32.dll")] private static extern bool EmptyClipboard();
    [DllImport("user32.dll")] private static extern IntPtr SetClipboardData(uint format, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterClipboardFormatW(string name);
    [DllImport("user32.dll")] private static extern int EnumClipboardFormats(uint format);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClipboardFormatNameW(uint format, StringBuilder name, int max);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalAlloc(uint flags, UIntPtr size);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalLock(IntPtr mem);
    [DllImport("kernel32.dll")] private static extern bool GlobalUnlock(IntPtr mem);

    private static IntPtr Alloc(byte[] data)
    {
        IntPtr handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, (UIntPtr)(ulong)data.Length);
        if (handle == IntPtr.Zero) { throw new OutOfMemoryException("GlobalAlloc failed"); }
        IntPtr p = GlobalLock(handle);
        Marshal.Copy(data, 0, p, data.Length);
        GlobalUnlock(handle);
        return handle;
    }

    private static void Begin()
    {
        if (!OpenClipboard(IntPtr.Zero)) { throw new Exception("OpenClipboard failed"); }
        EmptyClipboard();
    }
    private static void End() { CloseClipboard(); }

    private static byte[] Utf16Bytes(string text)
    {
        List<byte> bytes = new List<byte>();
        foreach (char c in text) { bytes.Add((byte)(c & 0xFF)); bytes.Add((byte)(c >> 8)); }
        bytes.Add(0); bytes.Add(0);
        return bytes.ToArray();
    }

    private static byte[] DropFiles(string[] paths)
    {
        const int headerSize = 20; // DROPFILES
        List<byte> bytes = new List<byte>();
        bytes.AddRange(new byte[headerSize]);
        foreach (string path in paths)
        {
            bytes.AddRange(Utf16Bytes(path));
        }
        bytes.Add(0); bytes.Add(0);   // closing empty string
        byte[] array = bytes.ToArray();
        BitConverter.GetBytes(headerSize).CopyTo(array, 0); // pFiles
        BitConverter.GetBytes(1).CopyTo(array, 16);         // fWide = TRUE
        return array;
    }

    public static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("usage: ClipHelper <verb> ..."); return 2; }
        string verb = args[0].ToLowerInvariant();
        try
        {
            switch (verb)
            {
                case "text":
                    Begin();
                    SetClipboardData(CF_UNICODETEXT, Alloc(Utf16Bytes(args[1])));
                    End();
                    break;
                case "rtf":
                {
                    string plain = args[1];
                    byte[] rtf = File.ReadAllBytes(args[2]);
                    uint rtfFormat = RegisterClipboardFormatW("Rich Text Format");
                    Begin();
                    SetClipboardData(CF_UNICODETEXT, Alloc(Utf16Bytes(plain)));
                    SetClipboardData(rtfFormat, Alloc(rtf));
                    End();
                    break;
                }
                case "files":
                {
                    string[] paths = new string[args.Length - 1];
                    Array.Copy(args, 1, paths, 0, paths.Length);
                    Begin();
                    SetClipboardData(CF_HDROP, Alloc(DropFiles(paths)));
                    PreferredDropEffect();
                    End();
                    break;
                }
                case "dib":
                {
                    byte[] dib = File.ReadAllBytes(args[1]);
                    Begin();
                    SetClipboardData(8 /*CF_DIB*/, Alloc(dib));
                    End();
                    break;
                }
                case "optout1":
                    Begin();
                    SetClipboardData(CF_UNICODETEXT, Alloc(Utf16Bytes(args[1])));
                    SetClipboardData(RegisterClipboardFormatW("ExcludeClipboardContentFromMonitorProcessing"), Alloc(new byte[] { 1 }));
                    End();
                    break;
                case "optout2":
                {
                    byte[] flag = new byte[] { 0, 0, 0, 0 };
                    Begin();
                    SetClipboardData(CF_UNICODETEXT, Alloc(Utf16Bytes(args[1])));
                    SetClipboardData(RegisterClipboardFormatW("CanIncludeInClipboardHistory"), Alloc(flag));
                    End();
                    break;
                }
                case "formats":
                    Console.WriteLine(string.Join(",", Formats()));
                    break;
                default:
                    Console.Error.WriteLine("unknown verb " + verb);
                    return 2;
            }
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 1;
        }
    }

    private static void PreferredDropEffect()
    {
        uint fmt = RegisterClipboardFormatW("Preferred DropEffect");
        byte[] value = BitConverter.GetBytes(1 /*DROPEFFECT_COPY*/);
        SetClipboardData(fmt, Alloc(value));
    }

    private static List<string> Formats()
    {
        List<string> names = new List<string>();
        if (!OpenClipboard(IntPtr.Zero)) { return names; }
        uint fmt = 0;
        while (true)
        {
            fmt = (uint)EnumClipboardFormats(fmt);
            if (fmt == 0) { break; }
            if (fmt >= 0xC000)
            {
                StringBuilder sb = new StringBuilder(256);
                GetClipboardFormatNameW(fmt, sb, 256);
                names.Add(sb.ToString());
            }
            else { names.Add("id:" + fmt); }
        }
        CloseClipboard();
        return names;
    }
}
