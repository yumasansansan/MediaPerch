// SPDX-License-Identifier: GPL-3.0-or-later

using Windows.Storage.Pickers;

namespace MediaPerch.Shell.Controls;

/// <summary>
/// The system's file picker, owned by this window.
/// </summary>
/// <remarks>
/// <b>Told about the window, because an unpackaged app is not.</b> A packaged
/// app's picker knows which window opened it; this one has to be handed the
/// HWND through <c>IInitializeWithWindow</c>, and without that the call
/// fails with an HRESULT and no picker. Every file type is offered: what a
/// file is, is the demuxer's decision, not a suffix's.
/// </remarks>
internal static class FilePicking
{
    public static async Task<IReadOnlyList<string>> PickAsync()
    {
        if (App.Window is null)
        {
            return Array.Empty<string>();
        }
        var picker = new FileOpenPicker
        {
            SuggestedStartLocation = PickerLocationId.MusicLibrary,
            // Windows remembers the last folder per identifier; without one,
            // every open starts from the library again.
            SettingsIdentifier = "mediaperch-open",
        };
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker, WinRT.Interop.WindowNative.GetWindowHandle(App.Window));
        var picked = await picker.PickMultipleFilesAsync();
        var paths = new List<string>();
        foreach (var file in picked)
        {
            if (file.Path.Length != 0)
            {
                paths.Add(file.Path);
            }
        }
        return paths;
    }
}
