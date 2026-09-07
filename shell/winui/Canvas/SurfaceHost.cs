// SPDX-License-Identifier: GPL-3.0-or-later

using System.Numerics;
using System.Runtime.InteropServices;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Hosting;
using WinRT;

namespace MediaPerch.Shell.Canvas;

/// <summary>
/// The engine's picture, composited into this window.
/// </summary>
/// <remarks>
/// <para>
/// <b>This is the far end of §9.7.1.</b> The engine draws into a composition
/// surface it made and never shows; the handle crosses as a number and is
/// duplicated into this process; and here it becomes something the window
/// composites. From then on no frame crosses the boundary at all -- what
/// crossed was a handle, once.
/// </para>
/// <para>
/// <b>Through WinUI's own compositor rather than DirectComposition.</b> Both
/// were on the table. Raw DComp would need a target on an <c>HWND</c>, and
/// WinUI 3 already owns this window's -- so it would mean a child window and a
/// second composition tree to keep in step with the first. WinUI's compositor
/// takes the same handle through
/// <c>ICompositorSwapChainInterop::CreateCompositionSurfaceForHandle</c> and
/// the result is a visual like any other, which is one tree.
/// </para>
/// <para>
/// The interop is by function pointer rather than by a generated wrapper, for
/// Native AOT: two calls on one vtable is less machinery than a source
/// generator, and the vtable is the thing the header actually declares.
/// </para>
/// </remarks>
/// <remarks>
/// <b>An instance, because what it makes has to be closed.</b> A track boundary
/// is a new picture and so a new surface, and on a playlist of short files that
/// is once a second; a brush and a visual left for the collector each time is a
/// handle count that climbs for as long as the window is open. Measured: 943 to
/// 961 over twenty attaches. They are closed here instead.
/// </remarks>
internal sealed unsafe class SurfaceHost
{
    private SpriteVisual? _visual;
    private CompositionSurfaceBrush? _brush;
    private ICompositionSurface? _surface;
    private Vector2 _placed;
    private Vector3 _offset;

    /// <summary>
    /// Puts the visual at (<paramref name="x"/>, <paramref name="y"/>) in the
    /// host, <paramref name="width"/> by <paramref name="height"/>, in the
    /// host's own units.
    /// </summary>
    /// <remarks>
    /// This is the box the engine was asked to render at, converted back to
    /// device-independent pixels, and centred by the caller: the surface then
    /// maps onto the visual one to one and the compositor scales nothing,
    /// which is the whole point of §9.7.1's size message. Remembered, so an
    /// attach that comes after a resize lands in the right place too.
    /// </remarks>
    public void Place(double x, double y, double width, double height)
    {
        _placed = new Vector2((float)width, (float)height);
        _offset = new Vector3((float)x, (float)y, 0f);
        if (_visual is not null)
        {
            _visual.Size = _placed;
            _visual.Offset = _offset;
        }
    }

    /// <summary>
    /// <c>Microsoft.UI.Composition.Interop.h</c>'s <c>ICompositorSwapChainInterop</c>,
    /// which derives from <c>ICompositorInterop</c>. Read from the header the
    /// Windows App SDK ships rather than remembered: the Windows.UI.Composition
    /// interface of nearly the same name has a different id and a different
    /// vtable, and getting that wrong is a call through the wrong slot.
    /// </summary>
    private static readonly Guid CompositorSwapChainInterop =
        new("FC084699-67D8-40E1-ADE7-08901D84FFDA");

    /// <summary>
    /// Slot 4: three for <c>IUnknown</c>, one for <c>ICompositorInterop</c>'s
    /// <c>CreateGraphicsDevice</c>, then this.
    /// </summary>
    private const int CreateSurfaceForHandleSlot = 4;

