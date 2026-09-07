// SPDX-License-Identifier: GPL-3.0-or-later

using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using MediaPerch.Shell.Ipc;

namespace MediaPerch.Shell.Canvas;

/// <summary>
/// The engine's shape, drawn.
/// </summary>
/// <remarks>
/// <para>
/// <b>Fusion's look over a chain's semantics.</b> §10 says why: the engine's
/// graph is not a free-form DAG. §5 is two graphs, and what is actually
/// variable is the membership and order of one linear chain, the path policy,
/// and each stage's settings. A canvas that let a person draw an edge from
/// anywhere to anywhere would be offering something the engine will refuse, so
/// this is a row of nodes that can be dragged to reorder and sockets that
/// accept the one connection that exists.
/// </para>
/// <para>
/// <b>It draws what the engine said, and nothing it worked out for itself.</b>
/// The layout is this control's; the nodes, the edges and which of them may be
/// moved all come from <c>graph</c>. Keeping a second model here is the thing
/// §10 says not to do, and it would be this file that drifted.
/// </para>
/// <para>
/// Written rather than taken from a toolkit, and not by preference: the
/// Community Toolkit does not support the Windows App SDK this targets.
/// </para>
/// </remarks>
public sealed partial class NodeCanvas : UserControl
{
    private readonly Microsoft.UI.Xaml.Controls.Canvas _surface = new();
    private Graph _graph = new();

    /// <summary>Node geometry, in the order the engine gave it.</summary>
    private readonly Dictionary<string, Rect> _placed = new();

    private const double NodeWidth = 200;
    private const double NodeHeight = 76;
    private const double GapX = 56;
    private const double GapY = 40;
    private const double Edge = 24;

    public NodeCanvas()
    {
        Content = _surface;
        _surface.Background = new SolidColorBrush(Colors.Transparent);
    }

    /// <summary>Which node's settings button was pressed.</summary>
    public event Action<Node>? SettingsWanted;

    /// <summary>
    /// A removable node was dragged to a new place in its chain: the node, and
    /// the index it should now have among that chain's stages.
    /// </summary>
    /// <remarks>
    /// <b>Only the order is decided here.</b> What the drag means is a new
    /// <c>dsp</c> or <c>video_dsp</c> value, and writing that is the page's:
    /// the canvas draws what the engine said and asks for a change, it does
    /// not keep a second copy of the chain to edit.
    /// </remarks>
    public event Action<Node, int>? ReorderWanted;

    /// <summary>The bin on a removable node was pressed.</summary>
    public event Action<Node>? RemoveWanted;

    /// <summary>A stage's own index, from its id: <c>dsp.2</c> is 2.</summary>
    public static int IndexOf(Node node)
    {
        int dot = node.Id.LastIndexOf('.');
        return dot >= 0 && int.TryParse(node.Id[(dot + 1)..], out int n) ? n : -1;
    }

    /// <summary>
    /// Where a stage whose centre is at <paramref name="centreX"/> belongs
    /// among the other stages of its kind: how many of them it is now to the
    /// right of.
    /// </summary>
    private int SlotFor(Node moved, double centreX)
    {
        int slot = 0;
        foreach (Node other in _graph.Nodes)
        {
            if (other.Id == moved.Id || other.Kind != moved.Kind ||
                (other.Flags & NodeFlags.Removable) == 0 ||
                !_placed.TryGetValue(other.Id, out Rect at))
            {
                continue;
            }
            if (at.X + at.Width / 2 < centreX)
            {
                ++slot;
            }
        }
        return slot;
    }

    public void Show(Graph graph)
    {
        _graph = graph;
        Redraw();
    }

    private void Redraw()
    {
        _surface.Children.Clear();
        _placed.Clear();
        if (_graph.Nodes.Count == 0)
        {
            return;
        }

        // **Two rows, because there are two chains and they never meet.** §4
        // gives the file one position and §8 gives the run one clock, but the
        // samples and the frames do not flow into one another -- what joins
        // them is the clock, and drawing an edge for that would be drawing data
        // where there is none.
        double audioX = Edge;
        double videoX = Edge;
        foreach (Node node in _graph.Nodes)
        {
            bool video = node.Kind is NodeKind.VideoSource or NodeKind.VideoStage
                                   or NodeKind.Presenter;
            double x = video ? videoX : audioX;
            double y = Edge + (video ? NodeHeight + GapY : 0);
            _placed[node.Id] = new Rect(x, y, NodeWidth, NodeHeight);
            if (video)
            {
                videoX += NodeWidth + GapX;
            }
            else
            {
                audioX += NodeWidth + GapX;
            }
        }

        // Edges first, so a node is drawn over the line that reaches it.
        foreach (Edge edge in _graph.Edges)
        {
            if (!_placed.TryGetValue(edge.From, out Rect from) ||
                !_placed.TryGetValue(edge.To, out Rect to))
            {
                continue;
            }
            _surface.Children.Add(Wire(from, to));
        }
        foreach (Node node in _graph.Nodes)
        {
            _surface.Children.Add(Box(node, _placed[node.Id]));
        }

        _surface.Width = Math.Max(audioX, videoX) + Edge;
        _surface.Height = Edge * 2 + NodeHeight * 2 + GapY;
    }

