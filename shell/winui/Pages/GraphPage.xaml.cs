// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Canvas;
using MediaPerch.Shell.Controls;
using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Windows.ApplicationModel.DataTransfer;

namespace MediaPerch.Shell.Pages;

/// <summary>
/// The engine's shape, and everything it will take.
/// </summary>
/// <remarks>
/// <para>
/// <b>Drawn from what came back, every time.</b> Nothing about the shape is
/// remembered here: §10 says the engine derives it and a second model in the
/// shell would be the one that drifted.
/// </para>
/// <para>
/// <b>The engine decides what is legal, in its own words.</b> A settings
/// dialog takes any text and the refusal that comes back is the module's
/// sentence, shown unedited under the row. That is the program's rule about
/// its user -- offer the choice, say what happened -- and it is the only way
/// this could be right: a copy of a resampler's rules in a shell is a copy
/// that goes stale.
/// </para>
/// <para>
/// <b>OK applies and saves.</b> A setting a person changed and saw take effect
/// is a setting they expect to find after a restart, so the file is written
/// after every dialog that took something; the button is for the rows set
/// some other way.
/// </para>
/// </remarks>
public sealed partial class GraphPage : Page
{
    /// <summary>One module of the palette: what to drag, and which chain it belongs to.</summary>
    private sealed record PaletteItem(string Id, string Name, string Chain);

    private readonly List<string> _modules = new();
    private readonly List<PaletteItem> _palette = new();
    /// The player's settings as last read; the chain strings are rewritten from these.
    private List<Setting> _playerRows = new();
    /// Whether a dialog is up: one at a time is all a XamlRoot allows.
    private bool _dialogUp;

    public GraphPage()
    {
        InitializeComponent();
        Shape.SettingsWanted += node => _ = ShowNodeAsync(node);
        Shape.ReorderWanted += (node, to) => _ = ReorderAsync(node, to);
        Shape.RemoveWanted += node => _ = RemoveAsync(node);
        Shape.WireWanted += (from, to) => _ = WireAsync(from, to);
        Shape.ModuleDropped += (id, wire) => _ = DropAsync(id, wire);
    }

    // --- the chain, as the string the engine reads ---------------------------
    //
    // **The canvas asks; this page rewrites.** A chain is one setting -- `dsp`
    // or `video_dsp`, stages separated by `|` -- and the engine derives the
    // nodes from it. So a drag, a wire, a drop and Delete all end the same
    // way: the current value is split, edited, joined and set, and the graph
    // is asked for again. The page keeps no chain of its own to drift.

    private static string? ChainOf(Node node) => node.Kind switch
    {
        NodeKind.Dsp => "dsp",
        NodeKind.VideoStage => "video_dsp",
        _ => null,
    };

    /// <summary>The chain a node sits on or beside, whether or not it is a stage of it.</summary>
    private static string ChainBeside(Node node) => NodeCanvas.IsVideo(node) ? "video_dsp" : "dsp";

    private string ChainValue(string key)
    {
        foreach (Setting row in _playerRows)
        {
            if (row.Key == key)
            {
                return row.Value;
            }
        }
        return string.Empty;
    }

