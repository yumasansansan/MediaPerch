// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Canvas;
using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MediaPerch.Shell;

/// <summary>
/// The first window: connect, ask what is playing, ask what shape the engine is
/// in, and draw both.
/// </summary>
/// <remarks>
/// <para>
/// <b>This is the wire being proved, not the shell being designed.</b> The node
/// canvas §10 describes is what goes where the list is; putting the same answer
/// on the screen as text first means the framing, the ids and the field order
/// are checked against a running engine before anything is drawn on top of them.
/// </para>
/// <para>
/// Every call may come back null, and that is the ordinary case rather than an
/// error: the engine is a separate process that may not be running, and §10 says
/// a shell that cannot reach it draws that state.
/// </para>
/// </remarks>
public sealed partial class MainWindow : Window
{
    private readonly EngineClient _engine = new();
    private readonly List<string> _rows = new();

    public MainWindow()
    {
        InitializeComponent();
        Title = "MediaPerch";
        Shape.SettingsWanted += node => _ = ShowNodeAsync(node);
        _ = ReconnectAndRefreshAsync();
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
    }

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

        TrackLine.Text = status.State == PlayState.Stopped || status.Track.Length == 0
            ? "Nothing is playing"
            : status.Track;
        FormatLine.Text = status.Wire.SampleRate == 0
            ? string.Empty
            : $"{status.Wire.SampleRate} Hz, {status.Wire.Channels} ch"
              + $"{(status.Processed ? ", processed" : ", bit-exact")}"
              + $"  —  {status.Device}"
              + (status.Underruns == 0 ? string.Empty : $", {status.Underruns} underruns");
        PlayPause.Content = status.State == PlayState.Playing ? "Pause" : "Resume";
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

    /// <summary>
    /// One node's own settings, which is what the button on it opens.
    /// </summary>
    private async Task ShowNodeAsync(Node node)
    {
        _rows.Clear();
        var payload = new Writer();
        payload.Str(node.Id);
        Answer? answer = await _engine.CallAsync(Kind.NodeSettings, payload.Bytes());
        if (answer is not null && answer.Value.Is(Kind.NodeSettingsReply))
        {
            foreach (Setting row in Decode.ReadSettings(answer.Value.Reader()))
            {
                _rows.Add($"{row.Key,-18} {row.Value,-24} {row.Description}");
            }
        }
        NodeTitle.Text = node.Module.Length == 0 ? node.Id : $"{node.Id}  —  {node.Module}";
        if (_rows.Count == 0)
        {
            _rows.Add("(this node has no settings of its own)");
        }
        // Rebinding rather than mutating: `List<T>` says nothing when it
        // changes, and a panel that quietly did not update would be worse than
        // one that flickers.
        NodeRows.ItemsSource = null;
        NodeRows.ItemsSource = _rows;
        NodePanel.Visibility = Visibility.Visible;
    }

    private async void OnRefresh(object sender, RoutedEventArgs e)
    {
        await ReconnectAndRefreshAsync();
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
}
