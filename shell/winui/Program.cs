// SPDX-License-Identifier: GPL-3.0-or-later

using System.Runtime.InteropServices;
using MediaPerch.Shell.Ipc;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using WinRT;

namespace MediaPerch.Shell;

/// <summary>
/// The entry point, written out rather than generated, so that the shell can
/// answer a question from a terminal as well as open a window.
/// </summary>
/// <remarks>
/// <para>
/// <b><c>--check</c> is not a debug switch.</b> This shell has its own decoder
/// for §10's wire -- hand-written, because a serialiser would be a third
/// description of the same bytes -- and two descriptions of one format is a
/// thing that drifts. So there is one way to make the drift visible without
/// looking at a window: connect, ask, and print what came back. A person whose
/// shell shows nothing can run it and find out whether the engine is not there
/// or the shell cannot read it.
/// </para>
/// <para>
/// A WinExe has no console of its own, so it borrows the one it was started
/// from. Started from Explorer there is none, and it says nothing and opens a
/// window, which is the right thing for a double click.
/// </para>
/// </remarks>
public static partial class Program
{
    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool AttachConsole(uint processId);

    private const uint AttachParentProcess = 0xFFFFFFFFu;

    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--check")
        {
            AttachConsole(AttachParentProcess);
            return Check().GetAwaiter().GetResult();
        }

        // What the generated main does, and the reason this file exists is only
        // the branch above.
        ComWrappersSupport.InitializeComWrappers();
        Application.Start(parameters =>
        {
            var context = new DispatcherQueueSynchronizationContext(
                DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            // Constructed for its side effect: `Application` registers itself,
            // and `OnLaunched` is what opens the window.
            _ = parameters;
            new App();
        });
        return 0;
    }

    private static async Task<int> Check()
    {
        using var engine = new EngineClient();
        if (!await engine.ConnectAsync(1000, CancellationToken.None))
        {
            Console.Error.WriteLine("not connected: mediaperchd is not listening on "
                                    + Protocol.DefaultPipe);
            return 1;
        }
        Console.WriteLine("connected   " + Protocol.DefaultPipe);

        Answer? status = await engine.CallAsync(Kind.Status);
        if (status is null || !status.Value.Is(Kind.StatusReply))
        {
            Console.Error.WriteLine("the engine would not say what it is playing");
            return 1;
        }
        Reader sr = status.Value.Reader();
        Status now = Decode.ReadStatus(sr);
        // **`Complete` and not merely `Ok`.** A reply this build read *most* of
        // is a reply whose fields have moved, and reading most of one is exactly
        // the failure two descriptions of a wire produce.
        if (!sr.Complete)
        {
            Console.Error.WriteLine("the status reply has fields this shell does not know");
            return 1;
        }
        Console.WriteLine($"state       {now.State}");
        Console.WriteLine($"track       {(now.Track.Length == 0 ? "(none)" : now.Track)}");
        Console.WriteLine($"device      {(now.Device.Length == 0 ? "(none)" : now.Device)}");

        Answer? graph = await engine.CallAsync(Kind.Graph);
        if (graph is null || !graph.Value.Is(Kind.GraphReply))
        {
            Console.Error.WriteLine("the engine would not say what shape it is in");
            return 1;
        }
        Reader gr = graph.Value.Reader();
        Graph shape = Decode.ReadGraph(gr);
        if (!gr.Complete)
        {
            Console.Error.WriteLine("the graph reply has fields this shell does not know");
            return 1;
        }
        foreach (Node node in shape.Nodes)
        {
            string module = node.Module.Length == 0 ? string.Empty : $"  [{node.Module}]";
            Console.WriteLine($"node        {node.Id,-12} {node.Kind,-12} {node.Name}{module}");
        }
        foreach (Edge edge in shape.Edges)
        {
            Console.WriteLine($"edge        {edge.From} -> {edge.To}");
        }

        // **The settings verbs, read to the end.** The kinds this shell sends
        // are a hand-written mirror of protocol.hpp, and a mirror can be wrong
        // in a way that only shows when a button is pressed: Save and Quit were
        // once swapped here, and "Save settings" quit the engine. Reading every
        // settings reply through to `Complete` is what makes this command
        // notice a row whose fields have moved.
        var verbs = new (Kind ask, Kind reply, string label, string? node)[]
        {
            (Kind.Settings, Kind.SettingsReply, "settings", null),
            (Kind.EngineSettings, Kind.EngineSettingsReply, "engine", null),
            (Kind.NodeSettings, Kind.NodeSettingsReply, "presenter", "presenter"),
        };
        foreach (var (ask, reply, label, node) in verbs)
        {
            byte[]? payload = null;
            if (node is not null)
            {
                var w = new Writer();
                w.Str(node);
                payload = w.Bytes();
            }
            Answer? rows = await engine.CallAsync(ask, payload);
            if (rows is null)
            {
                Console.Error.WriteLine($"the engine would not answer {label}");
                return 1;
            }
            if (rows.Value.Is(Kind.Error))
            {
                // A refusal is an answer: an engine started without a settings
                // file has no engine settings, and a run with no picture has no
                // presenter.
                Console.WriteLine($"{label,-11} refused: {Session.ErrorText(rows.Value)}");
                continue;
            }
            if (!rows.Value.Is(reply))
            {
                Console.Error.WriteLine($"the {label} reply is not a settings reply");
                return 1;
            }
            Reader rr = rows.Value.Reader();
            List<Setting> list = Decode.ReadSettings(rr);
            if (!rr.Complete)
            {
                Console.Error.WriteLine($"the {label} reply has fields this shell does not know");
                return 1;
            }
            Console.WriteLine($"{label,-11} {list.Count} rows");
            foreach (Setting row in list)
            {
                // The kind is what the dialog draws from, so it is what a
                // person checking the wire wants to see beside each key.
                string kind = row.ReadOnly ? "read only" : row.Kind.ToString().ToLowerInvariant();
                string choices = row.Choices.Count == 0
                    ? ""
                    : $" ({string.Join(", ", row.Choices)})";
                string group = row.Group.Length == 0 ? "" : $" [{row.Group}]";
                Console.WriteLine($"  {row.Key,-18} {kind}{choices}{group}");
            }
        }
        return 0;
    }
}
