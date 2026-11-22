package com.hcode91.pocketpager

import android.graphics.Bitmap
import android.os.SystemClock

/**
 * Runs the BlackBerry OS on a background thread. Every call into the VM
 * goes through [lock], so input from the UI thread lands between slices.
 */
class BbMachine(private val osDir: String) {
    val lock = Object()
    val bitmap: Bitmap = Bitmap.createBitmap(BbNative.SCREEN_W, BbNative.SCREEN_H, Bitmap.Config.ARGB_8888)
    private val pixels = IntArray(BbNative.SCREEN_W * BbNative.SCREEN_H)
    private var lastFrame = -1
    private var thread: Thread? = null
    @Volatile private var running = false
    @Volatile var stopped = false
        private set
    @Volatile var status = "Starting the OS…"
        private set
    @Volatile var booted = false
        private set

    /** Called (on the VM thread) when the screen changed. */
    var onFrame: (() -> Unit)? = null
    /** Called (on the VM thread) once the OS has finished booting. */
    var onBooted: (() -> Unit)? = null

    private var pendingLaunch: String? = null
    private var launchStep = 0
    private var launchAt = 0L
    var launchState = ""
        private set

    fun start(): String? {
        val err = synchronized(lock) { BbNative.start(osDir) }
        if (err != null) return err
        resume()
        return null
    }

    fun resume() {
        if (running || stopped) return
        running = true
        thread = Thread({ loop() }, "bbos").apply { priority = Thread.MAX_PRIORITY - 1; start() }
    }

    fun pause() {
        running = false
        thread?.join(500)
        thread = null
    }

    /** Starts an application module once the OS is up and brings it to the front. */
    fun launch(module: String) {
        synchronized(lock) {
            pendingLaunch = module
            launchStep = 0
            launchState = "Waiting for the OS…"
        }
    }

    private fun loop() {
        var lastStatus = 0L
        while (running) {
            val rc: Int
            synchronized(lock) {
                rc = BbNative.run(12)
                if (!booted && BbNative.booted()) {
                    booted = true
                    onBooted?.invoke()
                }
                stepLaunch()
                val f = BbNative.render(pixels)
                if (f != lastFrame) {
                    lastFrame = f
                    synchronized(bitmap) { bitmap.setPixels(pixels, 0, BbNative.SCREEN_W, 0, 0, BbNative.SCREEN_W, BbNative.SCREEN_H) }
                    onFrame?.invoke()
                }
            }
            val now = SystemClock.uptimeMillis()
            if (now - lastStatus > 500) {
                lastStatus = now
                status = synchronized(lock) { BbNative.status() }
            }
            if (rc < 0) {
                stopped = true
                running = false
                status = synchronized(lock) { BbNative.status() }
                break
            }
            if (rc == 1) SystemClock.sleep(4)
        }
    }

    /** launch → wait for the process → ask the OS to bring it forward */
    private fun stepLaunch() {
        val m = pendingLaunch ?: return
        if (!booted) return
        val now = SystemClock.uptimeMillis()
        when (launchStep) {
            0 -> {
                BbNative.launch(m)
                launchStep = 1
                launchAt = now
                launchState = "Starting $m…"
            }
            1 -> if (now - launchAt > 600) {
                if (BbNative.pidOf(m) >= 0 && BbNative.foreground(m) >= 0) {
                    pendingLaunch = null
                    launchState = ""
                } else if (now - launchAt > 15000) {
                    pendingLaunch = null
                    launchState = "$m did not start"
                } else launchAt = now - 300   // try again shortly
            }
        }
    }

    // ---- input ------------------------------------------------------------

    fun key(ch: Int, action: Int, status: Int = 0) = synchronized(lock) { BbNative.key(ch, action, status) }
    fun trackball(dx: Int, dy: Int) = synchronized(lock) { BbNative.trackball(dx, dy) }
    fun click(down: Boolean) = synchronized(lock) { BbNative.click(down) }
}
