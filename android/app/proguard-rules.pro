# The C platform adapter (src/platform/android_system.c) calls these by name through JNI.
-keep class app.nativespotify.CoreHost {
    public static *** http(java.lang.String, java.lang.String, byte[], byte[], boolean);
    public static *** openUrl(java.lang.String);
    public static *** secretLoad(java.lang.String);
    public static *** secretSave(java.lang.String, byte[]);
    public static *** secretDelete(java.lang.String);
    public static *** sha256(byte[]);
    public static *** wake();
}
-keep class app.nativespotify.CoreHost {
    public static *** engineStart(int, byte[]);
    public static *** engineStop(int);
    public static *** engineCommand(java.lang.String, int);
    public static *** enginePlayback(java.lang.String);
}
