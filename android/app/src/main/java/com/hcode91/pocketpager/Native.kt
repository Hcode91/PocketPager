package com.hcode91.pocketpager

object Native {
    init {
        System.loadLibrary("pocketpager")
    }

    // Key codes shared with core/rim/board.h
    const val KEY_ALT = 0x100
    const val KEY_SHIFT = 0x101
    const val KEY_CAP = 0x102
    const val KEY_ESC = 0x103
    const val KEY_WHEEL_CLICK = 0x104
    const val KEY_WHEEL_UP = 0x105
    const val KEY_WHEEL_DOWN = 0x106
    const val KEY_POWER = 0x107

    @JvmStatic external fun create(image: ByteArray, err: Array<String?>): Long
    @JvmStatic external fun destroy(h: Long)
    @JvmStatic external fun reset(h: Long)
    @JvmStatic external fun run(h: Long, us: Int)
    @JvmStatic external fun model(h: Long): Int
    @JvmStatic external fun lcdWidth(h: Long): Int
    @JvmStatic external fun lcdHeight(h: Long): Int
    @JvmStatic external fun render(h: Long, pixels: IntArray): Boolean
    @JvmStatic external fun key(h: Long, key: Int, down: Boolean)
    @JvmStatic external fun status(h: Long): String
    @JvmStatic external fun stopped(h: Long): Boolean
    @JvmStatic external fun drainLog(): String
    @JvmStatic external fun getFlash(h: Long): ByteArray
    @JvmStatic external fun setFlash(h: Long, data: ByteArray): Boolean
    @JvmStatic external fun flashDirty(h: Long): Boolean
    @JvmStatic external fun factory(h: Long)

    const val FLASH_SIZE = 0x800000

    val modelNames = arrayOf("RIM 950", "RIM 850", "RIM 957", "RIM 857")
}
