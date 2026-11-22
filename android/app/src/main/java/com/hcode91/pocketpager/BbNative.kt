package com.hcode91.pocketpager

/** The BlackBerry OS family: RIM's own Java OS on our VM (core/bbos). */
object BbNative {
    init {
        System.loadLibrary("bbos")
    }

    // key actions
    const val PRESS = 0
    const val DOWN = 1
    const val UP = 2
    const val REPEAT = 3

    // key codes besides plain characters
    const val KEY_ESCAPE = 27
    const val KEY_ENTER = 10
    const val KEY_BACKSPACE = 8
    const val KEY_SEND = 0x1011
    const val KEY_END = 0x1012
    const val KEY_MENU = 0x1002

    /** "ok:N" with the module count, or "error:message". */
    @JvmStatic external fun importInstaller(exe: String, outDir: String, tmpDir: String): String
    @JvmStatic external fun importProgress(): String
    @JvmStatic external fun importModules(path: String, outDir: String): String

    /** Boots the OS from a folder of modules; returns an error message or null. */
    @JvmStatic external fun start(dir: String): String?
    /** Runs up to [ms] milliseconds: 1 when idle, 0 when busy, -1 when stopped. */
    @JvmStatic external fun run(ms: Int): Int
    @JvmStatic external fun booted(): Boolean
    @JvmStatic external fun status(): String
    /** Copies the 480x320 screen; returns its frame counter. */
    @JvmStatic external fun render(pixels: IntArray): Int

    @JvmStatic external fun launch(module: String): Int
    @JvmStatic external fun foreground(module: String): Int
    @JvmStatic external fun pidOf(module: String): Int

    @JvmStatic external fun key(ch: Int, action: Int, status: Int)
    @JvmStatic external fun trackball(dx: Int, dy: Int)
    @JvmStatic external fun click(down: Boolean)

    const val SCREEN_W = 480
    const val SCREEN_H = 320
}
