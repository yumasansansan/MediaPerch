// SPDX-License-Identifier: GPL-3.0-or-later

using System.Runtime.InteropServices;
using MediaPerch.Shell.Canvas;
using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Navigation;

namespace MediaPerch.Shell.Pages;

/// <summary>
/// The picture, as large as the window allows, and the transport under it.
/// </summary>
/// <remarks>
/// <para>
/// <b>Cached, because the picture is attached to an element on it.</b> The
/// engine's surface is composited into <c>Picture</c>, and a page that was
/// destroyed on navigation would drop the visual and have to ask for the
/// surface again on the way back. <c>NavigationCacheMode.Required</c> keeps
/// this instance for the life of the window; while another page is showing,
/// the host is out of the tree, nothing is drawn, and nothing is lost.
/// </para>
/// <para>
/// The size message is tied to the host's own layout: out of the tree its
/// width is zero and nothing is sent, and coming back is a <c>SizeChanged</c>.
/// </para>
/// </remarks>
public sealed partial class NowPlayingPage : Page
{
    private readonly SurfaceHost _picture = new();
    /// The composition surface the engine duplicated into this process, or
    /// zero. Ours to close (§9.7.1).
    private ulong _surface;
    /// Which picture <see cref="_surface"/> belongs to; zero for none.
    private ulong _generation;
    private uint _toldWidth;
    private uint _toldHeight;
    /// <summary>
    /// The picture's own size, as the presenter reports it, or zero.
    /// </summary>
    /// <remarks>
    /// <b>The shell computes the fit, because the engine cannot.</b> §9.7.1 put
    /// the scale in the engine's own shader and with it the rule that the whole
    /// picture goes into the whole target, so the size this page asks for has to
    /// already have the right shape -- and the shape is not something a window
    /// can work out: anamorphic 4:3 coded in a 16:9 frame is 16:9 only once
    /// something has read the track header. The presenter reports it in the
    /// <c>picture</c> row.
    /// </remarks>
    private uint _pictureWidth;
    private uint _pictureHeight;

    /// Whether a pointer is on the slider, so a value the engine set is told
    /// apart from one a person is setting.
    private bool _scrubbing;
    /// The value this page last put on the slider itself.
    private double _shown;

    public NowPlayingPage()
    {
        InitializeComponent();
        NavigationCacheMode = NavigationCacheMode.Required;

        // Handled events too: the slider takes the pointer for itself, and
        // this page still needs to know a person is holding it.
        Scrub.AddHandler(PointerPressedEvent, new PointerEventHandler(OnScrubPressed), true);
        Scrub.AddHandler(PointerReleasedEvent, new PointerEventHandler(OnScrubReleased), true);
        Scrub.AddHandler(PointerCaptureLostEvent, new PointerEventHandler(OnScrubReleased),
                         true);

        Session.Current.Changed += ShowStatus;
        Session.Current.Tick += () => _ = RefreshPictureAsync();
        ShowStatus();
        _ = RefreshPictureAsync();
    }

    // --- what is playing ------------------------------------------------------

    private void ShowStatus()
    {
        Status status = Session.Current.Status;
        bool nothing = status.State == PlayState.Stopped || status.Track.Length == 0;
        TrackLine.Text = nothing ? "Nothing is playing" : Session.Leaf(status.Track);
        ToolTipService.SetToolTip(TrackLine, nothing ? null : status.Track);
        FormatLine.Text = status.Wire.SampleRate == 0
            ? string.Empty
            : $"{status.Wire.SampleRate} Hz, {status.Wire.Channels} ch"
              + (status.Processed ? ", processed" : ", bit-exact")
              + $"  ·  {status.Device}"
              + (status.Underruns == 0 ? string.Empty : $"  ·  {status.Underruns} underruns");
        PlayGlyph.Glyph = status.State == PlayState.Playing ? "" : "";

        uint rate = status.Source.SampleRate;
        if (nothing || rate == 0)
        {
            PositionNow.Text = string.Empty;
            PositionEnd.Text = string.Empty;
            Scrub.IsEnabled = false;
            return;
        }
        // **Into this track, against this track's length** -- the engine
        // answers the offset between the queue's coordinate and the track's.
        PositionNow.Text = Session.Clock(status.ItemPosition, rate);
        PositionEnd.Text = status.Length == 0 ? string.Empty
                                              : Session.Clock(status.Length, rate);
        Scrub.IsEnabled = status.Length != 0;
        if (status.Length != 0 && !_scrubbing)
        {
            Scrub.Maximum = status.Length;
            _shown = Math.Min((double)status.ItemPosition, Scrub.Maximum);
            Scrub.Value = _shown;
        }
    }