    /// <summary>
    /// Puts <paramref name="handle"/> on <paramref name="host"/>, and answers
    /// what went wrong when it could not.
    /// </summary>
    /// <remarks>
    /// The handle is this process's already -- the engine duplicated it into us
    /// -- and stays ours to close. The surface holds it for as long as it is
    /// alive, so it is closed when the host is cleared and not before.
    /// </remarks>
    public bool Attach(FrameworkElement host, nint handle, out string why)
    {
        why = string.Empty;
        if (handle == 0)
        {
            why = "there is no surface to show";
            return false;
        }

        Compositor compositor = ElementCompositionPreview.GetElementVisual(host).Compositor;
        nint compositorPtr = MarshalInspectable<Compositor>.FromManaged(compositor);
        if (compositorPtr == 0)
        {
            why = "this window's compositor would not come back as an object";
            return false;
        }

        // **What is there stays there until the new one is on.** Clearing the
        // host first shows its own background for as long as it takes to build
        // a visual, which the eye reads as a black frame; the old one is
        // replaced by `SetElementChildVisual` below and closed after, once
        // there is something in its place.
        SpriteVisual? previous = _visual;
        CompositionSurfaceBrush? previousBrush = _brush;
        ICompositionSurface? previousSurface = _surface;
        _visual = null;
        _brush = null;
        _surface = null;

        nint interop = 0;
        nint surfacePtr = 0;
        try
        {
            Guid iid = CompositorSwapChainInterop;
            int hr = Marshal.QueryInterface(compositorPtr, in iid, out interop);
            if (hr < 0 || interop == 0)
            {
                why = $"this compositor takes no surface handles (0x{hr:X8})";
                return false;
            }

            var vtable = *(void***)interop;
            var create =
                (delegate* unmanaged[Stdcall]<nint, nint, nint*, int>)
                    vtable[CreateSurfaceForHandleSlot];
            nint made = 0;
            hr = create(interop, handle, &made);
            if (hr < 0 || made == 0)
            {
                why = $"the surface handle was refused (0x{hr:X8})";
                return false;
            }
            surfacePtr = made;

            ICompositionSurface surface =
                MarshalInterface<ICompositionSurface>.FromAbi(surfacePtr);
            CompositionSurfaceBrush brush = compositor.CreateSurfaceBrush(surface);
            // **Uniform, and the letterbox is this side's.** §9.7.1 put the
            // scale in the engine's own shader and left the fit to the shell:
            // the engine fills whatever target it is given, so the shape has to
            // be in the size this window asks for. Uniform is what centres the
            // result in the space around it, and what keeps the picture right
            // in the moments where the two disagree -- between a resize and the
            // engine answering it, and before this window has asked at all.
            brush.Stretch = CompositionStretch.Uniform;
            // Centred rather than pinned to a corner, which is what a letterbox
            // is.
            brush.HorizontalAlignmentRatio = 0.5f;
            brush.VerticalAlignmentRatio = 0.5f;

            SpriteVisual visual = compositor.CreateSpriteVisual();
            visual.Brush = brush;
            // **Sized by `Place`, and by nothing else.** `Size` and
            // `RelativeSizeAdjustment` add: a visual given the host's size *and*
            // a relative adjustment of one is twice the host, and a picture
            // fitted into that shows its top-left quarter, off to one side --
            // which is what this looked like until it was measured. The box the
            // engine renders at is what this visual is, exactly, and the caller
            // says where it goes.
            visual.Size = _placed;
            visual.Offset = _offset;
            ElementCompositionPreview.SetElementChildVisual(host, visual);
            _brush = brush;
            _visual = visual;
            _surface = surface;
            Close(previous, previousBrush, previousSurface);
            return true;
        }
        finally
        {
            // On the way out of a failure the old visual is still on the host
            // and still this object's to close, so it goes back rather than
            // being dropped: a shell that could not attach shows what it had.
            if (_visual is null && previous is not null)
            {
                _visual = previous;
                _brush = previousBrush;
                _surface = previousSurface;
            }
            if (surfacePtr != 0)
            {
                Marshal.Release(surfacePtr);
            }
            if (interop != 0)
            {
                Marshal.Release(interop);
            }
            Marshal.Release(compositorPtr);
        }
    }

    /// <summary>
    /// Takes the picture off, which is what a shell does when the engine says
    /// there is none. The visual goes; the handle is the caller's to close.
    /// </summary>
    public void Detach(FrameworkElement host)
    {
        ElementCompositionPreview.SetElementChildVisual(host, null);
        // **Closed rather than dropped.** These hold the composition surface,
        // and the surface holds the handle; waiting for a collection to say so
        // is waiting for memory pressure that a shell showing one video at a
        // time will not produce.
        Close(_visual, _brush, _surface);
        _visual = null;
        _brush = null;
        _surface = null;
    }

    /// <summary>
    /// Closes one attachment's objects, in the order they hold each other:
    /// the visual, then the brush, then the surface.
    /// </summary>
    private static void Close(SpriteVisual? visual, CompositionSurfaceBrush? brush,
                              ICompositionSurface? surface)
    {
        visual?.Dispose();
        brush?.Dispose();
        // The surface last, because the brush held it. It is a projected
        // interface rather than a class, so whether it closes is a question
        // asked of the object rather than of the type.
        if (surface is IDisposable closable)
        {
            closable.Dispose();
        }
    }
}
