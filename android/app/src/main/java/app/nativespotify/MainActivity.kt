package app.nativespotify

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.BackEventCompat
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.PredictiveBackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.EnterExitState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.Coil
import coil.ImageLoader

class MainActivity : ComponentActivity() {
    private val setupOpen = mutableStateOf(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // The shared C core is process-wide; recreating this activity reuses the running session.
        Core.start(applicationContext)
        setupOpen.value = savedInstanceState?.getBoolean("setupOpen") ?: false
        enableEdgeToEdge()

        // Artwork fades in as it arrives instead of popping in.
        Coil.setImageLoader(ImageLoader.Builder(this).crossfade(true).build())
        setContent { NativeTheme { App() } }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putBoolean("setupOpen", setupOpen.value)
    }

    override fun onStart() {
        super.onStart()
        Core.setVisible(true)
    }

    override fun onStop() {
        Core.setVisible(false)
        super.onStop()
    }

    override fun onResume() {
        super.onResume()

        // Returning from Termux or Android's permission screen: check again without a tap.
        if (Engine.state.value.step == SetupStep.Termux) {
            Engine.refresh(this)
        }
    }

    @Composable
    private fun App() {
        val core by Core.state.collectAsState()
        val setup by Engine.state.collectAsState()
        var setupShown by rememberSaveable { mutableStateOf(false) }
        var engineChecked by rememberSaveable { mutableStateOf(false) }
        var seenSetupRevision by rememberSaveable { mutableStateOf(-1) }
        var hasKey by remember(setup) { mutableStateOf(Engine.hasKey(this@MainActivity)) }
        val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                Engine.refresh(this@MainActivity)
                return@rememberLauncherForActivityResult
            }

            // Android stops showing the prompt after repeated denials; the toggle is in app settings.
            startActivity(Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", packageName, null)))
        }
        val ready = core.started && core.authenticated && !core.loadingIntro

        // After sign-in, check playback setup once; the core owns the session itself.
        LaunchedEffect(core.authenticated) {
            if (core.authenticated && !engineChecked) {
                engineChecked = true
                Engine.refresh(this@MainActivity)
            }
        }

        // Open first-time setup once on its own when the check finds something to do.
        LaunchedEffect(setup.step, ready) {
            val needsUser = setup.step != SetupStep.Ready && setup.step != SetupStep.Checking

            if (ready && needsUser && !setupShown) {
                setupShown = true
                setupOpen.value = true
            }
        }

        // The core asks for setup when playback needs it, as the desktop opens its setup panel.
        LaunchedEffect(core.setupRevision, core.started) {
            if (!core.started) {
                return@LaunchedEffect
            }

            if (seenSetupRevision >= 0 && core.setupRevision != seenSetupRevision) {
                Engine.refresh(this@MainActivity)
                setupOpen.value = true
            }

            seenSetupRevision = core.setupRevision
        }

        if (!core.started || core.loadingIntro) {
            IntroScreen(core.authenticated)
            return
        }

        if (!core.authenticated) {
            LoginScreen(core.connecting, core.status, Core::connect)
            return
        }

        if (setupOpen.value) {
            BackHandler { setupOpen.value = false }
            SetupScreen(setup, hasKey, SetupActions(
                grantTermux = { permission.launch(Termux.permission) },
                getTermux = { openUrl("https://f-droid.org/packages/com.termux/") },
                openTermux = ::openTermux,
                copySetup = {
                    getSystemService(ClipboardManager::class.java)
                        .setPrimaryClip(ClipData.newPlainText("Termux setup", Termux.setupScript(this@MainActivity)))
                    Engine.state.value = setup.copy(
                        message = "Setup command copied. Open Termux, long-press, choose Paste, then press Enter.",
                        error = false)
                },
                getKey = { openUrl("https://developer.spotify.com/dashboard/soloist") },
                pasteKey = {
                    val clipboard = getSystemService(ClipboardManager::class.java)
                    val text = clipboard.primaryClip?.takeIf { it.itemCount > 0 }
                        ?.getItemAt(0)?.coerceToText(this@MainActivity)?.toString().orEmpty()
                    if (Engine.saveKey(this@MainActivity, text) && Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                        clipboard.clearPrimaryClip()
                    }
                },
                openSpotify = {
                    packageManager.getLaunchIntentForPackage("com.spotify.music")?.let(::startActivity)
                        ?: run { Engine.state.value = setup.copy(message = "Install Spotify to pair this phone.", error = true) }
                },
            ), retry = { Engine.refresh(this@MainActivity) }, install = { Engine.install(this@MainActivity) },
                pair = { Engine.pair(this@MainActivity) }, forgetKey = {
                    Core.engineStop()
                    Engine.forgetKey(this@MainActivity)
                    hasKey = false
                }, close = { setupOpen.value = false })
            return
        }

