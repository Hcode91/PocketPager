package com.hcode91.pocketpager

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.io.File

class MainActivity : Activity() {
    private lateinit var lcd: LcdView
    private lateinit var statusView: TextView
    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var logPanel: LinearLayout
    private var emulator: Emulator? = null
    private val ui = Handler(Looper.getMainLooper())
    private val logBuf = StringBuilder()

    private val osFile by lazy { File(filesDir, "os.bin") }
    private val flashFile by lazy { File(filesDir, "flash.bin") }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        if (osFile.exists()) startEmulator(osFile.readBytes()) else showWelcome()
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(0x1A, 0x1C, 0x20))
            setPadding(dp(8), dp(8), dp(8), dp(8))
        }

        val bar = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        bar.addView(toolButton("Load OS") { pickOs() }, weight())
        bar.addView(toolButton("Load Flash") { pickFlash() }, weight())
        bar.addView(toolButton("Reset") { emulator?.resetRequested = true }, weight())
        bar.addView(toolButton("Log") { showLog(true) }, weight())
        root.addView(bar)
        root.addView(toolButton("Bold 9000 (BlackBerry OS 5)") {
            startActivity(Intent(this, BlackBerryActivity::class.java))
        })

        lcd = LcdView(this)
        root.addView(lcd, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(6) })

        statusView = TextView(this).apply {
            setTextColor(Color.rgb(0x9A, 0xA4, 0xB0))
            textSize = 11f
            typeface = Typeface.MONOSPACE
            setPadding(0, dp(4), 0, dp(4))
        }
        root.addView(statusView)

        // Trackwheel and escape
        val wheel = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        wheel.addView(key("ESC", Native.KEY_ESC), weight())
        wheel.addView(key("▲", Native.KEY_WHEEL_UP), weight())
        wheel.addView(key("● Click", Native.KEY_WHEEL_CLICK), weight(1.6f))
        wheel.addView(key("▼", Native.KEY_WHEEL_DOWN), weight())
        root.addView(wheel)

        // QWERTY thumb keyboard
        val rows = listOf("QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM")
        rows.forEachIndexed { i, r ->
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            if (i == 2) row.addView(key("ALT", Native.KEY_ALT), weight(1.4f))
            r.forEach { c -> row.addView(key(c.toString(), c.code), weight()) }
            if (i == 1) row.addView(key("⌫", 8), weight(1.2f))
            if (i == 2) row.addView(key("↵", 13), weight(1.4f))
            root.addView(row)
        }
        val last = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        last.addView(key("CAP", Native.KEY_CAP), weight(1.4f))
        last.addView(key("SPACE", ' '.code), weight(5f))
        last.addView(key("SHIFT", Native.KEY_SHIFT), weight(1.4f))
        root.addView(last)

        // Full-screen log overlay
        logView = TextView(this).apply {
            setTextColor(Color.rgb(0x7F, 0xD9, 0x8C))
            textSize = 10f
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
            setPadding(dp(8), dp(8), dp(8), dp(8))
        }
        logScroll = ScrollView(this).apply { addView(logView) }
        logPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(0xF2, 0, 0, 0))
            visibility = View.GONE
            val top = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            top.addView(toolButton("Close log") { showLog(false) }, weight())
            top.addView(toolButton("Clear") { logBuf.setLength(0); logView.text = "" }, weight())
            addView(top)
            addView(logScroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        }

        val frame = FrameLayout(this)
        frame.addView(ScrollView(this).apply { addView(root) })
        frame.addView(logPanel, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        setContentView(frame)
        ui.post(tick)
    }

    private fun weight(w: Float = 1f) =
        LinearLayout.LayoutParams(0, dp(42), w).apply { setMargins(dp(2), dp(2), dp(2), dp(2)) }

    private fun toolButton(text: String, onClick: () -> Unit) = Button(this).apply {
        this.text = text
        isAllCaps = false
        setOnClickListener { onClick() }
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun key(label: String, code: Int) = TextView(this).apply {
        text = label
        gravity = Gravity.CENTER
        setTextColor(Color.WHITE)
        textSize = if (label.length > 2) 11f else 15f
        background = GradientDrawable().apply {
            cornerRadius = dp(8).toFloat()
            setColor(Color.rgb(0x3A, 0x3F, 0x47))
        }
        setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.alpha = 0.6f
                    emulator?.key(code, true)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    v.alpha = 1f
                    emulator?.key(code, false)
                }
            }
            true
        }
    }

    private val tick = object : Runnable {
        override fun run() {
            val emu = emulator
            if (emu != null) {
                statusView.text = "${emu.modelName} · " +
                    (if (emu.stopped) "STOPPED · " else "") + emu.status
            }
            val newLog = Native.drainLog()
            if (newLog.isNotEmpty()) {
                logBuf.append(newLog)
                if (logBuf.length > 60000) logBuf.delete(0, logBuf.length - 50000)
                logView.text = logBuf
                if (logPanel.visibility == View.VISIBLE)
                    logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
            }
            ui.postDelayed(this, 300)
        }
    }

    private fun showLog(show: Boolean) {
        logPanel.visibility = if (show) View.VISIBLE else View.GONE
        if (show) logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (logPanel.visibility == View.VISIBLE) showLog(false) else super.onBackPressed()
    }

    private fun showWelcome() {
        statusView.text = "Tap \"Load OS\" and choose a RIM OS image (Pager957.bin, Pager950.bin, Pager857.bin or Pager850.bin). " +
            "Then use \"Load Flash\" to import an 8 MB flash image with apps and games installed."
    }

    private fun pick(req: Int) {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        startActivityForResult(i, req)
    }

    private fun pickOs() = pick(REQ_OS)

    private fun pickFlash() {
        if (emulator == null) {
            Toast.makeText(this, "Load an OS image first", Toast.LENGTH_LONG).show()
            return
        }
        pick(REQ_FLASH)
    }

    private fun saveFlash() {
        val data = emulator?.dirtyFlash() ?: return
        try {
            val tmp = File(filesDir, "flash.tmp")
            tmp.writeBytes(data)
            tmp.renameTo(flashFile)
        } catch (e: Exception) {
            Toast.makeText(this, "Could not save flash: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun importFlash(bytes: ByteArray) {
        val emu = emulator ?: return
        if (bytes.size != Native.FLASH_SIZE) {
            Toast.makeText(this, "Not a PocketPager flash image (must be exactly 8 MB)",
                Toast.LENGTH_LONG).show()
            return
        }
        emu.stop()
        if (emu.loadFlash(bytes)) {
            flashFile.writeBytes(bytes)
            Toast.makeText(this, "Flash loaded", Toast.LENGTH_SHORT).show()
        }
        emu.start()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try {
            val bytes = contentResolver.openInputStream(uri)!!.use { it.readBytes() }
            when (requestCode) {
                REQ_OS -> {
                    // A new OS starts from a freshly formatted device.
                    flashFile.delete()
                    if (startEmulator(bytes)) osFile.writeBytes(bytes)
                }
                REQ_FLASH -> importFlash(bytes)
            }
        } catch (e: Exception) {
            Toast.makeText(this, "Could not read file: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun startEmulator(bytes: ByteArray): Boolean {
        emulator?.destroy()
        emulator = null
        return try {
            val emu = Emulator(bytes)
            emu.onFrame = { lcd.postInvalidate() }
            if (flashFile.length() == Native.FLASH_SIZE.toLong())
                emu.loadFlash(flashFile.readBytes())
            emulator = emu
            lcd.emulator = emu
            emu.start()
            true
        } catch (e: Exception) {
            Toast.makeText(this, e.message ?: "Not a valid OS image", Toast.LENGTH_LONG).show()
            false
        }
    }

    override fun onPause() {
        super.onPause()
        emulator?.stop()
        saveFlash()
    }

    override fun onResume() {
        super.onResume()
        emulator?.start()
    }

    override fun onDestroy() {
        ui.removeCallbacks(tick)
        emulator?.destroy()
        emulator = null
        super.onDestroy()
    }

    companion object {
        private const val REQ_OS = 1
        private const val REQ_FLASH = 2
    }
}
