# z-magnifier — Wayland overlay magnifier

A pure Wayland magnifying glass overlay using `wlr-layer-shell` and
`zwlr-screencopy`. Captures the full output once at startup, then
re-samples from the saved frame data on every pointer move, zoom change,
or radius change — no repeated captures.

## Controls

| Input | Action |
|---|---|
| Mouse move | Lens follows cursor |
| Scroll wheel | Zoom in/out (1×–20×, step 0.5) |
| Ctrl + scroll | Lens radius (50–400px, step 10) |
| ESC | Exit |
| Right-click | Exit |

## Build & Run

```sh
nix-shell shell.nix --run "make && ./magnifier"
```

## Dependencies

- Wayland client library
- `wayland-scanner` + `wayland-protocols`
- `pkg-config`

Requires a compositor that supports `wlr-layer-shell-unstable-v1` and
`wlr-screencopy-unstable-v1` (e.g., niri, Sway 1.9+, Hyprland).

## How it works

1. Capture the full output via `zwlr_screencopy_manager_v1.capture_output`
2. Create a full-size overlay surface (anchored all four sides, exclusive zone -1)
3. On each frame callback, render a circular magnified region at cursor position
   using bilinear interpolation from the saved frame data
4. Input region is a rectangle around the lens to pass clicks through to
   windows below
5. Rendering is paced to the compositor's refresh via `wl_surface_frame` —
   only one render per vsync
