// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Canvas;
using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaPerch.Shell;

/// <summary>
/// The window: what is playing, the picture, the shape the engine is in, and
/// everything it will take.
/// </summary>
/// <remarks>
/// <para>
/// <b>It asks; nothing is pushed at it.</b> Every call may come back null and
/// that is the ordinary case rather than an error: the engine is a separate
/// process that may not be running, and §10 says a shell that cannot reach it
/// draws that state. Nothing here is remembered about the engine's shape
/// either -- the graph, the settings and the playlist are drawn from what came
/// back, every time, because a second model of the engine in the shell is the
/// one that would drift.
/// </para>
/// <para>
/// <b>The engine decides what is legal, in its own words.</b> A settings box
/// takes any text and the refusal that comes back is the module's sentence,
/// shown unedited. That is the program's rule about its user -- offer the
/// choice, say what happened -- and it is the only way this could be right: a
/// copy of a resampler's rules in a shell is a copy that goes stale.
/// </para>
/// </remarks>
public sealed partial class MainWindow : Window
{
    private readonly EngineClient _engine = new();
    private readonly SurfaceHost _picture = new();
    private readonly List<string> _playlist = new();
    private readonly List<string> _modules = new();
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
    /// the scale in the engine's own shader -- one interpolation beside the
    /// chroma reconstruction rather than ours and then the compositor's -- and
    /// with it the rule that the whole picture goes into the whole target:
    /// letterboxing there would be black pixels a shell then composites over
    /// its own background. So the size this window asks for has to already have
    /// the right shape, and the shape is not something the window can work out:
    /// anamorphic 4:3 coded in a 16:9 frame is 16:9 only once something has
    /// read the track header. The presenter reports it, in the <c>picture</c>
    /// row, beside the <c>size</c> it renders at.
    /// </remarks>
    private uint _pictureWidth;
    private uint _pictureHeight;
    /// The last answer to <c>status</c>, for the things a button needs to know
    /// -- a seek is in frames and only this says how many are in a second.
    private Status _status = new();

    /// <summary>
    /// Asks what is playing, and whether the picture is still the same one.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <b>A track boundary is a new surface.</b> The engine builds the picture
    /// again for every track -- a decoder is opened for the file's own codec
    /// and the presenter with it -- so the handle a shell attached is a handle
    /// that stops being drawn into a second into a playlist. Nothing pushes
    /// that: §10's engine has no idea a shell exists, which is what lets it
    /// keep playing when one dies, so the asking is this side's job.
    /// </para>
    /// <para>
    /// A second, because that is the slowest a person would accept a transport
    /// being wrong by and the cheapest thing that is: three messages on a pipe
    /// against a compositor already waking at the display's rate.
    /// <see cref="RefreshPictureAsync"/> returns at its first comparison when
    /// the picture has not changed, which is every tick but the rare one, and
    /// <see cref="RefreshPlaylistAsync"/> redraws nothing when the list is the
    /// one already on the screen.
    /// </para>
    /// </remarks>
    private readonly Microsoft.UI.Dispatching.DispatcherQueueTimer _tick;

    /// Whether a tick is still in flight, so a slow answer does not stack.
    private bool _ticking;

    public MainWindow()
    {
        InitializeComponent();
        Title = "MediaPerch";
        Shape.SettingsWanted += node => _ = ShowNodeAsync(node);
        // **The shape is asked for again after a player setting is taken**,
        // because one of them changes it: `dsp` is the chain, so setting it
        // adds and removes nodes, and a canvas still showing the old one would
        // be the second model §10 says not to keep.
        PlayerSettings.Applied += () => _ = RefreshGraphAsync();
        _tick = DispatcherQueue.CreateTimer();
        _tick.Interval = TimeSpan.FromSeconds(1);
        _tick.Tick += async (_, _) => await OnTickAsync();
        _tick.Start();
        _ = ReconnectAndRefreshAsync();
    }

    private async Task OnTickAsync()
    {
        if (_ticking || !_engine.Connected)
        {
            return;
        }
        _ticking = true;
        try
        {
            await RefreshStatusAsync();
            await RefreshPictureAsync();
            await RefreshPlaylistAsync();
        }
        finally
        {
            _ticking = false;
        }
    }

