# Vendored Dear ImGui

    Upstream: https://github.com/ocornut/imgui
    Version:  v1.92.9
    Commit:   01380c579715e62fb9a8d6ec0502c4ea83bfde6e

Copied in rather than fetched at build time, so the repository builds with no
network access and no package manager, which is the same property the rest of
the project has.

Only what `virtual-xinput-gui` needs is here: the core, and the Win32 + D3D11
backends. `imgui_demo.cpp` is deliberately absent.

## Local modifications

**None.** Configuration is done entirely through compile definitions in
`CMakeLists.txt`, so updating is a straight re-copy of the file list below.

One definition is load-bearing rather than cosmetic:

    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD

`imgui_impl_win32.cpp` otherwise `LoadLibrary`s `xinput1_4.dll`, then
`xinput1_3.dll`, and so on down the list, to drive gamepad UI navigation. Those
are the exact names this project builds. Run the GUI from a folder containing
our own wrapper and ImGui would load it, starting a second DirectInput stack
inside the configurator that is polling the same pads the configurator is
already reading. Disabling it also stops the pad you are configuring from
moving the UI focus while you configure it, which would be unusable regardless.

## Updating

Re-copy these paths from a checkout of the new tag and update the version above:

    imgui.cpp  imgui_draw.cpp  imgui_tables.cpp  imgui_widgets.cpp
    imgui.h  imgui_internal.h  imconfig.h
    imstb_rectpack.h  imstb_textedit.h  imstb_truetype.h
    LICENSE.txt
    backends/imgui_impl_win32.cpp  backends/imgui_impl_win32.h
    backends/imgui_impl_dx11.cpp   backends/imgui_impl_dx11.h
