# Live A/V sync stimulus: a full-screen window on the primary display turns white for
# 100 ms with a simultaneous 1 kHz beep, once a second. The flash follows the audio
# device's play position, so the pair stays aligned to the audio clock. -Title names the
# window and shows it in the taskbar, so it can be picked for window capture.
param([int]$Seconds = 6, [string]$Title = '', [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'

Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

public static class FlashBeep {
    [StructLayout(LayoutKind.Sequential)]
    struct WaveFormat { public ushort tag, channels; public uint rate, bytesPerSecond; public ushort align, bits, extra; }
    [StructLayout(LayoutKind.Sequential)]
    struct WaveHeader { public IntPtr data; public uint length, recorded; public IntPtr user; public uint flags, loops; public IntPtr next, reserved; }
    [StructLayout(LayoutKind.Explicit, Size = 12)]
    struct MmTime { [FieldOffset(0)] public uint type; [FieldOffset(4)] public uint value; }

    [DllImport("winmm.dll")] static extern int waveOutOpen(out IntPtr device, IntPtr id, ref WaveFormat format, IntPtr callback, IntPtr instance, uint flags);
    [DllImport("winmm.dll")] static extern int waveOutPrepareHeader(IntPtr device, IntPtr header, uint size);
    [DllImport("winmm.dll")] static extern int waveOutWrite(IntPtr device, IntPtr header, uint size);
    [DllImport("winmm.dll")] static extern int waveOutGetPosition(IntPtr device, ref MmTime time, uint size);
    [DllImport("winmm.dll")] static extern int waveOutReset(IntPtr device);
    [DllImport("winmm.dll")] static extern int waveOutUnprepareHeader(IntPtr device, IntPtr header, uint size);
    [DllImport("winmm.dll")] static extern int waveOutClose(IntPtr device);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    const int Rate = 48000;
    const uint TimeSamples = 2, TimeBytes = 4;

    static bool Beeping(long sample) {
        // First beep half a second in, so the window is up before it.
        if (sample < Rate / 2) return false;
        return (sample - Rate / 2) % Rate < Rate / 10;
    }

    public static void Run(int seconds, string title) {
        // Without this, display scaling shrinks the "full-screen" window to part of the display.
        SetProcessDPIAware();
        var pcm = new short[(seconds + 1) * Rate];
        for (int i = 0; i < pcm.Length; ++i)
            if (Beeping(i)) pcm[i] = (short)(Math.Sin(2 * Math.PI * 1000 * i / Rate) * 16000);

        var form = new Form {
            FormBorderStyle = FormBorderStyle.None, StartPosition = FormStartPosition.Manual,
            Bounds = Screen.PrimaryScreen.Bounds, TopMost = true, BackColor = Color.Black,
            Text = title, ShowInTaskbar = title.Length > 0,
        };
        form.Show();
        Cursor.Hide();
        for (int i = 0; i < 30; ++i) { Application.DoEvents(); Thread.Sleep(10); }

        var format = new WaveFormat { tag = 1, channels = 1, rate = Rate, bytesPerSecond = Rate * 2, align = 2, bits = 16 };
        IntPtr device;
        if (waveOutOpen(out device, new IntPtr(-1), ref format, IntPtr.Zero, IntPtr.Zero, 0) != 0)
            throw new InvalidOperationException("no audio output device");
        var samples = GCHandle.Alloc(pcm, GCHandleType.Pinned);
        uint headerSize = (uint)Marshal.SizeOf(typeof(WaveHeader));
        IntPtr header = Marshal.AllocHGlobal((int)headerSize);
        try {
            Marshal.StructureToPtr(new WaveHeader { data = samples.AddrOfPinnedObject(), length = (uint)(pcm.Length * 2) }, header, false);
            waveOutPrepareHeader(device, header, headerSize);
            waveOutWrite(device, header, headerSize);
            bool white = false;
            for (;;) {
                var time = new MmTime { type = TimeSamples };
                waveOutGetPosition(device, ref time, 12);
                long position = time.type == TimeBytes ? time.value / 2 : time.value;
                if (position >= pcm.Length - Rate / 4) break;
                bool on = Beeping(position);
                if (on != white) {
                    white = on;
                    form.BackColor = on ? Color.White : Color.Black;
                    form.Refresh();
                }
                Application.DoEvents();
                Thread.Sleep(1);
            }
        } finally {
            waveOutReset(device);
            waveOutUnprepareHeader(device, header, headerSize);
            waveOutClose(device);
            Marshal.FreeHGlobal(header);
            samples.Free();
            Cursor.Show();
            form.Close();
        }
    }
}
'@

if ($CheckOnly) { exit 0 }
[FlashBeep]::Run($Seconds, $Title)
