// SPDX-License-Identifier: GPL-3.0-or-later

using System.Globalization;
using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Globalization.NumberFormatting;

namespace MediaPerch.Shell.Controls;

/// <summary>
/// Whatever the engine will take, as a dialog: the rows drawn by their kind
/// under their group's heading, OK to apply and Cancel to leave everything as
/// it was.
/// </summary>
/// <remarks>
/// <para>
/// <b>One dialog, three verbs.</b> §10 answers settings in three places -- a
/// node's own (<c>node_settings</c>), the player's (<c>settings</c>) and the
/// engine's (<c>engine_settings</c>) -- and all three answer the same rows:
/// key, value, description, whether it can be set, and since the wire's
/// second version what kind of value it takes, which group it sits in and
/// when it matters. So this takes the rows and a delegate that applies one,
/// and the caller decides which verb that is.
/// </para>
/// <para>
/// <b>The engine validates, not this.</b> A choice is an editable drop-down
/// and a number field has no minimum and no maximum: whatever a person typed
/// is sent, and the answer comes back from the module that owns the setting,
/// in its own words, under the row it was about. What the kind decides is
/// which control to draw -- the one decision a shell cannot make from the
/// text, and the reason the rows carry it.
/// </para>
/// <para>
/// <b>OK applies what changed, in the rows' order, and stops at the first
/// refusal.</b> The dialog stays open with the engine's sentence under that
/// row and everything before it already applied, which is the truth: those
/// keys were taken. Cancel sends nothing.
/// </para>
/// </remarks>
internal sealed partial class SettingsDialog : ContentDialog
{
    /// <summary>One row's control, and how to read it back as the text the engine takes.</summary>
    private sealed class Row
    {
        public required Setting Setting { get; init; }
        public required Func<string> Read { get; init; }
        public required FrameworkElement Line { get; init; }
        public required InfoBar Answer { get; init; }
        /// <summary>Raised when the control's value moved, for the rows that depend on it.</summary>
        public Action? Changed { get; set; }
    }

    private readonly List<Row> _rows = new();
    private readonly Func<string, string, Task<string>> _apply;

    /// <summary>Raised once at least one key was taken by the engine.</summary>
    public event Action? Applied;

    /// <summary>
    /// Builds the dialog. <paramref name="note"/> is a sentence shown above the
    /// rows, and <paramref name="action"/> a button beside it -- what the engine
    /// settings use to say that a change waits for the next start, and to offer
    /// one.
    /// </summary>
    public SettingsDialog(XamlRoot root, string title, IReadOnlyList<Setting> rows,
                          Func<string, string, Task<string>> apply, string? note = null,
                          string? actionLabel = null, Func<Task<string>>? action = null)
    {
        _apply = apply;
        XamlRoot = root;
        Title = title;
        PrimaryButtonText = "OK";
        CloseButtonText = "Cancel";
        DefaultButton = ContentDialogButton.Primary;

        var stack = new StackPanel { Spacing = 12, MinWidth = 520 };
        if (note is not null)
        {
            var bar = new InfoBar
            {
                IsOpen = true,
                IsClosable = false,
                Severity = InfoBarSeverity.Informational,
                Message = note,
            };
            if (actionLabel is not null && action is not null)
            {
                var button = new Button { Content = actionLabel };
                button.Click += async (_, _) =>
                {
                    button.IsEnabled = false;
                    string why = await action();
                    bar.Message = why.Length == 0 ? "The engine was restarted." : why;
                    bar.Severity = why.Length == 0 ? InfoBarSeverity.Success
                                                   : InfoBarSeverity.Warning;
                    button.IsEnabled = true;
                };
                bar.ActionButton = button;
            }
            stack.Children.Add(bar);
        }

        if (rows.Count == 0)
        {
            stack.Children.Add(Caption("This has no settings of its own."));
            IsPrimaryButtonEnabled = false;
        }
        else
        {
            Build(stack, rows);
        }

        Content = new ScrollViewer
        {
            Content = stack,
            MaxHeight = 560,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Padding = new Thickness(0, 0, 12, 0),
        };
        PrimaryButtonClick += OnOk;
    }

