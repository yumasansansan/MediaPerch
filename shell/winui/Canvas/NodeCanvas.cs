// SPDX-License-Identifier: GPL-3.0-or-later

using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.ApplicationModel.DataTransfer;
using Windows.Foundation;
using Windows.System;
using MediaPerch.Shell.Ipc;

namespace MediaPerch.Shell.Canvas;

/// <summary>
/// The engine's shape, drawn: nodes with sockets, wires between them, and the
/// gestures a node editor is worked by.
/// </summary>
/// <remarks>
/// <para>
/// <b>Fusion's look over a chain's semantics.</b> §10 says why: the engine's
/// graph is not a free-form DAG. §5 is two graphs, and what is actually
/// variable is the membership and order of one linear chain, the path policy,
/// and each stage's settings. So every gesture here ends as one of three asks
/// -- put this module here, move this stage there, take this stage out -- and
/// the page turns the ask into a new <c>dsp</c> or <c>video_dsp</c> value. A
/// wire dragged from anywhere to anywhere would offer connections the engine
/// refuses; a wire dragged along a chain moves a stage, which is the one
/// connection that exists.
/// </para>
/// <para>
/// <b>It draws what the engine said, and nothing it worked out for itself.</b>
/// The layout is this control's; the nodes, the edges and which of them may be
/// moved all come from <c>graph</c>. What is kept between draws is the
/// selection and a drag in progress, and nothing else.
/// </para>
/// <para>
/// The gestures: a module from the palette dropped on a chain is appended, and
/// dropped on a wire is put there; a wire dragged from a node's output socket
/// onto another's input socket moves the removable one of the two so that the
/// first flows into the second; a wire picked up at a node's input and let go
/// on nothing removes that node; a click selects, Delete removes, and the
/// right button opens a menu with both. The words of a stage can still be
/// dragged sideways to reorder it, because that was the first gesture and
/// still the quickest.
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
    /// <summary>Each node's box, for the selection outline.</summary>
    private readonly Dictionary<string, Border> _boxes = new();
    private string? _selected;

    /// <summary>A wire being dragged: where it started, and the line following the pointer.</summary>
    private Node? _wireFrom;
    private bool _wireFromInput;
    private Microsoft.UI.Xaml.Shapes.Path? _wireLine;

    private const double NodeWidth = 200;
    private const double NodeHeight = 76;
    private const double GapX = 56;
    private const double GapY = 40;
    private const double Edge = 24;
    private const double Socket = 14;
    /// <summary>How near a drop has to land to a wire to be put on it.</summary>
    private const double NearWire = 18;

    public NodeCanvas()
    {
        Content = _surface;
        _surface.Background = new SolidColorBrush(Colors.Transparent);
        // Keyboard, for Delete on the selection; a click on a node takes focus.
        IsTabStop = true;
        KeyDown += OnKey;
        // The palette's modules are dropped here.
        AllowDrop = true;
        DragOver += OnDragOver;
        Drop += OnDrop;
    }

    /// <summary>Which node's settings were asked for: its gear, or the menu.</summary>
    public event Action<Node>? SettingsWanted;

    /// <summary>
    /// A removable node should now have this index among its chain's stages.
    /// </summary>
    /// <remarks>
    /// <b>Only the order is decided here.</b> What the gesture means is a new
    /// <c>dsp</c> or <c>video_dsp</c> value, and writing that is the page's:
    /// the canvas draws what the engine said and asks for a change, it does
    /// not keep a second copy of the chain to edit.
    /// </remarks>
    public event Action<Node, int>? ReorderWanted;

    /// <summary>A removable node should go.</summary>
    public event Action<Node>? RemoveWanted;

    /// <summary>
    /// A wire was drawn from <c>from</c>'s output to <c>to</c>'s input: the
    /// first should flow into the second. Which of them moves is the page's
    /// call, because it knows the chains.
    /// </summary>
    public event Action<Node, Node>? WireWanted;

    /// <summary>
    /// A module from the palette was dropped: its id, and the wire it landed
    /// on, or null for open canvas -- the end of its chain.
    /// </summary>
    public event Action<string, Edge?>? ModuleDropped;

    /// <summary>A stage's own index, from its id: <c>dsp.2</c> is 2.</summary>
    public static int IndexOf(Node node)
    {
        int dot = node.Id.LastIndexOf('.');
        return dot >= 0 && int.TryParse(node.Id[(dot + 1)..], out int n) ? n : -1;
    }

    /// <summary>The node with this id, or null.</summary>
    public Node? NodeById(string id) => _graph.Nodes.FirstOrDefault(n => n.Id == id);

    /// <summary>Whether a node is on the picture's row rather than the sound's.</summary>
    public static bool IsVideo(Node node) =>
        node.Kind is NodeKind.VideoSource or NodeKind.VideoStage or NodeKind.Presenter;

    private static bool HasInput(Node node) =>
        node.Kind is not (NodeKind.Source or NodeKind.VideoSource);

    private static bool HasOutput(Node node) =>
        node.Kind is not (NodeKind.Sink or NodeKind.Presenter);

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
        if (_selected is not null && !graph.Nodes.Any(n => n.Id == _selected))
        {
            _selected = null;
        }
        Redraw();
    }

    private void Redraw()
    {
        _surface.Children.Clear();
        _placed.Clear();
        _boxes.Clear();
        _wireLine = null;
        _wireFrom = null;
        if (_graph.Nodes.Count == 0)
        {
            _surface.Width = 0;
            _surface.Height = 0;
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
            bool video = IsVideo(node);
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
            _surface.Children.Add(Wire(OutputOf(from), InputOf(to), false));
        }
        foreach (Node node in _graph.Nodes)
        {
            _surface.Children.Add(Box(node, _placed[node.Id]));
        }
        // Sockets last, over the boxes' edges.
        foreach (Node node in _graph.Nodes)
        {
            Rect where = _placed[node.Id];
            if (HasInput(node))
            {
                _surface.Children.Add(SocketAt(node, InputOf(where), true));
            }
            if (HasOutput(node))
            {
                _surface.Children.Add(SocketAt(node, OutputOf(where), false));
            }
        }

        _surface.Width = Math.Max(audioX, videoX) + Edge;
        _surface.Height = Edge * 2 + NodeHeight * 2 + GapY;
    }

    private static Point InputOf(Rect box) => new(box.Left, box.Top + box.Height / 2);

    private static Point OutputOf(Rect box) => new(box.Right, box.Top + box.Height / 2);

    /// <summary>
    /// One connection, as the curve a node graph draws: out of the right of one
    /// and into the left of the next, flattening at both ends so the line reads
    /// as leaving and arriving rather than merely passing through.
    /// </summary>
    private static Microsoft.UI.Xaml.Shapes.Path Wire(Point start, Point end, bool live)
    {
        double bend = Math.Max(24, Math.Abs(end.X - start.X) / 2);
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
            Stroke = live ? AccentBrush()
                          : (Brush)Application.Current.Resources["TextFillColorTertiaryBrush"],
            IsHitTestVisible = false,
        };
    }

    /// <summary>
    /// A socket: the circle a wire is dragged out of, or into. An input socket
    /// of a removable node also lets its wire be picked up and dropped on
    /// nothing, which takes the node out.
    /// </summary>
    private UIElement SocketAt(Node node, Point centre, bool input)
    {
        var dot = new Ellipse
        {
            Width = Socket,
            Height = Socket,
            Fill = AccentBrush(),
            Stroke = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
            StrokeThickness = 2,
        };
        Microsoft.UI.Xaml.Controls.Canvas.SetLeft(dot, centre.X - Socket / 2);
        Microsoft.UI.Xaml.Controls.Canvas.SetTop(dot, centre.Y - Socket / 2);
        ToolTipService.SetToolTip(dot, input
            ? ((node.Flags & NodeFlags.Removable) != 0
                   ? "Drag a wire here from another node's output; drag this wire away to remove the node"
                   : "Drag a wire here from another node's output")
            : "Drag a wire from here to another node's input");

        bool dragging = false;
        dot.PointerPressed += (_, e) =>
        {
            // An input socket on a node that cannot be removed has no wire
            // to pick up: a wire drawn *to* it comes from somewhere else.
            if (input && (node.Flags & NodeFlags.Removable) == 0)
            {
                return;
            }
            dragging = dot.CapturePointer(e.Pointer);
            if (!dragging)
            {
                return;
            }
            e.Handled = true;
            _wireFrom = node;
            _wireFromInput = input;
            _wireLine = Wire(centre, e.GetCurrentPoint(_surface).Position, true);
            _surface.Children.Add(_wireLine);
        };
        dot.PointerMoved += (_, e) =>
        {
            if (!dragging || _wireLine is null)
            {
                return;
            }
            Point at = e.GetCurrentPoint(_surface).Position;
            _surface.Children.Remove(_wireLine);
            _wireLine = input ? Wire(at, centre, true) : Wire(centre, at, true);
            _surface.Children.Add(_wireLine);
        };
        dot.PointerReleased += (_, e) =>
        {
            if (!dragging)
            {
                return;
            }
            dragging = false;
            dot.ReleasePointerCapture(e.Pointer);
            Point at = e.GetCurrentPoint(_surface).Position;
            if (_wireLine is not null)
            {
                _surface.Children.Remove(_wireLine);
                _wireLine = null;
            }
            Node? from = _wireFrom;
            _wireFrom = null;
            if (from is null)
            {
                return;
            }
            if (_wireFromInput)
            {
                // Picked up at this node's input. Let go on another node's
                // output, that node flows into this one; let go on nothing,
                // this node is out.
                Node? source = NodeWithSocketAt(at, false);
                if (source is not null && source.Id != from.Id)
                {
                    WireWanted?.Invoke(source, from);
                }
                else if (source is null)
                {
                    RemoveWanted?.Invoke(from);
                }
                return;
            }
            Node? target = NodeWithSocketAt(at, true);
            if (target is not null && target.Id != from.Id)
            {
                WireWanted?.Invoke(from, target);
            }
        };
        dot.PointerCaptureLost += (_, _) =>
        {
            dragging = false;
            if (_wireLine is not null)
            {
                _surface.Children.Remove(_wireLine);
                _wireLine = null;
            }
            _wireFrom = null;
        };
        return dot;
    }

    /// <summary>The node whose input (or output) socket is under <paramref name="at"/>, if any.</summary>
    private Node? NodeWithSocketAt(Point at, bool input)
    {
        foreach (Node node in _graph.Nodes)
        {
            if ((input && !HasInput(node)) || (!input && !HasOutput(node)) ||
                !_placed.TryGetValue(node.Id, out Rect box))
            {
                continue;
            }
            Point centre = input ? InputOf(box) : OutputOf(box);
            double dx = at.X - centre.X;
            double dy = at.Y - centre.Y;
            // Generous: a socket is fourteen pixels and a wire is let go in a
            // hurry.
            if (dx * dx + dy * dy <= 20 * 20)
            {
                return node;
            }
        }
        return null;
    }

    /// <summary>The wire nearest <paramref name="at"/>, when one is within reach.</summary>
    private Edge? WireNear(Point at)
    {
        Edge? best = null;
        double bestDistance = NearWire;
        foreach (Edge edge in _graph.Edges)
        {
            if (!_placed.TryGetValue(edge.From, out Rect from) ||
                !_placed.TryGetValue(edge.To, out Rect to))
            {
                continue;
            }
            Point start = OutputOf(from);
            Point end = InputOf(to);
            // The wire is a curve, but between two nodes on one row it is a
            // near-horizontal one; the distance to the straight line between
            // its ends is what a person aims at.
            double dx = end.X - start.X;
            double t = Math.Abs(dx) < 1e-6 ? 0.0 : Math.Clamp((at.X - start.X) / dx, 0.0, 1.0);
            double lineX = start.X + t * dx;
            double lineY = start.Y + t * (end.Y - start.Y);
            double distance = Math.Sqrt((at.X - lineX) * (at.X - lineX) + (at.Y - lineY) * (at.Y - lineY));
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = edge;
            }
        }
        return best;
    }

    // --- the palette's drop ---------------------------------------------------------

    private void OnDragOver(object sender, DragEventArgs e)
    {
        bool module = e.DataView.Contains(StandardDataFormats.Text);
        e.AcceptedOperation = module ? DataPackageOperation.Copy : DataPackageOperation.None;
        if (module && e.DragUIOverride is not null)
        {
            Edge? wire = WireNear(e.GetPosition(_surface));
            e.DragUIOverride.Caption = wire is null ? "Add to the chain" : "Put it here";
            e.DragUIOverride.IsCaptionVisible = true;
        }
    }

    private async void OnDrop(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.Text))
        {
            return;
        }
        Point at = e.GetPosition(_surface);
        string id = (await e.DataView.GetTextAsync()).Trim();
        if (id.Length == 0)
        {
            return;
        }
        ModuleDropped?.Invoke(id, WireNear(at));
    }

    // --- the selection ----------------------------------------------------------

    private void Select(Node? node)
    {
        _selected = node?.Id;
        foreach ((string id, Border box) in _boxes)
        {
            bool chosen = id == _selected;
            box.BorderBrush = chosen
                ? AccentBrush()
                : (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"];
            box.BorderThickness = new Thickness(chosen ? 2 : 1);
        }
    }

    private void OnKey(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Delete || _selected is null)
        {
            return;
        }
        Node? node = _graph.Nodes.FirstOrDefault(n => n.Id == _selected);
        if (node is not null && (node.Flags & NodeFlags.Removable) != 0)
        {
            e.Handled = true;
            RemoveWanted?.Invoke(node);
        }
    }

    // --- the boxes -------------------------------------------------------------

    private UIElement Box(Node node, Rect where)
    {
        bool removable = (node.Flags & NodeFlags.Removable) != 0;
        bool chosen = node.Id == _selected;
        var border = new Border
        {
            Width = where.Width,
            Height = where.Height,
            CornerRadius = new CornerRadius(8),
            BorderThickness = new Thickness(chosen ? 2 : 1),
            BorderBrush = chosen ? AccentBrush()
                                 : (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
        };
        _boxes[node.Id] = border;

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
        Grid.SetColumn(text, 0);
        row.Children.Add(text);

        // The menu: what the gear and Delete do, for the right button.
        var menu = new MenuFlyout();
        if ((node.Flags & NodeFlags.Settable) != 0)
        {
            var settings = new MenuFlyoutItem
            {
                Text = "Settings…",
                Icon = new FontIcon { Glyph = "" },
            };
            settings.Click += (_, _) => SettingsWanted?.Invoke(node);
            menu.Items.Add(settings);
        }
        if (removable)
        {
            var remove = new MenuFlyoutItem
            {
                Text = "Remove",
                Icon = new FontIcon { Glyph = "" },
            };
            remove.Click += (_, _) => RemoveWanted?.Invoke(node);
            menu.Items.Add(remove);
        }
        if (menu.Items.Count != 0)
        {
            border.ContextFlyout = menu;
        }

        // A click selects, and takes the keyboard for Delete.
        text.PointerPressed += (_, _) =>
        {
            Select(node);
            Focus(FocusState.Programmatic);
        };

        if (removable)
        {
            // **Dragged to reorder, and the drag is the words.** The button
            // beside them keeps its own pointer, so pressing it does not
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
            ToolTipService.SetToolTip(text, "Drag sideways to reorder; Delete removes");
        }

        // **The settings button, and only where there is something to set.**
        // `graph` says which nodes have settings; a button on one that does not
        // would open an empty dialog and teach a person not to press it.
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

    private static Brush AccentBrush()
    {
        if (Application.Current.Resources.TryGetValue("AccentFillColorDefaultBrush",
                                                      out object? found) &&
            found is Brush brush)
        {
            return brush;
        }
        return (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"];
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
