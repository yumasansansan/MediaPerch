// SPDX-License-Identifier: GPL-3.0-or-later

using Microsoft.Windows.Storage.Pickers;

namespace MediaPerch.Shell.Controls;

/// <summary>
/// The system's file picker, owned by this window.
/// </summary>
/// <remarks>
/// <para>
/// <b>The Windows App SDK's own picker, not <c>Windows.Storage</c>'s.</b> The
/// older one needs to be handed an HWND through <c>IInitializeWithWindow</c>
/// and, in this app, showed nothing and never returned -- no dialog, no
/// exception, and a shell that seemed to ignore the button. The App SDK's
/// picker takes the window's id in its constructor and is the one made for
/// an unpackaged desktop app.
/// </para>
/// <para>
/// Every file type is offered: what a file is, is the demuxer's decision, not
/// a suffix's. Windows remembers the last folder per identifier.
/// </para>
/// </remarks>
internal static class FilePicking
{
    public static async Task<IReadOnlyList<string>> PickAsync()
    {
        var paths = new List<string>();
        if (App.Window is null)
        {
            return paths;
        }
        Ipc.Session.Log("picker: opening");
        try
        {
            var picker = new FileOpenPicker(App.Window.AppWindow.Id)
            {
                SuggestedStartLocation = PickerLocationId.MusicLibrary,
                SettingsIdentifier = "mediaperch-open",
            };
            picker.FileTypeFilter.Add("*");
            var picked = await picker.PickMultipleFilesAsync();
            foreach (var one in picked)
            {
                if (!string.IsNullOrEmpty(one.Path))
                {
                    paths.Add(one.Path);
                }
            }
        }
        catch (Exception e)
        {
            // **Named, because this is the call that fails on a machine rather
            // than in a build.**
            Ipc.Session.Log("picker: failed: " + e);
            return paths;
        }
        Ipc.Session.Log($"picker: {paths.Count} file(s) chosen");
        return paths;
    }
}