        Shell(core, setup) {
            // Like the desktop, logging out also forgets the Soloist key and stops the engine.
            Core.disconnect()
            engineChecked = false
            setupShown = false
        }
    }

    /** Keeps the data a page showed while it animates out, so it never swaps content mid-transition. */
    private class Frozen(var state: CoreState)

    @Composable
    private fun Shell(core: CoreState, setup: EngineSetup, logOut: () -> Unit) {
        var tab by rememberSaveable { mutableStateOf("Home") }
        var stacksText by rememberSaveable { mutableStateOf("{}") }
        val stacks = remember(stacksText) { decodeStacks(stacksText) }
        var query by rememberSaveable { mutableStateOf("") }
        var filter by rememberSaveable { mutableStateOf(BuildConfig.SEARCH_ALL) }
        var librarySection by rememberSaveable { mutableStateOf(LIBRARY_PLAYLISTS) }
        var selected by remember { mutableStateOf<Item?>(null) }
        var sleepTimerOpen by rememberSaveable { mutableStateOf(false) }
        var accountMenu by remember { mutableStateOf(false) }
        var devicesOpen by remember { mutableStateOf(false) }
        val homeList = rememberLazyListState()

        // Tells the screen transition which way to move; set only by back navigation.
        var backNav by remember { mutableStateOf(false) }
        var backProgress by remember { mutableStateOf(0f) }
        var backFromLeft by remember { mutableStateOf(true) }

        fun stack(name: String) = stacks[name].orEmpty()

        fun setStack(name: String, entries: List<Destination>) {
            stacksText = encodeStacks(stacks + (name to entries))
        }

        val top = stack(tab).lastOrNull()
        val screen = top?.screen.orEmpty()

        /** Asks the core again for what a tab's root shows, when it has been replaced since. */
        fun replayRoot(name: String) {
            if (name == "Search" && query.isNotBlank() && spotifyUri(query) == null) {
                Core.search(query.trim(), filter)
            }

            if (name == "Library" && librarySection != LIBRARY_PLAYLISTS) {
                Core.browse(if (librarySection == LIBRARY_ALBUMS) BuildConfig.BROWSE_SAVED_ALBUMS else BuildConfig.BROWSE_FOLLOWED_ARTISTS)
            }
        }

        fun restore(destination: Destination?) {
            if (destination == null) {
                replayRoot(tab)
                return
            }

            if (!showing(destination, Core.state.value.list)) {
                destination.replay()
            }
        }

        fun push(destination: Destination) {
            backNav = false
            setStack(tab, stack(tab) + destination)
            destination.replay()
        }

        fun navigate(name: String) = push(Destination(name))

        fun openItem(item: Item) {
            push(Destination(if (item.kind == BuildConfig.ITEM_ARTIST) "Artist" else "Collection", "open", item.json))
        }

        fun back() {
            backNav = true
            val entries = stack(tab)

            if (entries.isEmpty()) {
                tab = "Home"
                restore(stack("Home").lastOrNull())
                return
            }

            val rest = entries.dropLast(1)
            setStack(tab, rest)
            restore(rest.lastOrNull())
        }

        fun selectTab(name: String) {
            backNav = false

            // Tapping the current tab again returns it to its root, as on other Android apps.
            if (name == tab) {
                if (stack(name).isNotEmpty()) {
                    setStack(name, emptyList())
                    replayRoot(name)
                }
                return
            }

            tab = name
            restore(stack(name).lastOrNull())
        }

        // A pasted link: songs play, albums and playlists open their page.
        fun playLink(uri: String) {
            val (kind, id) = uri.split(':').let { it[1] to it[2] }

            when (kind) {
                "track" -> Core.play(itemStub(BuildConfig.ITEM_TRACK, id, "", ""))
                "episode" -> Core.play(itemStub(BuildConfig.ITEM_EPISODE, id, "", ""))
                "album" -> openItem(itemStub(BuildConfig.ITEM_ALBUM, id, "", ""))
                else -> openItem(itemStub(BuildConfig.ITEM_PLAYLIST, id, "", ""))
            }
        }

        // Songs play through the core inside the list they were picked from, exactly as on desktop.
        val actions = SongActions(play = Core::play, more = { selected = it })

        // The system back gesture previews the pop: the page shrinks and follows the thumb.
        PredictiveBackHandler(stack(tab).isNotEmpty() || tab != "Home") { events ->
            try {
                events.collect { event ->
                    backFromLeft = event.swipeEdge == BackEventCompat.EDGE_LEFT
                    backProgress = event.progress
                }
                back()
            } finally {
                backProgress = 0f
            }
        }

        selected?.let { item ->
            TrackActions(item, dismiss = { selected = null },
                addToQueue = { Core.enqueue(item) },
                toggleLiked = { Core.toggleLike(item) },
                startMix = { push(Destination("Collection", "radio", item.json)) },
                openAlbum = item.albumId.takeIf { it.isNotEmpty() }?.let { id ->
                    { openItem(itemStub(BuildConfig.ITEM_ALBUM, id, item.albumTitle, item.artwork)) }
                },
                openArtist = item.artistId.takeIf { it.isNotEmpty() }?.let { id ->
                    { openItem(itemStub(BuildConfig.ITEM_ARTIST, id, item.detail, "")) }
                },
                sleepTimer = { sleepTimerOpen = true },
                share = {
                    startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_TEXT, "https://open.spotify.com/${kindName(item.kind)}/${item.id}"), null))
                })
        }

        if (sleepTimerOpen) {
            SleepTimerDialog(Core.sleepTimer) { sleepTimerOpen = false }
        }

        val fullScreen = screen in setOf("Now Playing", "Queue", "Lyrics")
        val playback = core.playback

        if (devicesOpen) {
            DeviceSheet(core, setup.step == SetupStep.Ready, openSetup = {
                devicesOpen = false
                Engine.refresh(this@MainActivity)
                setupOpen.value = true
            }, dismiss = { devicesOpen = false })
        }

        // Pages handle the status bar themselves so Now Playing can colour the area behind it.
        Scaffold(containerColor = Palette.body, contentWindowInsets = WindowInsets(0), bottomBar = {
            AnimatedVisibility(!fullScreen, enter = slideInVertically { it } + fadeIn(),
                exit = slideOutVertically { it } + fadeOut()) {
                Column {
                    // The core restores the last session, so the bar can resume it like the desktop player.
                    if (playback.uri.isNotEmpty()) {
                        val position = livePosition(playback, step = 1_000)
                        val device = currentDevice(core)
                        val remote = playback.hasDevice && device?.here != true && playback.deviceId != core.devices.engineId
                        MiniPlayer(
                            title = playback.title,
                            subtitle = if (remote) deviceName(core, device) else miniSubtitle(core),
                            image = playback.artwork,
                            playing = playback.playing,
                            progress = if (playback.durationMs > 0) position.toFloat() / playback.durationMs else 0f,
                            changingTrack = playback.loading,
                            deviceIcon = deviceIcon(device),
                            remote = remote,
                            open = { navigate("Now Playing") },
                            devices = { devicesOpen = true },
                            toggle = { Core.action(BuildConfig.ACTION_TOGGLE_PLAYBACK) })
                        Spacer(Modifier.height(4.dp))
                    }
                    BottomTabs(tab, ::selectTab)
                }
            }
        }) { insets ->
            SideEffect { backNav = false }
            val density = LocalDensity.current

            AnimatedContent(Nav(tab, screen, stack(tab).size), Modifier.fillMaxSize().padding(insets).graphicsLayer {
                // Live preview of the back gesture before it commits.
                val shrink = 1f - 0.08f * backProgress
                scaleX = shrink
                scaleY = shrink
                translationX = (if (backFromLeft) 1f else -1f) * backProgress * with(density) { 24.dp.toPx() }
                shape = RoundedCornerShape(28.dp * backProgress)
                clip = backProgress > 0f
            }, transitionSpec = { navTransition(initialState, targetState, backNav) }, label = "screen") { nav ->
                val frozen = remember { Frozen(core) }

                if (transition.targetState == EnterExitState.Visible) {
                    frozen.state = core
                }

                val page = frozen.state

                // Now Playing is full-bleed with its own top bar, like the Spotify app.
                if (nav.screen == "Now Playing") {
                    NowPlayingScreen(page, ::back, ::openItem, ::navigate, more = { selected = it },
                        devices = { devicesOpen = true })
                    return@AnimatedContent
                }

                Column(Modifier.fillMaxSize().background(Palette.body).statusBarsPadding()
                    .then(if (nav.screen in setOf("Queue", "Lyrics")) Modifier.navigationBarsPadding() else Modifier)
                    .padding(horizontal = 16.dp)) {
                    Row(Modifier.fillMaxWidth().height(64.dp), verticalAlignment = Alignment.CenterVertically) {
                        if (nav.depth > 0) {
                            IconButton(onClick = ::back) {
                                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                            }
                        } else {
                            Box {
                                Avatar(page.profile) { accountMenu = true }
                                DropdownMenu(accountMenu, { accountMenu = false }, Modifier.background(Palette.sheet)) {
                                    DropdownMenuItem(text = { Text("Profile") }, onClick = {
                                        accountMenu = false
                                        push(Destination("Profile", "browse:${BuildConfig.BROWSE_PROFILE_PLAYLISTS}"))
                                    })
                                    DropdownMenuItem(text = { Text("Settings") }, onClick = { accountMenu = false; navigate("Settings") })
                                    DropdownMenuItem(text = { Text("Log out") }, onClick = { accountMenu = false; logOut() })
                                }
                            }
                        }
                        val title = when {
                            nav.screen in setOf("Collection", "Artist", "Profile") -> ""
                            nav.screen.isNotEmpty() -> nav.screen
                            nav.tab == "Home" -> greeting()
                            nav.tab == "Library" -> "Your Library"
                            else -> nav.tab
                        }
                        Text(title, Modifier.weight(1f).padding(start = 8.dp), style = MaterialTheme.typography.headlineSmall,
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }

                    when (nav.screen) {
                        "Queue" -> QueueScreen(page, actions)
                        "Lyrics" -> LyricsScreen(page)
                        "Settings" -> Settings(setup, page.searchRadio, Core::setSearchRadio, { setupOpen.value = true }, logOut)
                        "Profile" -> ProfileScreen(page, actions, ::openItem)
                        "Artist" -> ArtistScreen(page, actions, ::openItem,
                            openLikes = { push(Destination("Collection", "likes", it.json)) })
                        "Collection" -> CollectionScreen(page, actions, ::openItem)
                        else -> when (nav.tab) {
                            "Search" -> SearchScreen(page, query, { query = it }, filter, { filter = it }, actions, ::openItem,
                                playLink = ::playLink)
                            "Library" -> LibraryScreen(page, librarySection, { librarySection = it },
                                openLiked = { push(Destination("Collection", "browse:${BuildConfig.BROWSE_SAVED_TRACKS}")) },
                                actions, ::openItem)
                            else -> HomeScreen(page, homeList,
                                openLiked = { push(Destination("Collection", "browse:${BuildConfig.BROWSE_SAVED_TRACKS}")) },
                                open = ::openItem, openTab = ::selectTab)
                        }
                    }
                }
            }
        }
    }

    @Composable
    private fun Settings(setup: EngineSetup, searchRadio: Boolean, setSearchRadio: (Boolean) -> Unit,
                         openSetup: () -> Unit, logOut: () -> Unit) {
        LazyColumn(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            item {
                Text("Playback on this phone", style = MaterialTheme.typography.titleLarge)
                val summary = when (setup.step) {
                    SetupStep.Ready -> "Ready. Music plays on this phone."
                    SetupStep.Checking -> "Checking the playback engine…"
                    else -> "Setup isn't finished."
                }
                Text(summary, Modifier.padding(vertical = 8.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
                PillButton("Playback setup", filled = false, onClick = openSetup)
            }
            item {
                Row(Modifier.fillMaxWidth().padding(top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("Autoplay a song mix from search", Modifier.weight(1f))
                    Switch(checked = searchRadio, onCheckedChange = setSearchRadio)
                }
                TextButton(onClick = Core::engineStop) { Text("Quit playback engine") }
            }
            item {
                Text("Account", Modifier.padding(top = 12.dp), style = MaterialTheme.typography.titleLarge)
                TextButton(onClick = logOut) { Text("Log out") }
            }
        }
    }

    private fun openUrl(url: String) {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
    }

    private fun openTermux() {
        packageManager.getLaunchIntentForPackage("com.termux")?.let(::startActivity)
            ?: run { Engine.refresh(this) }
    }
}

/** Whether the core's main list already shows this page, so going back need not ask again. */
private fun showing(destination: Destination, list: ListState): Boolean {
    val item = itemFromJson(destination.item)

    return when (destination.request) {
        "" -> true
        "likes" -> list.kind == BuildConfig.BROWSE_ARTIST_LIKES && list.id == item?.id
        "radio" -> list.kind == BuildConfig.BROWSE_SONG_RADIO && list.id == item?.id
        "open" -> list.id == item?.id && list.kind != BuildConfig.BROWSE_ARTIST_LIKES && list.kind != BuildConfig.BROWSE_SONG_RADIO
        else -> list.kind == destination.request.removePrefix("browse:").toIntOrNull()
    }
}

internal fun spotifyUri(input: String): String? {
    val value = input.trim()
    if (value.matches(Regex("spotify:(track|album|playlist|episode):[a-zA-Z0-9]{22}"))) return value
    val uri = Uri.parse(value)
    if (uri.scheme != "https" || uri.host != "open.spotify.com") return null
    val path = uri.pathSegments.takeLast(2)
    if (path.size != 2 || path[0] !in setOf("track", "album", "playlist", "episode") ||
        !path[1].matches(Regex("[a-zA-Z0-9]{22}"))) return null
    return "spotify:${path[0]}:${path[1]}"
}