    /// <summary>
    /// One connection, as the curve a node graph draws: out of the right of one
    /// and into the left of the next, flattening at both ends so the line reads
    /// as leaving and arriving rather than merely passing through.
    /// </summary>
    private static Microsoft.UI.Xaml.Shapes.Path Wire(Rect from, Rect to)
    {
        var start = new Point(from.Right, from.Top + from.Height / 2);
        var end = new Point(to.Left, to.Top + to.Height / 2);
        double bend = Math.Max(24, (end.X - start.X) / 2);

        var figure = new PathFigure { StartPoint = start, IsClosed = false };
        figure.Segments.Add(new BezierSegment
        {
            Point1 = new Point(start.X + bend, start.Y),
            Point2 = new Point(end.X - bend, end.Y),
            Point3 = end,
        });
        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);

        return new Microsoft.UI.Xaml.Shapes.Path
        {
            Data = geometry,
            StrokeThickness = 2,
            Stroke = (Brush)Application.Current.Resources["TextFillColorTertiaryBrush"],
        };
    }

    private UIElement Box(Node node, Rect where)
    {
        bool removable = (node.Flags & NodeFlags.Removable) != 0;
        var border = new Border
        {
            Width = where.Width,
            Height = where.Height,
            CornerRadius = new CornerRadius(8),
            BorderThickness = new Thickness(1),
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
        };

        var title = new TextBlock
        {
            Text = node.Name.Length == 0 ? node.Id : node.Name,
            TextTrimming = TextTrimming.CharacterEllipsis,
            MaxLines = 2,
            TextWrapping = TextWrapping.Wrap,
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
        };
        var subtitle = new TextBlock
        {
            // The module, or what the node is when it is not one -- the
            // converter is arithmetic in the core and has no module id.
            Text = node.Module.Length == 0 ? Describe(node.Kind) : node.Module,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
        };

        // Hit-testable, so a drag can start on the words as well as the gaps.
        var text = new StackPanel { Spacing = 2, Background = new SolidColorBrush(Colors.Transparent) };
        text.Children.Add(title);
        text.Children.Add(subtitle);

        var row = new Grid { ColumnSpacing = 8, Padding = new Thickness(12, 10, 8, 10) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(text, 0);
        row.Children.Add(text);

        if (removable)
        {
            // **Dragged to reorder, and the drag is the words.** The buttons
            // beside them keep their own pointer, so pressing one does not
            // start a drag. The box follows the pointer sideways; on release
            // its centre says which slot it is in now, and the page is asked.
            double startX = 0;
            bool dragging = false;
            text.PointerPressed += (_, e) =>
            {
                startX = e.GetCurrentPoint(_surface).Position.X;
                dragging = text.CapturePointer(e.Pointer);
                if (dragging)
                {
                    Microsoft.UI.Xaml.Controls.Canvas.SetZIndex(border, 1);
                    e.Handled = true;
                }
            };
            text.PointerMoved += (_, e) =>
            {
                if (dragging)
                {
                    border.RenderTransform = new TranslateTransform
                    {
                        X = e.GetCurrentPoint(_surface).Position.X - startX,
                    };
                }
            };
            text.PointerReleased += (_, e) =>
            {
                if (!dragging)
                {
                    return;
                }
                dragging = false;
                text.ReleasePointerCapture(e.Pointer);
                double dx = e.GetCurrentPoint(_surface).Position.X - startX;
                border.RenderTransform = null;
                Microsoft.UI.Xaml.Controls.Canvas.SetZIndex(border, 0);
                int to = SlotFor(node, where.X + where.Width / 2 + dx);
                if (to != IndexOf(node))
                {
                    ReorderWanted?.Invoke(node, to);
                }
            };
            text.PointerCaptureLost += (_, _) =>
            {
                dragging = false;
                border.RenderTransform = null;
                Microsoft.UI.Xaml.Controls.Canvas.SetZIndex(border, 0);
            };
            ToolTipService.SetToolTip(text, "Drag to reorder");

            var bin = new Button
            {
                Content = new FontIcon { Glyph = "\uE74D", FontSize = 14 },
                VerticalAlignment = VerticalAlignment.Top,
                Padding = new Thickness(6),
            };
            ToolTipService.SetToolTip(bin, "Remove from the chain");
            bin.Click += (_, _) => RemoveWanted?.Invoke(node);
            Grid.SetColumn(bin, 2);
            row.Children.Add(bin);
        }

        // **The settings button, and only where there is something to set.**
        // `graph` says which nodes have settings; a button on one that does not
        // would open an empty panel and teach a person not to press it.
        if ((node.Flags & NodeFlags.Settable) != 0)
        {
            var button = new Button
            {
                Content = new FontIcon { Glyph = "", FontSize = 14 },
                VerticalAlignment = VerticalAlignment.Top,
                Padding = new Thickness(6),
            };
            ToolTipService.SetToolTip(button, "Settings");
            button.Click += (_, _) => SettingsWanted?.Invoke(node);
            Grid.SetColumn(button, 1);
            row.Children.Add(button);
        }

        border.Child = row;
        Microsoft.UI.Xaml.Controls.Canvas.SetLeft(border, where.X);
        Microsoft.UI.Xaml.Controls.Canvas.SetTop(border, where.Y);
        return border;
    }

    private static string Describe(NodeKind kind) => kind switch
    {
        NodeKind.Source => "the file",
        NodeKind.Convert => "Path B",
        NodeKind.Dsp => "a stage",
        NodeKind.Sink => "the device",
        NodeKind.VideoSource => "the video decoder",
        NodeKind.VideoStage => "a picture stage",
        NodeKind.Presenter => "the presenter",
        // A kind this build does not know is still drawn and still says so,
        // which is why the wire carries a number rather than a word.
        _ => "something this shell does not know",
    };
}