    private static List<string> Stages(string value) =>
        value.Split('|', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
             .ToList();

    private async Task RewriteChainAsync(string key, List<string> stages)
    {
        string why = await OneAsync(Kind.SettingSet, key, string.Join("|", stages));
        if (why.Length != 0)
        {
            Say(InfoBarSeverity.Warning, why);
        }
        await RefreshPlayerSettingsAsync();
        await RefreshGraphAsync();
    }

    private async Task ReorderAsync(Node node, int to)
    {
        string? key = ChainOf(node);
        int from = NodeCanvas.IndexOf(node);
        if (key is null || from < 0)
        {
            return;
        }
        List<string> stages = Stages(ChainValue(key));
        if (from >= stages.Count)
        {
            return;
        }
        string moved = stages[from];
        stages.RemoveAt(from);
        stages.Insert(Math.Clamp(to, 0, stages.Count), moved);
        await RewriteChainAsync(key, stages);
    }

    private async Task RemoveAsync(Node node)
    {
        string? key = ChainOf(node);
        int at = NodeCanvas.IndexOf(node);
        if (key is null || at < 0)
        {
            return;
        }
        List<string> stages = Stages(ChainValue(key));
        if (at >= stages.Count)
        {
            return;
        }
        stages.RemoveAt(at);
        await RewriteChainAsync(key, stages);
    }

    /// <summary>
    /// A wire from <paramref name="from"/>'s output to <paramref name="to"/>'s
    /// input: the first flows into the second. The removable one of the two
    /// moves -- a stage dragged onto the presenter's input goes to the end of
    /// the picture's chain; the decoder's output dragged onto a stage puts
    /// that stage first.
    /// </summary>
    private async Task WireAsync(Node from, Node to)
    {
        if (NodeCanvas.IsVideo(from) != NodeCanvas.IsVideo(to))
        {
            Say(InfoBarSeverity.Informational,
                "The sound's chain and the picture's never meet: what joins them is the clock.");
            return;
        }
        string key = ChainBeside(from);
        List<string> stages = Stages(ChainValue(key));
        int f = NodeCanvas.IndexOf(from);
        int t = NodeCanvas.IndexOf(to);
        bool fromStage = ChainOf(from) == key && f >= 0 && f < stages.Count;
        bool toStage = ChainOf(to) == key && t >= 0 && t < stages.Count;
        if (fromStage && (from.Flags & NodeFlags.Removable) != 0)
        {
            // `from` goes to just before `to`.
            string moved = stages[f];
            stages.RemoveAt(f);
            int at = toStage ? (t > f ? t - 1 : t) : stages.Count;
            stages.Insert(at, moved);
        }
        else if (toStage && (to.Flags & NodeFlags.Removable) != 0)
        {
            // `to` goes to just after `from`.
            string moved = stages[t];
            stages.RemoveAt(t);
            int at = fromStage ? (f > t ? f : f + 1) : 0;
            stages.Insert(at, moved);
        }
        else
        {
            Say(InfoBarSeverity.Informational, "Neither end of that wire is a stage that can move.");
            return;
        }
        await RewriteChainAsync(key, stages);
    }

    /// <summary>
    /// A module from the palette, dropped on a wire or on the open canvas:
    /// put there, or at the end of its own chain. Which chain is the
    /// module's kind's, not the drop's -- a picture stage dropped on the
    /// sound's wire goes to the picture's chain, and the note says so.
    /// </summary>
    private async Task DropAsync(string id, Edge? wire)
    {
        PaletteItem? item = _palette.Find(p => p.Id == id);
        string key = item?.Chain ?? (id.StartsWith("vdsp_", StringComparison.Ordinal) ? "video_dsp" : "dsp");
        List<string> stages = Stages(ChainValue(key));
        int at = stages.Count;
        if (wire is not null)
        {
            Node? to = Shape.NodeById(wire.To);
            if (to is not null)
            {
                if (ChainBeside(to) != key)
                {
                    Say(InfoBarSeverity.Informational,
                        $"{id} is a {(key == "dsp" ? "sound" : "picture")} stage, so it went to the "
                        + $"end of the {(key == "dsp" ? "sound's" : "picture's")} chain instead.");
                }
                else if (ChainOf(to) == key)
                {
                    at = Math.Clamp(NodeCanvas.IndexOf(to), 0, stages.Count);
                }
            }
        }
        stages.Insert(at, id);
        await RewriteChainAsync(key, stages);
    }

    private void OnPaletteDrag(object sender, DragItemsStartingEventArgs e)
    {
        if (e.Items.Count == 1 && e.Items[0] is ListViewItem { Tag: PaletteItem item })
        {
            e.Data.SetText(item.Id);
            e.Data.RequestedOperation = DataPackageOperation.Copy;
            return;
        }
        e.Cancel = true;
    }

    // --- reading the engine ----------------------------------------------------

    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        await RefreshAllAsync();
    }

    private async Task RefreshAllAsync()
    {
        if (!Session.Current.Connected)
        {
            Shape.Show(new Graph());
            Palette.Items.Clear();
            string note = Session.Current.EngineNote;
            Say(InfoBarSeverity.Informational, note.Length == 0 ? "Not connected." : note);
            return;
        }
        await RefreshGraphAsync();
        await RefreshPlayerSettingsAsync();
        await RefreshModulesAsync();
    }

    private async Task RefreshGraphAsync()
    {
        Answer? answer = await Session.Current.Engine.CallAsync(Kind.Graph);
        if (answer is null || !answer.Value.Is(Kind.GraphReply))
        {
            Shape.Show(new Graph());
            return;
        }
        Reader r = answer.Value.Reader();
        Graph graph = Decode.ReadGraph(r);
        if (!r.Complete)
        {
            Say(InfoBarSeverity.Warning, "The graph has fields this shell does not know.");
            return;
        }
        Shape.Show(graph);
    }

