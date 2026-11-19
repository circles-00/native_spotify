plugins {
    id("com.android.application") version "8.10.1"
    id("org.jetbrains.kotlin.android") version "2.0.21"
    id("org.jetbrains.kotlin.plugin.compose") version "2.0.21"
}

val clientId = Regex("SPOTIFY_CLIENT_ID \"([0-9a-fA-F]{32})\"")
    .find(providers.fileContents(layout.projectDirectory.file("../../CMakeLists.txt")).asText.get())
    ?.groupValues?.get(1) ?: error("Missing SPOTIFY_CLIENT_ID in CMakeLists.txt")
/** Constants of a C enum, by name, so Kotlin never hard-codes the core's numbering. */
fun cEnum(header: String, type: String): List<String> =
    providers.fileContents(layout.projectDirectory.file("../../src/$header")).asText.get()
        .let { Regex("typedef enum \\{([^}]*)\\} $type;").find(it)?.groupValues?.get(1) ?: error("Missing enum $type") }
        .split(',').map(String::trim).filter(String::isNotEmpty)
val coreEnums = listOf("core/browse.h" to "BrowseKind", "core/browse.h" to "ItemKind", "core/browse.h" to "SearchFilter",
    "core/app.h" to "ListSort", "core/app.h" to "Action", "core/engine.h" to "EngineState", "core/core.h" to "RepeatMode",
    "core/lyrics.h" to "LyricsState", "ui/canvas.h" to "Icon").map { (header, type) -> cEnum(header, type) }
val palette = providers.fileContents(layout.projectDirectory.file("../../src/ui/dark_palette.h")).asText.get()

android {
    namespace = "app.nativespotify"
    compileSdk = 35
    ndkVersion = "27.1.12297006"
    defaultConfig {
        applicationId = "app.nativespotify"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters.add("arm64-v8a") }
        buildConfigField("String", "SPOTIFY_CLIENT_ID", "\"$clientId\"")
        coreEnums.forEach { names -> names.forEachIndexed { index, name -> buildConfigField("int", name, "$index") } }
        listOf("BODY", "CHROME", "TILE", "HOVER", "SELECTED", "BORDER", "TEXT", "MUTED", "ACCENT",
            "BRAND", "PROGRESS", "ERROR", "CONTROL", "SHEET", "LIKED_FROM", "LIKED_TO").forEach { role ->
            val rgb = Regex("(?m)^#define DARK_$role 0x([0-9a-fA-F]{6})u$")
                .find(palette)?.groupValues?.get(1) ?: error("Missing desktop palette role $role")
            buildConfigField("long", "DARK_$role", "0xFF${rgb}L")
        }
    }
    buildTypes {
        // Compose is much slower in debug builds; install this one to judge smoothness.
        // It is signed with the local debug key so it upgrades the debug install in place.
        getByName("release") {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    buildFeatures { compose = true; buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    sourceSets["main"].assets.srcDir("../termux")
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }
    defaultConfig.externalNativeBuild { cmake { arguments += "-DSPOTIFY_CLIENT_ID=$clientId" } }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    // Android's org.json is a stub in JVM unit tests; the snapshot parser needs the real one.
    testImplementation("org.json:json:20240303")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation(platform("androidx.compose:compose-bom:2024.12.01"))
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.profileinstaller:profileinstaller:1.4.1")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.7.0")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("io.coil-kt:coil-compose:2.6.0")
}