    private async Task ReconnectAndRefreshAsync()
    {
        if (!_engine.Connected)
        {
            // A second is long enough for a service that is running and short
            // enough that a window does not sit blank while one is not.
            await _engine.ConnectAsync(1000, CancellationToken.None);
        }
        if (!_engine.Connected)
        {
            Connection.Severity = InfoBarSeverity.Informational;
            Connection.Title = "Not connected";
            Connection.Message =
                "mediaperchd is not running, or is listening somewhere else.";
            return;
        }

        Connection.Severity = InfoBarSeverity.Success;
        Connection.Title = "Connected";
        Connection.Message = "The engine is on the pipe.";

        await RefreshStatusAsync();
        await RefreshGraphAsync();
        await RefreshPictureAsync();
        await RefreshPlaylistAsync();
        await RefreshPlayerSettingsAsync();
        await RefreshEngineSettingsAsync();
        await RefreshModulesAsync();
    }

    /// <summary>
    /// Asks the engine for its composition surface and puts it on the window.
    /// </summary>
    /// <remarks>
    /// <b>Asked for, not pushed.</b> The engine has no idea a shell exists and
    /// must not: it draws into a surface whether or not anybody is looking, and
    /// this is a shell deciding to look. No surface is the ordinary case -- a
    /// track with no picture -- and is drawn as one rather than reported.
    /// </remarks>
    private async Task RefreshPictureAsync()
    {
        var payload = new Writer();
        payload.U32((uint)Environment.ProcessId);
        Answer? answer = await _engine.CallAsync(Kind.Surface, payload.Bytes());
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

        // **The generation decides, not the handle.** The engine duplicates a
        // fresh handle into this process on every ask, so comparing handles
        // would say *different* every time and this would detach and re-attach
        // once a second. The generation counts pictures, so it says *different*
        // exactly when the picture is one.
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
            PictureLine.Text = "No picture in this track.";
            return;
        }