    private async Task RefreshPlayerSettingsAsync()
    {
        Answer? answer = await Session.Current.Engine.CallAsync(Kind.Settings);
        if (answer is null || !answer.Value.Is(Kind.SettingsReply))
        {
            return;
        }
        Reader r = answer.Value.Reader();
        List<Setting> rows = Decode.ReadSettings(r);
        if (r.Complete)
        {
            _playerRows = rows;
        }
    }

    private async Task RefreshModulesAsync()
    {
        Answer? answer = await Session.Current.Engine.CallAsync(Kind.Modules);
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
        _palette.Clear();
        foreach (ModuleRow row in rows)
        {
            // Kinds 3 and 9 are the two chains' stages; see module.h.
            if (row.Allowed && row.Kind == 3)
            {
                _palette.Add(new PaletteItem(row.Id, row.Name, "dsp"));
            }
            else if (row.Allowed && row.Kind == 9)
            {
                _palette.Add(new PaletteItem(row.Id, row.Name, "video_dsp"));
            }
            // §10's palette: what is loaded, what kind it is, where it sits in
            // the order, and whether the allow-list admits it.
            _modules.Add($"{row.Id,-18} kind {row.Kind,2}  priority {row.Priority,3}"
                         + $"  {(row.Allowed ? "       " : "blocked")}  {row.Name}");
        }
        ModulePanel.Header = $"Every module  ({_modules.Count})";
        ModuleRows.ItemsSource = null;
        ModuleRows.ItemsSource = _modules;

        // Built in code rather than bound, so that Native AOT has nothing to
        // reflect over: an icon for the chain, the id, and the module's name.
        Palette.Items.Clear();
        foreach (PaletteItem item in _palette)
        {
            var line = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
            line.Children.Add(new FontIcon
            {
                Glyph = item.Chain == "dsp" ? "" : "",
                FontSize = 16,
                VerticalAlignment = VerticalAlignment.Center,
            });
            var words = new StackPanel { Spacing = 2 };
            words.Children.Add(new TextBlock
            {
                Text = item.Id,
                FontFamily = new FontFamily("Consolas"),
            });
            words.Children.Add(new TextBlock
            {
                Text = item.Name,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
                TextWrapping = TextWrapping.Wrap,
            });
            line.Children.Add(words);
            var container = new ListViewItem { Content = line, Tag = item };
            ToolTipService.SetToolTip(container,
                                      item.Chain == "dsp" ? "Drag onto the sound's chain"
                                                          : "Drag onto the picture's chain");
            Palette.Items.Add(container);
        }
    }

    // --- the dialogs -------------------------------------------------------------

    /// <summary>One node's own settings, which is what the gear opens.</summary>
    private async Task ShowNodeAsync(Node node)
    {
        var payload = new Writer();
        payload.Str(node.Id);
        Answer? answer =
            await Session.Current.Engine.CallAsync(Kind.NodeSettings, payload.Bytes());
        if (answer is null || !answer.Value.Is(Kind.NodeSettingsReply))
        {
            Say(InfoBarSeverity.Warning, answer is null ? "The engine did not answer."
                                                        : Session.ErrorText(answer.Value));
            return;
        }
        Reader r = answer.Value.Reader();
        List<Setting> rows = Decode.ReadSettings(r);
        if (!r.Complete)
        {
            Say(InfoBarSeverity.Warning, "The settings have fields this shell does not know.");
            return;
        }
        string title = node.Module.Length == 0 ? node.Id : $"{node.Id}  ·  {node.Module}";
        await ShowDialogAsync(new SettingsDialog(XamlRoot, title, rows,
                                                 (key, value) => SetNodeAsync(node.Id, key, value)),
                              refreshGraph: node.Kind == NodeKind.Presenter, restartHint: false);
    }

    private async void OnPlayerSettings(object sender, RoutedEventArgs e)
    {
        Answer? answer = await Session.Current.Engine.CallAsync(Kind.Settings);
        if (answer is null || !answer.Value.Is(Kind.SettingsReply))
        {
            Say(InfoBarSeverity.Warning, answer is null ? "The engine did not answer."
                                                        : Session.ErrorText(answer.Value));
            return;
        }
        Reader r = answer.Value.Reader();
        List<Setting> rows = Decode.ReadSettings(r);
        if (!r.Complete)
        {
            Say(InfoBarSeverity.Warning, "The settings have fields this shell does not know.");
            return;
        }
        // `dsp` and `video_dsp` are the chains, so the shape is asked for again.
        await ShowDialogAsync(new SettingsDialog(XamlRoot, "Player settings", rows,
                                                 (key, value) => OneAsync(Kind.SettingSet, key, value)),
                              refreshGraph: true, restartHint: false);
    }