    // --- the rows ---------------------------------------------------------------

    /// <summary>
    /// The settable rows under their groups, in the order the groups first
    /// appear; then what the engine only reports, under "Status". A group with
    /// no name has no heading, which is what a module with three keys wants.
    /// </summary>
    private void Build(StackPanel into, IReadOnlyList<Setting> rows)
    {
        var groups = new List<string>();
        foreach (Setting row in rows)
        {
            if (!row.ReadOnly && !groups.Contains(row.Group))
            {
                groups.Add(row.Group);
            }
        }
        bool anySettable = groups.Count != 0;
        foreach (string group in groups)
        {
            if (group.Length != 0)
            {
                into.Children.Add(Heading(group));
            }
            foreach (Setting row in rows)
            {
                if (!row.ReadOnly && row.Group == group)
                {
                    into.Children.Add(Editing(row));
                }
            }
        }
        bool anyStatus = false;
        foreach (Setting row in rows)
        {
            if (row.ReadOnly)
            {
                if (!anyStatus && anySettable)
                {
                    into.Children.Add(Heading("Status"));
                }
                anyStatus = true;
                into.Children.Add(Reading(row));
            }
        }
        Wire();
    }

    /// <summary>
    /// <c>when=key=value[,value]</c>: a row that matters only while another has
    /// one of those values is folded away otherwise, and follows the other
    /// live. A key that is not in this dialog leaves the row shown.
    /// </summary>
    private void Wire()
    {
        foreach (Row dependent in _rows)
        {
            string when = dependent.Setting.When;
            int equals = when.IndexOf('=');
            if (equals <= 0)
            {
                continue;
            }
            string key = when[..equals];
            string[] values = when[(equals + 1)..].Split(',', StringSplitOptions.RemoveEmptyEntries);
            Row? master = _rows.Find(r => r.Setting.Key == key);
            if (master is null)
            {
                continue;
            }
            void Follow()
            {
                string now = master.Read();
                dependent.Line.Visibility =
                    values.Contains(now) ? Visibility.Visible : Visibility.Collapsed;
            }
            master.Changed += Follow;
            Follow();
        }
    }

