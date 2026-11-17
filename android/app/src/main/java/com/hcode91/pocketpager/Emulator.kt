package com.hcode91.pocketpager

import android.graphics.Bitmap
import android.os.SystemClock
import java.util.concurrent.ConcurrentLinkedQueue

/** Runs the board on its own thread in real time and publishes LCD frames. */
class Emulator(image: ByteArray) {
    val handle: Long
    val width: Int
    val height: Int
    val modelName: String

    private val pixels: IntArray
    val bitmap: Bitmap
    private val keys = ConcurrentLinkedQueue<Pair<Int, Boolean>>()
    @Volatile private var running = false
    @Volatile var resetRequested = false
    private var thread: Thread? = null

    var onFrame: (() -> Unit)? = null
    @Volatile var status: String = ""
        private set
    @Volatile var stopped = false
        private set

    init {
        val err = arrayOfNulls<String>(1)
        handle = Native.create(image, err)
        if (handle == 0L) throw IllegalArgumentException(err[0] ?: "could not start emulator")
        width = Native.lcdWidth(handle)
        height = Native.lcdHeight(handle)
        modelName = Native.modelNames.getOrElse(Native.model(handle)) { "RIM" }
        pixels = IntArray(width * height)
        bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        Native.render(handle, pixels)
        bitmap.setPixels(pixels, 0, width, 0, 0, width, height)
    }

    fun key(code: Int, down: Boolean) {
        keys.add(code to down)
    }

    fun start() {
        if (running) return
        running = true
        thread = Thread({ loop() }, "PocketPager-emu").also { it.start() }
    }

    fun stop() {
        running = false
        thread?.join(500)
        thread = null
    }

    /** Must be called with the emulator stopped. */
    fun loadFlash(data: ByteArray): Boolean = Native.setFlash(handle, data)

    /** Must be called with the emulator stopped. Returns null if nothing changed. */
    fun dirtyFlash(): ByteArray? = if (Native.flashDirty(handle)) Native.getFlash(handle) else null

    fun factoryReset() = Native.factory(handle)

    fun destroy() {
        stop()
        Native.destroy(handle)
    }

    private fun loop() {
        val frameMs = 16L
        var lastStatus = 0L
        while (running) {
            val t0 = SystemClock.uptimeMillis()
            if (resetRequested) {
                resetRequested = false
                Native.reset(handle)
            }
            while (true) {
                val k = keys.poll() ?: break
                Native.key(handle, k.first, k.second)
            }
            Native.run(handle, (frameMs * 1000).toInt())
            if (Native.render(handle, pixels)) {
                synchronized(bitmap) {
                    bitmap.setPixels(pixels, 0, width, 0, 0, width, height)
                }
                onFrame?.invoke()
            }
            if (t0 - lastStatus > 500) {
                status = Native.status(handle)
                stopped = Native.stopped(handle)
                lastStatus = t0
            }
            val spent = SystemClock.uptimeMillis() - t0
            if (spent < frameMs) Thread.sleep(frameMs - spent)
        }
    }
}