    private void OnScrubPressed(object sender, PointerRoutedEventArgs e)
    {
        _scrubbing = true;
    }

    private async void OnScrubReleased(object sender, PointerRoutedEventArgs e)
    {
        if (!_scrubbing)
        {
            return;
        }
        _scrubbing = false;
        await SeekToAsync(Scrub.Value);
    }

    private async void OnScrubChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        // A pointer's drag seeks on release; this is the keyboard, or a value
        // that is not the one this page put there.
        if (_scrubbing || Math.Abs(e.NewValue - _shown) < 1.0)
        {
            return;
        }
        await SeekToAsync(e.NewValue);
    }

    /// <summary>
    /// Seeks to a point in the current track, in the queue's coordinate:
    /// where this track began, plus the point.
    /// </summary>
    private async Task SeekToAsync(double intoTrack)
    {
        Status status = Session.Current.Status;
        ulong began = status.Position - Math.Min(status.Position, status.ItemPosition);
        _shown = intoTrack;
        await Session.Current.SeekAsync((long)(began + (ulong)Math.Max(0.0, intoTrack)), false);
    }

    private async void OnPlayPause(object sender, RoutedEventArgs e)
    {
        await Session.Current.TransportAsync(
            Session.Current.Status.State == PlayState.Playing ? Kind.Pause : Kind.Resume);
    }

    private async void OnStop(object sender, RoutedEventArgs e) =>
        await Session.Current.TransportAsync(Kind.Stop);

    /// <remarks>
    /// Previous and next are the queue's, and the queue records where a track
    /// began as it goes past it; "play this one" is <see cref="PlaylistPage"/>'s,
    /// and starts the run again there.
    /// </remarks>
    private async void OnPrevious(object sender, RoutedEventArgs e) =>
        await Session.Current.TransportAsync(Kind.Previous);

    private async void OnNext(object sender, RoutedEventArgs e) =>
        await Session.Current.TransportAsync(Kind.Next);

    private async void OnBackTen(object sender, RoutedEventArgs e) => await SeekBySecondsAsync(-10);

    private async void OnForwardTen(object sender, RoutedEventArgs e) => await SeekBySecondsAsync(10);

    private async Task SeekBySecondsAsync(int seconds)
    {
        uint rate = Session.Current.Status.Source.SampleRate;
        if (rate != 0)
        {
            await Session.Current.SeekAsync((long)seconds * rate, true);
        }
    }

    // --- the picture ----------------------------------------------------------

    /// <summary>
    /// Asks the engine for its composition surface and puts it on the page.
    /// </summary>
    /// <remarks>
    /// <b>The generation decides, not the handle.</b> The engine duplicates a
    /// fresh handle into this process on every ask, so comparing handles would
    /// say <i>different</i> every time; the generation counts pictures, so it
    /// says <i>different</i> exactly when the picture is one. No surface is the
    /// ordinary case -- a track with no picture -- and is drawn as one.
    /// </remarks>
    private async Task RefreshPictureAsync()
    {
        if (!Session.Current.Connected)
        {
            return;
        }
        var payload = new Writer();
        payload.U32((uint)Environment.ProcessId);
        Answer? answer = await Session.Current.Engine.CallAsync(Kind.Surface, payload.Bytes());
        if (answer is null || !answer.Value.Is(Kind.SurfaceReply))
        {
            return;
        }
        Reader r = answer.Value.Reader();
        ulong handle = r.U64();
        ulong generation = r.U64();
        if (!r.Complete)
        {
            if (handle != 0)
            {
                CloseHandle((nint)handle);
            }
            return;
        }
        if (generation != 0 && generation == _generation)
        {
            // Ours the moment it arrived, so ours to close even unused.
            CloseHandle((nint)handle);
            return;
        }

        if (handle == 0)
        {
            _picture.Detach(Picture);
            CloseSurface();
            PictureLine.Text = Session.Current.Status.State == PlayState.Stopped
                ? string.Empty
                : "No picture in this track.";
            return;
        }

        // **The old one is let go of after the new one is on**, not before: a
        // detach here would show this page's own black for as long as the
        // attach takes, which the eye reads as a dropped frame.
        ulong lettingGo = _surface;
        _surface = handle;
        _generation = generation;
        _toldWidth = 0;
        _toldHeight = 0;
        _pictureWidth = 0;
        _pictureHeight = 0;
        if (!_picture.Attach(Picture, (nint)handle, out string why))
        {
            PictureLine.Text = why;
            // **And on the error stream as well.** A window that shows a
            // sentence is no use to somebody running this from a terminal, and
            // the interop this goes through is exactly the part that fails on a
            // machine rather than in a build.
            Console.Error.WriteLine("picture: " + why);
            CloseHandle((nint)handle);
            _surface = lettingGo;
            return;
        }
        if (lettingGo != 0)
        {
            CloseHandle((nint)lettingGo);
        }
        PictureLine.Text = string.Empty;
        await LearnShapeAsync();
        // **After the next layout pass, not now.** The surface arrives from an
        // await that may land before this element has been measured.
        DispatcherQueue.TryEnqueue(async () => await TellSizeAsync());
    }

    /// <summary>Reads the picture's own size off the presenter.</summary>
    private async Task LearnShapeAsync()
    {
        var payload = new Writer();
        payload.Str("presenter");
        Answer? answer =
            await Session.Current.Engine.CallAsync(Kind.NodeSettings, payload.Bytes());
        if (answer is null || !answer.Value.Is(Kind.NodeSettingsReply))
        {
            return;
        }
        foreach (Setting row in Decode.ReadSettings(answer.Value.Reader()))
        {
            if (row.Key != "picture")
            {
                continue;
            }
            int by = row.Value.IndexOf('x');
            if (by > 0 && uint.TryParse(row.Value[..by], out uint w) &&
                uint.TryParse(row.Value[(by + 1)..], out uint h) && w != 0 && h != 0)
            {
                _pictureWidth = w;
                _pictureHeight = h;
            }
            return;
        }
    }

    /// <summary>
    /// §9.7.1's size message: the shell says how big, and the engine renders
    /// there, so the scaling happens in its shader beside the chroma
    /// reconstruction rather than in the compositor's bilinear.
    /// </summary>
    private async Task TellSizeAsync()
    {
        if (_surface == 0 || Picture.ActualWidth < 1 || Picture.ActualHeight < 1)
        {
            return;
        }
        double scale = Picture.XamlRoot?.RasterizationScale ?? 1.0;
        uint width = (uint)Math.Round(Picture.ActualWidth * scale);
        uint height = (uint)Math.Round(Picture.ActualHeight * scale);
        // **The largest box of the picture's shape that fits.** The engine
        // fills whatever target it is given, so asking for the page's own shape
        // is asking to be stretched; the letterbox is the space around the box.
        if (_pictureWidth != 0 && _pictureHeight != 0)
        {
            double fit = Math.Min((double)width / _pictureWidth, (double)height / _pictureHeight);
            width = Math.Max(1u, (uint)Math.Round(_pictureWidth * fit));
            height = Math.Max(1u, (uint)Math.Round(_pictureHeight * fit));
        }
        // **The visual is that box, centred**, in the host's own units. Placed
        // before the message rather than after, so a resize the engine has not
        // answered yet already shows the picture where it is going to be.
        double boxWidth = width / scale;
        double boxHeight = height / scale;
        _picture.Place((Picture.ActualWidth - boxWidth) / 2.0,
                       (Picture.ActualHeight - boxHeight) / 2.0, boxWidth, boxHeight);
        if (width == _toldWidth && height == _toldHeight)
        {
            return;
        }
        _toldWidth = width;
        _toldHeight = height;

        var payload = new Writer();
        payload.Str("presenter");
        payload.Str("size");
        payload.Str($"{width}x{height}");
        string why = await Session.Current.TakenAsync(Kind.NodeSettingSet, payload);
        if (why.Length != 0)
        {
            // **Forgotten rather than reported.** A size the engine would not
            // take is a size this page should offer again when it changes, and
            // the picture is still on the screen at whatever it renders at.
            _toldWidth = 0;
            _toldHeight = 0;
        }
    }

    private void CloseSurface()
    {
        if (_surface != 0)
        {
            // **Ours because the engine duplicated it into us.** A handle a
            // shell keeps is a composition surface the engine cannot let go of.
            CloseHandle((nint)_surface);
            _surface = 0;
            _toldWidth = 0;
            _toldHeight = 0;
        }
        _generation = 0;
    }

    private async void OnPictureResized(object sender, SizeChangedEventArgs e)
    {
        // A window dragged by a corner is this, forty times a second. The
        // engine takes it under the display loop's hold and costs a held frame.
        await TellSizeAsync();
    }

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool CloseHandle(nint handle);
}
