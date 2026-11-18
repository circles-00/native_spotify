# Desktop UI reference

The primary layout target is the [1920 × 1200 Spotifast screenshot](https://raw.githubusercontent.com/crmne/spotifast/refs/heads/main/docs/screenshot.png).
The [launch film](https://spotifast.rocks/assets/videos/launch-film.mp4) supplements it for screens not shown in
the still (12.8s for Liked Songs, 18.6s for the queue). The UI is dark only.
Use live account content and controls backed by this client, not sample account data or a simulated spectrum.
The application remains native C with GDI on Windows and Cairo/Pango on Linux.

References fetched and visually inspected during implementation:

- [Modern top-centered Home/Search and playlist table (Spotify Community, 2024)](https://ca.v-cdn.net/6038735/uploads/images/177252iA2572004B5347163/H2theJ_0-1723641806431.png)
- [Official desktop/library/Now Playing reference (Spotify Newsroom, 2023)](https://storage.googleapis.com/pr-newsroom-wp/1/2023/06/FTR-Header-v.2-x2-1920x733.jpg)
- [Official description of the library and Now Playing redesign](https://newsroom.spotify.com/2023-06-20/spotify-desktop-experience-redesign-your-library-now-playing-views-customize/)
- [Official desktop demo video](https://storage.googleapis.com/pr-newsroom-wp/1/2023/06/Desktop-update-compressed.mp4)
  (downloaded and sampled as a storyboard; useful for persistent pane composition,
  not evidence of exact navigation easing or duration).
- [User-supplied Spotify search reference](https://api.stereofox.com/wp-content/uploads/2023/02/search-on-spotify-based-on-label-record.jpg)
  (fetched and inspected: All/Songs/Artists/Albums chips, top-result card beside
  four songs, circular artist covers and album shelves).

Match the sampled charcoal palette, Home/Search/Library rail, compact search pill,
two-column shortcuts with the queue open, artwork shelves, flush queue and bottom
transport. Use the shared dark palette in `src/ui/dark_palette.h`. The reference
viewport is 1280 × 800 logical units at 150% scale.
Following user feedback,
navigation is immediate with no crossfade, retained page, card lift or sliding play
button. Hover colors settle in 55ms on enter and 35ms on leave. Timers stop when hover effects settle; idle
does not run a permanent render loop. Windows text is composited at native
resolution separately from supersampled shapes/artwork.
Both passes are assembled off-screen and presented with one final window blit;
never present a textless intermediate frame during hover redraws.

Home uses Liked Songs and playlist shortcuts, with separate playlist, recent
album, and saved album shelves. The recent albums come from the documented recently
played endpoint, not a reconstruction of Spotify's private Home feed. Artist
cards open album/single browsing; only song selection or explicit player controls
start playback. Queue and Devices have toggleable native side panels. Friends
and Lyrics toggles show their public-API limitation rather than invented content.

Inter is bundled under its included OFL license. Spotify Mix and Spotify's private Home/recommendation
feed are not bundled or reproduced. Gradient collection icons are fallbacks;
album/playlist/player artwork loads through bounded native decoders. Do not
insert fictitious listening history or claim pixel-identical parity.

Shared layout, drawing commands, hit testing and animation state live in
`src/ui/scene*`, `src/ui/controller.c` and `src/ui/icons.c`. The platform
adapters in `src/platform/` render these directly with OS-native drawing,
including the custom-drawn search pill (with clear control when non-empty)
on both Windows and Linux.

Rounded fills and artwork use the same radius contract (`src/ui/canvas.h`). Art
is center-cropped and mapped directly into the rounded shape, without a second
coincident clip. Cairo uses an antialiased fill path; the Windows DIB rasterizer
integrates subpixel spans and intersects clip geometry before blending. Neither
uses a hard-edged region for rounded artwork. Active sidebar/queue/player
controls use Spotify green `#1ed760` without permanent rest timers.