    private UIElement Editing(Setting row)
    {
        var stack = new StackPanel { Spacing = 4 };
        var line = new Grid { ColumnSpacing = 8 };
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(170) });
        line.ColumnDefinitions.Add(new ColumnDefinition
        {
            Width = new GridLength(1, GridUnitType.Star),
        });
        TextBlock key = Key(row.Key);
        Grid.SetColumn(key, 0);
        line.Children.Add(key);

        var answer = new InfoBar
        {
            IsOpen = false,
            IsClosable = true,
            Severity = InfoBarSeverity.Error,
        };
        var made = new Row
        {
            Setting = row,
            Read = () => row.Value,
            Line = stack,
            Answer = answer,
        };
        Func<string> read;
        FrameworkElement control;
        switch (row.Kind)
        {
            case SettingKind.Choice:
            {
                // **Editable**, so the list is an offer and not a fence: the
                // engine takes whatever is typed and says no in its own words.
                var combo = new ComboBox
                {
                    IsEditable = true,
                    ItemsSource = row.Choices,
                    Text = row.Value,
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    MinWidth = 200,
                };
                int at = row.Choices.IndexOf(row.Value);
                if (at >= 0)
                {
                    combo.SelectedIndex = at;
                }
                combo.SelectionChanged += (_, _) => made.Changed?.Invoke();
                combo.TextSubmitted += (_, _) => made.Changed?.Invoke();
                read = () => combo.Text ?? string.Empty;
                control = combo;
                break;
            }
            case SettingKind.Integer:
            case SettingKind.Number:
            {
                // **No minimum and no maximum.** `min=` and `max=` are advice
                // the help repeats; a thousand lobes is a thousand lobes. The
                // step is the spin button's; a current value that is not a
                // number -- `auto`, `none` -- is shown as the placeholder and
                // sent unchanged unless a number replaces it.
                double step = 1.0;
                string? stepHint = row.Hint("step");
                if (stepHint is not null &&
                    double.TryParse(stepHint, NumberStyles.Float, CultureInfo.InvariantCulture,
                                    out double parsedStep) && parsedStep > 0.0)
                {
                    step = parsedStep;
                }
                bool numeric = double.TryParse(row.Value, NumberStyles.Float,
                                               CultureInfo.InvariantCulture, out double current);
                var box = new NumberBox
                {
                    Value = numeric ? current : double.NaN,
                    PlaceholderText = numeric ? string.Empty : row.Value,
                    SmallChange = step,
                    LargeChange = step * 10.0,
                    SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline,
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    MinWidth = 200,
                    NumberFormatter = new DecimalFormatter
                    {
                        IsGrouped = false,
                        FractionDigits = 0,
                        IntegerDigits = 1,
                    },
                };
                string? unit = row.Hint("unit");
                if (unit is not null)
                {
                    box.Description = unit;
                }
                box.ValueChanged += (_, _) => made.Changed?.Invoke();
                read = () => double.IsNaN(box.Value)
                    ? row.Value
                    : box.Value.ToString("0.############", CultureInfo.InvariantCulture);
                control = box;
                break;
            }
            case SettingKind.Toggle:
            {
                var toggle = new ToggleSwitch
                {
                    IsOn = row.Value == "1" || row.Value == "true",
                    OnContent = "On",
                    OffContent = "Off",
                };
                toggle.Toggled += (_, _) => made.Changed?.Invoke();
                read = () => toggle.IsOn ? "1" : "0";
                control = toggle;
                break;
            }
            case SettingKind.Path:
            {
                var text = new TextBox
                {
                    Text = row.Value,
                    FontFamily = new FontFamily("Consolas"),
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                };
                bool folder = row.Hint("pick") == "folder";
                var browse = new Button { Content = "Browse…" };
                browse.Click += async (_, _) =>
                {
                    string? picked = folder ? await FilePicking.PickFolderAsync()
                                            : await FilePicking.PickOneAsync();
                    if (picked is not null)
                    {
                        text.Text = picked;
                        made.Changed?.Invoke();
                    }
                };
                var pair = new Grid { ColumnSpacing = 8 };
                pair.ColumnDefinitions.Add(new ColumnDefinition
                {
                    Width = new GridLength(1, GridUnitType.Star),
                });
                pair.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                Grid.SetColumn(text, 0);
                Grid.SetColumn(browse, 1);
                pair.Children.Add(text);
                pair.Children.Add(browse);
                text.TextChanged += (_, _) => made.Changed?.Invoke();
                read = () => text.Text;
                control = pair;
                break;
            }
            default:
            {
                var text = new TextBox
                {
                    Text = row.Value,
                    FontFamily = new FontFamily("Consolas"),
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    PlaceholderText = row.Kind == SettingKind.Size ? "WxH, or native" : string.Empty,
                };
                text.TextChanged += (_, _) => made.Changed?.Invoke();
                read = () => text.Text;
                control = text;
                break;
            }
        }
        Grid.SetColumn(control, 1);
        line.Children.Add(control);
        stack.Children.Add(line);
        if (row.Description.Length != 0)
        {
            stack.Children.Add(Caption(row.Description));
        }
        stack.Children.Add(answer);

        _rows.Add(new Row
        {
            Setting = row,
            Read = read,
            Line = stack,
            Answer = answer,
            Changed = null,
        });
        // The row above is the one wired for `when`; `made` only carried the
        // closure the controls raise into, so it is joined to it here.
        Row placed = _rows[^1];
        made.Changed = () => placed.Changed?.Invoke();
        return stack;
    }

    /// <summary>A row the engine answers with rather than takes: shown, not offered.</summary>
    private static UIElement Reading(Setting row)
    {
        var line = new Grid { ColumnSpacing = 8 };
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(170) });
        line.ColumnDefinitions.Add(new ColumnDefinition
        {
            Width = new GridLength(1, GridUnitType.Star),
        });
        TextBlock key = Key(row.Key);
        var value = new TextBlock
        {
            Text = row.Value,
            FontFamily = new FontFamily("Consolas"),
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,
            Foreground = ThemeBrush("TextFillColorSecondaryBrush", "TextFillColorPrimaryBrush"),
        };
        Grid.SetColumn(key, 0);
        Grid.SetColumn(value, 1);
        line.Children.Add(key);
        line.Children.Add(value);

        var stack = new StackPanel { Spacing = 2 };
        stack.Children.Add(line);
        if (row.Description.Length != 0)
        {
            stack.Children.Add(Caption(row.Description));
        }
        return stack;
    }

    // --- OK -----------------------------------------------------------------------

    /// <summary>
    /// What changed, in the rows' order, one key at a time; the first refusal
    /// keeps the dialog open with the sentence under its row.
    /// </summary>
    private async void OnOk(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        ContentDialogButtonClickDeferral deferral = args.GetDeferral();
        IsPrimaryButtonEnabled = false;
        try
        {
            bool taken = false;
            foreach (Row row in _rows)
            {
                row.Answer.IsOpen = false;
                if (row.Line.Visibility == Visibility.Collapsed)
                {
                    continue; // folded away: a value the engine will not read yet
                }
                string now = row.Read();
                if (Same(row.Setting, now))
                {
                    continue;
                }
                string why = await _apply(row.Setting.Key, now);
                if (why.Length != 0)
                {
                    // **The engine's own sentence, unedited**, where the value
                    // was typed. It knows why; this does not.
                    row.Answer.Message = why;
                    row.Answer.IsOpen = true;
                    args.Cancel = true;
                    break;
                }
                taken = true;
            }
            if (taken)
            {
                Applied?.Invoke();
            }
        }
        finally
        {
            IsPrimaryButtonEnabled = true;
            deferral.Complete();
        }
    }

    /// <summary>
    /// Whether the control still holds what the engine said: numbers are
    /// compared as numbers, so that `3` and `3.0` are one value and a step
    /// that went up and back down sends nothing.
    /// </summary>
    private static bool Same(Setting setting, string now)
    {
        if (now == setting.Value)
        {
            return true;
        }
        if (setting.Kind is SettingKind.Integer or SettingKind.Number &&
            double.TryParse(setting.Value, NumberStyles.Float, CultureInfo.InvariantCulture,
                            out double was) &&
            double.TryParse(now, NumberStyles.Float, CultureInfo.InvariantCulture, out double is_))
        {
            return Math.Abs(was - is_) <= 1e-9 * Math.Max(1.0, Math.Abs(was));
        }
        return false;
    }

    // --- the small pieces -----------------------------------------------------------

    private static TextBlock Heading(string text) => new()
    {
        Text = text,
        Margin = new Thickness(0, 8, 0, 0),
        Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
    };

    private static TextBlock Key(string key) => new()
    {
        Text = key,
        FontFamily = new FontFamily("Consolas"),
        TextWrapping = TextWrapping.Wrap,
        VerticalAlignment = VerticalAlignment.Center,
    };

    private static TextBlock Caption(string text) => new()
    {
        Text = text,
        TextWrapping = TextWrapping.Wrap,
        Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
        Foreground = ThemeBrush("TextFillColorSecondaryBrush", "TextFillColorPrimaryBrush"),
    };

    /// <summary>
    /// A theme brush by name, or the fallback. Asked for rather than indexed
    /// because a key this Windows App SDK does not have would otherwise be an
    /// exception on a machine rather than a compile error here.
    /// </summary>
    private static Brush ThemeBrush(string name, string fallback)
    {
        if (Application.Current.Resources.TryGetValue(name, out object? found) &&
            found is Brush brush)
        {
            return brush;
        }
        return (Brush)Application.Current.Resources[fallback];
    }
}