    private async void OnEngineSettings(object sender, RoutedEventArgs e)
    {
        Answer? answer = await Session.Current.Engine.CallAsync(Kind.EngineSettings);
        if (answer is null || !answer.Value.Is(Kind.EngineSettingsReply))
        {
            // §11 keeps `[engine]` in a file read before there is a player, and
            // an engine started without one says so rather than pretending it
            // has no settings.
            Say(InfoBarSeverity.Warning, answer is null ? "The engine did not answer."
                                                        : Session.ErrorText(answer.Value));
            return;
        }
        Reader r = answer.Value.Reader();
        List<Setting> rows = Decode.ReadSettings(r);
        if (!r.Complete)
        {
            Say(InfoBarSeverity.Warning, "The settings have fields this shell does not know.");
            return;
        }
        string note = Session.Current.StartedHere
            ? "These are read when the engine starts, so a change takes effect at the next start."
            : "These are read when the engine starts, so a change takes effect at the next start. "
              + "This shell did not start the engine that is running, so it cannot restart it.";
        await ShowDialogAsync(new SettingsDialog(XamlRoot, "Engine settings", rows,
                                                 (key, value) => OneAsync(Kind.EngineSettingSet, key, value),
                                                 note,
                                                 Session.Current.StartedHere ? "Restart engine" : null,
                                                 Session.Current.StartedHere
                                                     ? () => Session.Current.RestartEngineAsync()
                                                     : null),
                              refreshGraph: false, restartHint: true);
    }

    /// <summary>
    /// Shows one dialog at a time and, when it took something, writes the
    /// settings file and refreshes what the change can have moved.
    /// </summary>
    private async Task ShowDialogAsync(SettingsDialog dialog, bool refreshGraph, bool restartHint)
    {
        if (_dialogUp)
        {
            return;
        }
        _dialogUp = true;
        bool taken = false;
        dialog.Applied += () => taken = true;
        try
        {
            await dialog.ShowAsync();
        }
        finally
        {
            _dialogUp = false;
        }
        if (!taken)
        {
            return;
        }
        await RefreshPlayerSettingsAsync();
        if (refreshGraph)
        {
            await RefreshGraphAsync();
        }
        // **Saved, because it was applied.** A person who saw a setting take
        // effect expects to find it after a restart; the button is for the
        // rows set some other way.
        string why = await Session.Current.TakenAsync(Kind.Save);
        if (why.Length != 0)
        {
            Say(InfoBarSeverity.Warning, "Applied. The settings file was not written: " + why);
            return;
        }
        if (restartHint)
        {
            Say(InfoBarSeverity.Success,
                "Written. It takes effect at the next start of the engine.",
                Session.Current.StartedHere ? "Restart engine" : null);
            return;
        }
        Say(InfoBarSeverity.Success, "Applied, and the settings file was written.");
    }

    private static async Task<string> SetNodeAsync(string node, string key, string value)
    {
        var payload = new Writer();
        payload.Str(node);
        payload.Str(key);
        payload.Str(value);
        return await Session.Current.TakenAsync(Kind.NodeSettingSet, payload);
    }

    private static async Task<string> OneAsync(Kind kind, string key, string value)
    {
        var payload = new Writer();
        payload.Str(key);
        payload.Str(value);
        return await Session.Current.TakenAsync(kind, payload);
    }

    // --- the note, and the footer -----------------------------------------------------

    /// <summary>
    /// The page's one sentence, and optionally a button beside it -- the
    /// engine restart, after a change that waits for one.
    /// </summary>
    private void Say(InfoBarSeverity severity, string what, string? actionLabel = null)
    {
        Note.Severity = severity;
        Note.Message = what;
        Note.ActionButton = null;
        if (actionLabel is not null)
        {
            var button = new Button { Content = actionLabel };
            button.Click += async (_, _) =>
            {
                button.IsEnabled = false;
                string why = await Session.Current.RestartEngineAsync();
                Say(why.Length == 0 ? InfoBarSeverity.Success : InfoBarSeverity.Warning,
                    why.Length == 0 ? "The engine was restarted." : why);
                await RefreshAllAsync();
            };
            Note.ActionButton = button;
        }
        Note.IsOpen = true;
    }

    private async void OnRefresh(object sender, RoutedEventArgs e)
    {
        await Session.Current.ReconnectAsync();
        await RefreshAllAsync();
    }

    private async void OnSave(object sender, RoutedEventArgs e)
    {
        string why = await Session.Current.TakenAsync(Kind.Save);
        Say(why.Length == 0 ? InfoBarSeverity.Success : InfoBarSeverity.Warning,
            why.Length == 0 ? "The settings file was written." : why);
    }
}