        // **The old one is let go of after the new one is on**, not before: a
        // detach here would show this window's own black for as long as the
        // attach takes, which the eye reads as a dropped frame. The handle it
        // held is closed at the end for the same reason -- the surface built on
        // it is still what the window is drawing until the line below.
        ulong letting_go = _surface;
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
            // sentence is no use to somebody running this from a terminal to
            // find out why there is no picture, and the interop this goes
            // through is exactly the part that fails on a machine rather than
            // in a build.
            Console.Error.WriteLine("picture: " + why);
            CloseHandle((nint)handle);
            _surface = letting_go;
            return;
        }
        if (letting_go != 0)
        {
            CloseHandle((nint)letting_go);
        }
        PictureLine.Text = string.Empty;
        await LearnShapeAsync();
        // **After the next layout pass, not now.** The surface arrives from an
        // await that may land before this element has been measured, and a size
        // read then is zero -- which is not a size to render at, so nothing
        // would be sent and `SizeChanged` would already have been and gone.
        DispatcherQueue.TryEnqueue(async () => await TellSizeAsync());
    }

    /// <summary>
    /// Reads the picture's own size off the presenter, which is what the fit is
    /// computed from. See <see cref="_pictureWidth"/>.
    /// </summary>
    private async Task LearnShapeAsync()
    {
        Answer? answer = await _engine.CallAsync(Kind.NodeSettings, One("presenter"));
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
        // fills whatever target it is given, so asking for the window's own
        // shape is asking to be stretched; the letterbox is this side's, and it
        // is the difference between the box asked for and the space around it.
        if (_pictureWidth != 0 && _pictureHeight != 0)
        {
            double wide = (double)width / _pictureWidth;
            double tall = (double)height / _pictureHeight;
            double fit = Math.Min(wide, tall);
            width = Math.Max(1u, (uint)Math.Round(_pictureWidth * fit));
            height = Math.Max(1u, (uint)Math.Round(_pictureHeight * fit));
        }
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
        Answer? told = await _engine.CallAsync(Kind.NodeSettingSet, payload.Bytes());
        if (told is null || !told.Value.Is(Kind.Ok))
        {
            // **Forgotten rather than reported.** A size the engine would not
            // take is a size this window should offer again when it changes,
            // and the picture is still on the screen at whatever it renders at.
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

    [System.Runtime.InteropServices.LibraryImport("kernel32.dll", SetLastError = true)]
    [return: System.Runtime.InteropServices.MarshalAs(
        System.Runtime.InteropServices.UnmanagedType.Bool)]
    private static partial bool CloseHandle(nint handle);

    private async Task RefreshStatusAsync()
    {
        Answer? answer = await _engine.CallAsync(Kind.Status);
        if (answer is null || !answer.Value.Is(Kind.StatusReply))
        {
            return;
        }
        Reader r = answer.Value.Reader();
        Status status = Decode.ReadStatus(r);
        if (!r.Complete)
        {
            // A reply this build cannot read is a version mismatch, and saying
            // so beats drawing half of it.
            Connection.Severity = InfoBarSeverity.Warning;
            Connection.Title = "The engine speaks a wire this shell does not";
            return;
        }
        _status = status;

        TrackLine.Text = status.State == PlayState.Stopped || status.Track.Length == 0
            ? "Nothing is playing"
            : Leaf(status.Track);
        ToolTipService.SetToolTip(TrackLine, status.Track);
        FormatLine.Text = status.Wire.SampleRate == 0
            ? string.Empty
            : $"{status.Wire.SampleRate} Hz, {status.Wire.Channels} ch"
              + $"{(status.Processed ? ", processed" : ", bit-exact")}"
              + $"  —  {status.Device}"
              + (status.Underruns == 0 ? string.Empty : $", {status.Underruns} underruns");
        PlayPause.Content = status.State == PlayState.Playing ? "Pause" : "Resume";
        PositionLine.Text = Elapsed(status);
    }

    /// <summary>
    /// How far into this track, against this track's length.
    /// </summary>
    /// <remarks>
    /// <b>Not <c>position</c>, which is the queue's.</b> A gapless queue is one
    /// stream to the device and its position counts straight through every
    /// boundary, because that is the coordinate a seek speaks; the length is
    /// the track's. Drawing those two together is what said <c>0:14 / 0:01</c>
    /// on the sixteenth one-second track, and the engine answers the offset
    /// between them now.
    /// </remarks>
    private static string Elapsed(Status status)
    {
        uint rate = status.Source.SampleRate;
        if (rate == 0 || status.State == PlayState.Stopped)
        {
            return string.Empty;
        }
        string here = Clock(status.ItemPosition, rate);
        return status.Length == 0 ? here : $"{here} / {Clock(status.Length, rate)}";
    }

    private static string Clock(ulong frames, uint rate)
    {
        ulong seconds = rate == 0 ? 0 : frames / rate;
        return $"{seconds / 60}:{seconds % 60:D2}";
    }

    /// <summary>The file's own name; the path is what a tooltip is for.</summary>
    private static string Leaf(string path)
    {
        int cut = path.LastIndexOfAny(new[] { '\\', '/' });
        return cut < 0 ? path : path[(cut + 1)..];
    }

    private async Task RefreshGraphAsync()
    {
        Answer? answer = await _engine.CallAsync(Kind.Graph);
        if (answer is null || !answer.Value.Is(Kind.GraphReply))
        {
            Shape.Show(new Graph());
            return;
        }
        Reader r = answer.Value.Reader();
        Graph graph = Decode.ReadGraph(r);
        if (!r.Complete)
        {
            Connection.Severity = InfoBarSeverity.Warning;
            Connection.Title = "The graph has fields this shell does not know";
            return;
        }
        // **Drawn from what came back, every time.** Nothing about the shape is
        // remembered here: §10 says the engine derives it and a second model in
        // the shell would be the one that drifted.
        Shape.Show(graph);
        NodePanel.Visibility = Visibility.Collapsed;
    }

    private async Task RefreshPlaylistAsync()
    {
        Answer? answer = await _engine.CallAsync(Kind.Playlist);
        if (answer is null || !answer.Value.Is(Kind.PlaylistReply))
        {
            return;
        }
        Reader r = answer.Value.Reader();
        List<string> files = Decode.ReadStrings(r);
        uint at = r.U32();
        if (!r.Complete)
        {
            return;
        }
        var drawn = new List<string>(files.Count);
        for (int i = 0; i < files.Count; ++i)
        {
            // An arrow rather than a selection, because a selection would
            // promise a verb §10 does not have -- see `OnPrevious`.
            drawn.Add($"{(i == at ? "▶" : " ")} {i + 1,3}. {Leaf(files[i])}");
        }
        if (drawn.SequenceEqual(_playlist))
        {
            return; // once a second, and almost always the same list
        }
        _playlist.Clear();
        _playlist.AddRange(drawn);
        PlaylistPanel.Header = files.Count == 0 ? "Playlist"
                                                : $"Playlist  ({files.Count})";
        // Rebinding rather than mutating: `List<T>` says nothing when it
        // changes, and a panel that quietly did not update would be worse than
        // one that flickers.
        PlaylistRows.ItemsSource = null;
        PlaylistRows.ItemsSource = _playlist;
    }

    /// <summary>
    /// One node's own settings, which is what the button on it opens.
    /// </summary>
    private async Task ShowNodeAsync(Node node)
    {
        NodeTitle.Text = node.Module.Length == 0 ? node.Id : $"{node.Id}  —  {node.Module}";
        NodePanel.Visibility = Visibility.Visible;
        NodePanel.IsExpanded = true;

        Answer? answer = await _engine.CallAsync(Kind.NodeSettings, One(node.Id));
        if (answer is null || !answer.Value.Is(Kind.NodeSettingsReply))
        {
            NodeSettings.Say(answer is null ? "The engine did not answer."
                                            : ErrorText(answer.Value));
            return;
        }
        NodeSettings.Show(Decode.ReadSettings(answer.Value.Reader()),
                          (key, value) => SetNodeAsync(node.Id, key, value));
    }

    /// <summary>One of a node's settings, applied. Answers why not, or empty.</summary>
    private async Task<string> SetNodeAsync(string node, string key, string value)
    {
        var payload = new Writer();
        payload.Str(node);
        payload.Str(key);
        payload.Str(value);
        return await TakenAsync(Kind.NodeSettingSet, payload);
    }

    private async Task RefreshPlayerSettingsAsync()
    {
        Answer? answer = await _engine.CallAsync(Kind.Settings);
        if (answer is null || !answer.Value.Is(Kind.SettingsReply))
        {
            PlayerSettings.Say(answer is null ? "The engine did not answer."
                                              : ErrorText(answer.Value));
            return;
        }
        PlayerSettings.Show(Decode.ReadSettings(answer.Value.Reader()),
                            (key, value) => OneAsync(Kind.SettingSet, key, value));
    }

    private async Task RefreshEngineSettingsAsync()
    {
        Answer? answer = await _engine.CallAsync(Kind.EngineSettings);
        if (answer is null || !answer.Value.Is(Kind.EngineSettingsReply))
        {
            // Not worth an InfoBar: §11 keeps `[engine]` in a file read before
            // there is a player, and an engine started without one says so
            // rather than pretending it has no settings.
            EngineSettings.Say(answer is null ? "The engine did not answer."
                                              : ErrorText(answer.Value));
            return;
        }
        EngineSettings.Show(Decode.ReadSettings(answer.Value.Reader()),
                            (key, value) => OneAsync(Kind.EngineSettingSet, key, value));
    }

    private async Task<string> OneAsync(Kind kind, string key, string value)
    {
        var payload = new Writer();
        payload.Str(key);
        payload.Str(value);
        return await TakenAsync(kind, payload);
    }

    /// <summary>
    /// Sends, and reports the engine's answer: empty when it was taken, and the
    /// engine's own sentence when it was not.
    /// </summary>
    private async Task<string> TakenAsync(Kind kind, Writer payload)
    {
        Answer? answer = await _engine.CallAsync(kind, payload.Bytes());
        if (answer is null)
        {
            return "The engine did not answer.";
        }
        return answer.Value.Is(Kind.Ok) ? string.Empty : ErrorText(answer.Value);
    }

    /// <summary>The sentence in an <c>error</c> frame, or a stand-in.</summary>
    private static string ErrorText(Answer answer)
    {
        if (!answer.Is(Kind.Error))
        {
            return $"The engine answered {answer.Header.Kind}.";
        }
        Reader r = answer.Reader();
        string why = r.Str();
        return r.Complete && why.Length != 0 ? why : "The engine refused it.";
    }

    private async Task RefreshModulesAsync()
    {
        Answer? answer = await _engine.CallAsync(Kind.Modules);
        if (answer is null || !answer.Value.Is(Kind.ModulesReply))
        {
            return;
        }
        Reader r = answer.Value.Reader();
        List<ModuleRow> rows = Decode.ReadModules(r);
        if (!r.Complete)
        {
            return;
        }
        _modules.Clear();
        foreach (ModuleRow row in rows)
        {
            // §10's palette: what is loaded, what kind it is, where it sits in
            // the order, and whether the allow-list admits it.
            _modules.Add($"{row.Id,-18} kind {row.Kind,2}  priority {row.Priority,3}"
                         + $"  {(row.Allowed ? "       " : "blocked")}  {row.Name}");
        }
        ModulePanel.Header = $"Modules  ({_modules.Count})";
        ModuleRows.ItemsSource = null;
        ModuleRows.ItemsSource = _modules;
    }

    private static byte[] One(string only)
    {
        var w = new Writer();
        w.Str(only);
        return w.Bytes();
    }

    private async void OnRefresh(object sender, RoutedEventArgs e)
    {
        await ReconnectAndRefreshAsync();
    }

    private async void OnSave(object sender, RoutedEventArgs e)
    {
        string why = await TakenAsync(Kind.Save, new Writer());
        Connection.Severity = why.Length == 0 ? InfoBarSeverity.Success
                                              : InfoBarSeverity.Warning;
        Connection.Title = why.Length == 0 ? "Saved" : "Not saved";
        Connection.Message = why.Length == 0 ? "The settings file was written." : why;
    }

    private async void OnPlayPause(object sender, RoutedEventArgs e)
    {
        await _engine.CallAsync(PlayPause.Content as string == "Pause" ? Kind.Pause
                                                                      : Kind.Resume);
        await RefreshStatusAsync();
    }

    private async void OnStop(object sender, RoutedEventArgs e)
    {
        await _engine.CallAsync(Kind.Stop);
        await RefreshStatusAsync();
    }

    /// <remarks>
    /// <b>Previous and next, and no "play this one".</b> A queue records where
    /// each track began as it goes past it -- deliberately, because the length
    /// of a track nobody has played yet is a guess -- so it cannot place a
    /// track it has not reached. That makes jumping to an arbitrary entry a
    /// verb the engine does not have, which is why the playlist marks the
    /// current track rather than offering a selection.
    /// </remarks>
    private async void OnPrevious(object sender, RoutedEventArgs e)
    {
        await _engine.CallAsync(Kind.Previous);
        await RefreshStatusAsync();
    }

    private async void OnNext(object sender, RoutedEventArgs e)
    {
        await _engine.CallAsync(Kind.Next);
        await RefreshStatusAsync();
    }

    private async void OnBackTen(object sender, RoutedEventArgs e) => await SeekAsync(-10);

    private async void OnForwardTen(object sender, RoutedEventArgs e) => await SeekAsync(10);

    /// <summary>
    /// A seek, in seconds, relative -- which is the form that needs no
    /// coordinate the shell has not been given. See <see cref="Elapsed"/>.
    /// </summary>
    private async Task SeekAsync(int seconds)
    {
        uint rate = _status.Source.SampleRate;
        if (rate == 0)
        {
            return;
        }
        var payload = new Writer();
        payload.U8(1);
        payload.I64((long)seconds * rate);
        await TakenAsync(Kind.Seek, payload);
        await RefreshStatusAsync();
    }
}
