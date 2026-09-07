// SPDX-License-Identifier: GPL-3.0-or-later

using MediaPerch.Shell.Ipc;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace MediaPerch.Shell.Controls;

/// <summary>
/// Whatever the engine will take, as something to type into.
/// </summary>
/// <remarks>
/// <para>
/// <b>One editor, three verbs.</b> §10 answers settings in three places -- a
/// node's own (<c>node_settings</c>), the player's (<c>settings</c>) and the
/// engine's (<c>engine_settings</c>) -- and all three answer the same shape:
/// key, value, description, and whether it can be set. So this takes the rows
/// and a delegate that applies one, and the caller decides which verb that is.
/// A second and third editor would be two more places for the same bug.
/// </para>
/// <para>
/// <b>The engine validates, not this.</b> A box takes any text and the answer
/// comes back from the module that owns the setting, in its own words. That is
/// the program's rule about its user -- offer the choice, say what happened --
/// and it is also the only way this could be right: the shell has no idea what
/// a resampler will accept, and a copy of that knowledge here would be a copy
/// that went stale.
/// </para>
/// <para>
/// What the shell does decide is whether to draw a box at all, and it is told:
/// a measurement (<c>peak</c>, <c>cost</c>, a handle) arrives with
/// <see cref="Setting.ReadOnly"/> set and is shown as a value.
/// </para>
/// </remarks>
public sealed partial class SettingsEditor : UserControl
{
    private readonly StackPanel _rows = new() { Spacing = 14 };
    private Func<string, string, Task<string>>? _apply;

    public SettingsEditor()
    {
        Content = _rows;
    }

    /// <summary>Raised once a value has actually been taken by the engine.</summary>
    public event Action? Applied;

    /// <summary>
    /// Draws <paramref name="rows"/>, applying through <paramref name="apply"/>,
    /// which answers what went wrong or an empty string.
    /// </summary>
    public void Show(IReadOnlyList<Setting> rows, Func<string, string, Task<string>> apply)
    {
        _apply = apply;
        _rows.Children.Clear();
        if (rows.Count == 0)
        {
            _rows.Children.Add(Caption("This has no settings of its own."));
            return;
        }
        foreach (Setting row in rows)
        {
            _rows.Children.Add(row.ReadOnly ? Reading(row) : Editing(row));
        }
    }

    /// <summary>Draws a sentence instead of rows, for when there are none to draw.</summary>
    public void Say(string what)
    {
        _apply = null;
        _rows.Children.Clear();
        _rows.Children.Add(Caption(what));
    }

    /// <summary>
    /// A row that can be set: the key, a box, and a button that says so.
    /// </summary>
    private UIElement Editing(Setting row)
    {
        var box = new TextBox
        {
            Text = row.Value,
            FontFamily = new FontFamily("Consolas"),
            MinWidth = 160,
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
        var button = new Button { Content = "Set" };
        var answer = new TextBlock
        {
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            TextWrapping = TextWrapping.Wrap,
            Visibility = Visibility.Collapsed,
        };

        async Task ApplyAsync()
        {
            if (_apply is null)
            {
                return;
            }
            button.IsEnabled = false;
            box.IsEnabled = false;
            string why = await _apply(row.Key, box.Text);
            button.IsEnabled = true;
            box.IsEnabled = true;
            answer.Visibility = Visibility.Visible;
            if (why.Length == 0)
            {
                answer.Text = "Set.";
                answer.Foreground = ThemeBrush("SystemFillColorSuccessBrush",
                                               "TextFillColorSecondaryBrush");
                Applied?.Invoke();
            }
            else
            {
                // **The engine's own sentence, unedited.** It knows why; this
                // does not, and a shell that rewrote the reason would be
                // inventing one.
                answer.Text = why;
                answer.Foreground = ThemeBrush("SystemFillColorCriticalBrush",
                                               "TextFillColorSecondaryBrush");
            }
        }

        button.Click += async (_, _) => await ApplyAsync();
        // Enter, because a box you have to reach for the mouse to commit is a
        // box people leave half-typed.
        box.KeyDown += async (_, e) =>
        {
            if (e.Key == VirtualKey.Enter)
            {
                e.Handled = true;
                await ApplyAsync();
            }
        };

        var line = new Grid { ColumnSpacing = 8 };
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(170) });
        line.ColumnDefinitions.Add(new ColumnDefinition
        {
            Width = new GridLength(1, GridUnitType.Star),
        });
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        TextBlock key = Key(row.Key);
        Grid.SetColumn(key, 0);
        Grid.SetColumn(box, 1);
        Grid.SetColumn(button, 2);
        line.Children.Add(key);
        line.Children.Add(box);
        line.Children.Add(button);

        var stack = new StackPanel { Spacing = 4 };
        stack.Children.Add(line);
        if (row.Description.Length != 0)
        {
            stack.Children.Add(Caption(row.Description));
        }
        stack.Children.Add(answer);
        return stack;
    }

    /// <summary>
    /// A row the engine answers with rather than takes: shown, not offered.
    /// </summary>
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
            Foreground = ThemeBrush("TextFillColorSecondaryBrush", "TextFillColorPrimaryBrush"),
        };
        Grid.SetColumn(key, 0);
        Grid.SetColumn(value, 1);
        line.Children.Add(key);
        line.Children.Add(value);

        var stack = new StackPanel { Spacing = 4 };
        stack.Children.Add(line);
        if (row.Description.Length != 0)
        {
            stack.Children.Add(Caption(row.Description));
        }
        return stack;
    }

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
