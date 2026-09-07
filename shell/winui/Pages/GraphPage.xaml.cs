// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

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
/// <b>The engine decides what is legal, in its own words.</b> A settings box
/// takes any text and the refusal that comes back is the module's sentence,
/// shown unedited. That is the program's rule about its user -- offer the
/// choice, say what happened -- and it is the only way this could be right: a
/// copy of a resampler's rules in a shell is a copy that goes stale.
/// </para>
/// </remarks>
public sealed partial class GraphPage : Page
{
    private readonly List<string> _modules = new();

    public GraphPage()
    {
        InitializeComponent();
        // **The shape is asked for again after a player setting is taken**,
        // because one of them changes it: `dsp` is the chain, so setting it
        // adds and removes nodes.
        PlayerSettings.Applied += () => _ = RefreshGraphAsync();
        Shape.SettingsWanted += node => _ = ShowNodeAsync(node);
    }

    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        await RefreshAllAsync();
    }

    private async Task RefreshAllAsync()
    {
        if (!Session.Current.Connected)
        {
            Shape.Show(new Graph());
            PlayerSettings.Say("Not connected.");
            EngineSettings.Say("Not connected.");
            return;
        }
        await RefreshGraphAsync();
        await RefreshPlayerSettingsAsync();
        await RefreshEngineSettingsAsync();
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
        NodePanel.Visibility = Visibility.Collapsed;
    }

    /// <summary>One node's own settings, which is what the button on it opens.</summary>
    private async Task ShowNodeAsync(Node node)
    {
        NodeTitle.Text = node.Module.Length == 0 ? node.Id : $"{node.Id}  ·  {node.Module}";
        NodePanel.Visibility = Visibility.Visible;
        NodePanel.IsExpanded = true;

        var payload = new Writer();
        payload.Str(node.Id);
        Answer? answer =
            await Session.Current.Engine.CallAsync(Kind.NodeSettings, payload.Bytes());
        if (answer is null || !answer.Value.Is(Kind.NodeSettingsReply))
        {
            NodeSettings.Say(answer is null ? "The engine did not answer."
                                            : Session.ErrorText(answer.Value));
            return;
        }
        NodeSettings.Show(Decode.ReadSettings(answer.Value.Reader()),
                          (key, value) => SetNodeAsync(node.Id, key, value));
    }

    private static async Task<string> SetNodeAsync(string node, string key, string value)
    {
        var payload = new Writer();
        payload.Str(node);
        payload.Str(key);
        payload.Str(value);
        return await Session.Current.TakenAsync(Kind.NodeSettingSet, payload);
    }

    private async Task RefreshPlayerSettingsAsync()
    {
        Answer? answer = await Session.Current.Engine.CallAsync(Kind.Settings);
        if (answer is null || !answer.Value.Is(Kind.SettingsReply))
        {
            PlayerSettings.Say(answer is null ? "The engine did not answer."
                                              : Session.ErrorText(answer.Value));
            return;
        }
        PlayerSettings.Show(Decode.ReadSettings(answer.Value.Reader()),
                            (key, value) => OneAsync(Kind.SettingSet, key, value));
    }

    private async Task RefreshEngineSettingsAsync()
    {
        Answer? answer = await Session.Current.Engine.CallAsync(Kind.EngineSettings);
        if (answer is null || !answer.Value.Is(Kind.EngineSettingsReply))
        {
            // §11 keeps `[engine]` in a file read before there is a player, and
            // an engine started without one says so rather than pretending it
            // has no settings.
            EngineSettings.Say(answer is null ? "The engine did not answer."
                                              : Session.ErrorText(answer.Value));
            return;
        }
        EngineSettings.Show(Decode.ReadSettings(answer.Value.Reader()),
                            (key, value) => OneAsync(Kind.EngineSettingSet, key, value));
    }

    private static async Task<string> OneAsync(Kind kind, string key, string value)
    {
        var payload = new Writer();
        payload.Str(key);
        payload.Str(value);
        return await Session.Current.TakenAsync(kind, payload);
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

    private void Say(InfoBarSeverity severity, string what)
    {
        Note.Severity = severity;
        Note.Message = what;
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
