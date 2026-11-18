# Desktop UI reference

The target is Spotify's modern desktop visual language, not classic OS widgets.
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

Match the black outer chrome, #121212 split panes, 8px panel gaps/radii, compact
library rail, pill-shaped controls/search, bold geometric typography, artwork-led
cards, dense track table and persistent bottom player. Following user feedback,
navigation is immediate with no crossfade, retained page, card lift or sliding play
button. Hover colors settle in 55ms on enter and 35ms on leave. Timers stop when hover effects settle; idle
does not run a permanent render loop. Windows text is composited at native
resolution separately from supersampled shapes/artwork.
Both passes are assembled off-screen and presented with one final window blit;
never present a textless intermediate frame during hover redraws.

Home uses scrollable recent-album shortcuts and saved collection shelves, not
height-compressed cards. The recent albums come from the documented recently
played endpoint, not a reconstruction of Spotify's private Home feed. Artist
cards open album/single browsing; only song selection or explicit player controls
start playback. Queue and Now Playing have toggleable native side panels. Friends
and Lyrics toggles show their public-API limitation rather than invented content.

Fira Sans is bundled under its included OFL license as a freely distributable
typographic substitute. Spotify Mix and Spotify's private Home/recommendation
feed are not bundled or reproduced. Gradient collection icons are fallbacks;
album/playlist/player artwork loads through bounded native decoders. Do not
insert fictitious listening history or claim pixel-identical parity.

Shared layout, drawing commands, hit testing and animation state live in
`src/ui/scene*` and `src/ui/icons.c`. The platform adapters render these directly
with OS-native drawing. The only stock visible text control is the borderless
Windows search editor, integrated into the custom search pill.
