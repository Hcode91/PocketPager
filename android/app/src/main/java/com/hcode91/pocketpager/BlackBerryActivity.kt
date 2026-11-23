package com.hcode91.pocketpager

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.io.File

/**
 * The BlackBerry OS family: the Bold 9000's OS 5.0, imported from RIM's own
 * Windows installer, running on our VM. Runs in its own process (":bbos") so
 * leaving it and coming back boots a fresh device.
 */
class BlackBerryActivity : Activity() {
    private val ui = Handler(Looper.getMainLooper())
    private val osDir by lazy { File(filesDir, "bbos/os") }
    private val prefs by lazy { getSharedPreferences("bbos", MODE_PRIVATE) }
    private var machine: BbMachine? = null
    private var screen: BbScreenView? = null
    private var statusLine: TextView? = null
    private var running = false
    private var shift = false
    private var alt = false
    private val caps = mutableMapOf<String, KeyCap>()

    /** Games that come with the OS, shown when their module is present. */
    private val builtIn = listOf(
        "BrickBreaker" to "net_rim_device_apps_games_brickbreaker",
        "Word Mole" to "net_rim_device_apps_games_wordmole",
        "Klondike" to "Klondike_v46_480x320",
        "Sudoku" to "Sudoku_v46_480x320",
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // a launch request without a game is for the pagers
        if (intent?.action == POCKET_LAUNCH && intent.getStringExtra("game_id").isNullOrBlank()) {
            startActivity(Intent(this, MainActivity::class.java))
            finish()
            return
        }
        if (!osInstalled()) showImport() else {
            bootMachine()
            val want = intent?.getStringExtra("game_id")
            val m = want?.let { findModule(it) }
            if (m != null) play(m.first, m.second) else showLibrary()
        }
    }

    private fun osInstalled() = File(osDir, "net_rim_cldc.cod").exists()
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun dpf(v: Float) = v * resources.displayMetrics.density

    // ---- the library ------------------------------------------------------

    private fun apps(): List<Pair<String, String>> {
        val list = builtIn.filter { File(osDir, it.second + ".cod").exists() }.toMutableList()
        prefs.getStringSet("apps", emptySet())!!.sorted().forEach { e ->
            val (label, module) = e.split('\t').let { if (it.size == 2) it[0] to it[1] else it[0] to it[0] }
            if (File(osDir, "$module.cod").exists() && list.none { it.second == module }) list.add(label to module)
        }
        return list
    }

    private fun norm(s: String) = s.lowercase().filter { it.isLetterOrDigit() }
    private fun findModule(id: String): Pair<String, String>? {
        val n = norm(id)
        return apps().firstOrNull { norm(it.first) == n || norm(it.second) == n }
            ?: apps().firstOrNull { norm(it.second).contains(n) || norm(it.first).contains(n) }
    }

    private fun page(title: String): LinearLayout {
        running = false
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(0x14, 0x16, 0x1A))
            setPadding(dp(16), dp(16), dp(16), dp(16))
        }
        root.addView(TextView(this).apply {
            text = title
            textSize = 22f
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setPadding(0, 0, 0, dp(10))
        })
        setContentView(ScrollView(this).apply { addView(root) })
        return root
    }

    private fun note(root: LinearLayout, text: String) = TextView(this).apply {
        this.text = text
        textSize = 14f
        setTextColor(Color.rgb(0xB0, 0xB8, 0xC4))
        setPadding(0, dp(4), 0, dp(10))
        root.addView(this)
    }

    private fun button(root: ViewGroup, text: String, onClick: () -> Unit) = Button(this).apply {
        this.text = text
        isAllCaps = false
        setOnClickListener { onClick() }
        root.addView(this, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(4) })
    }

    private fun showLibrary() {
        val root = page("Bold 9000 · OS 5.0")
        statusLine = note(root, machine?.status ?: "")
        val list = apps()
        if (list.isEmpty()) note(root, "No games found. Add some with \"Add apps\".")
        list.forEach { (label, module) ->
            button(root, "▶  $label") { play(label, module) }
        }
        note(root, "")
        button(root, "Add apps (.cod or .zip)") { pick(REQ_APPS) }
        button(root, "Re-import the OS installer") { pick(REQ_OS) }
        button(root, "Back to the RIM pagers") {
            startActivity(Intent(this, MainActivity::class.java))
            finish()
        }
        ui.post(statusTick)
    }

    private val statusTick = object : Runnable {
        override fun run() {
            val m = machine ?: return
            val s = if (m.launchState.isNotEmpty()) m.launchState else m.status
            statusLine?.text = if (m.stopped) "Stopped · $s" else if (!m.booted) "Starting the OS… $s" else s
            ui.postDelayed(this, 400)
        }
    }

    // ---- importing --------------------------------------------------------

    private fun showImport(msg: String? = null) {
        val root = page("Bold 9000 · OS 5.0")
        note(root, "This runs RIM's own handheld OS. Import it from RIM's Windows installer for the Bold 9000 " +
            "(the 130 MB \"9000M_PBr5.0.0_…_Wind.exe\"). The modules are unpacked into the app; the installer " +
            "itself is not kept.")
        if (msg != null) note(root, msg).setTextColor(Color.rgb(0xFF, 0x9A, 0x8A))
        button(root, "Import OS installer (.exe)") { pick(REQ_OS) }
        button(root, "Back to the RIM pagers") {
            startActivity(Intent(this, MainActivity::class.java))
            finish()
        }
    }

    private fun pick(req: Int) {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }, req)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        when (requestCode) {
            REQ_OS -> importOs(uri)
            REQ_APPS -> importApps(uri)
        }
    }

    private fun copyToCache(uri: Uri, name: String): File {
        val f = File(cacheDir, name)
        contentResolver.openInputStream(uri)!!.use { inp -> f.outputStream().use { inp.copyTo(it, 1 shl 20) } }
        return f
    }

    private fun importOs(uri: Uri) {
        val root = page("Importing the OS")
        val progress = note(root, "Copying the installer…")
        Thread {
            var result: String
            try {
                val exe = copyToCache(uri, "installer.exe")
                val tmp = File(filesDir, "bbos/os.new").apply { deleteRecursively(); mkdirs() }
                val poll = object : Runnable {
                    override fun run() { progress.text = BbNative.importProgress(); ui.postDelayed(this, 300) }
                }
                ui.post(poll)
                result = BbNative.importInstaller(exe.path, tmp.path, cacheDir.path)
                ui.removeCallbacks(poll)
                exe.delete()
                if (result.startsWith("ok:")) {
                    osDir.deleteRecursively()
                    if (!tmp.renameTo(osDir)) result = "error:could not move the modules into place"
                } else tmp.deleteRecursively()
            } catch (e: Exception) {
                result = "error:${e.message}"
            }
            ui.post {
                if (result.startsWith("ok:")) {
                    Toast.makeText(this, "Imported ${result.substring(3)} modules", Toast.LENGTH_LONG).show()
                    restart()
                } else showImport("Import failed: ${result.removePrefix("error:")}")
            }
        }.start()
    }

    private fun importApps(uri: Uri) {
        Thread {
            val before = osDir.list()?.toSet() ?: emptySet()
            val result = try {
                val f = copyToCache(uri, "app.bin")
                BbNative.importModules(f.path, osDir.path).also { f.delete() }
            } catch (e: Exception) { "error:${e.message}" }
            val added = (osDir.list()?.toSet() ?: emptySet()) - before
            // the application is the module without a "-N" sibling suffix
            val mains = added.map { it.removeSuffix(".cod") }.filter { !Regex("-\\d+$").matches(it) }.sorted()
            ui.post {
                if (!result.startsWith("ok:")) {
                    Toast.makeText(this, result.removePrefix("error:"), Toast.LENGTH_LONG).show()
                    return@post
                }
                if (mains.isEmpty()) { Toast.makeText(this, "Modules updated", Toast.LENGTH_SHORT).show(); return@post }
                AlertDialog.Builder(this)
                    .setTitle("Which module starts the app?")
                    .setItems(mains.toTypedArray()) { _, i ->
                        val set = prefs.getStringSet("apps", emptySet())!!.toMutableSet()
                        set.add("${prettyName(mains[i])}\t${mains[i]}")
                        prefs.edit().putStringSet("apps", set).apply()
                        Toast.makeText(this, "Added ${mains[i]}. Restarting the OS…", Toast.LENGTH_SHORT).show()
                        restart()
                    }
                    .show()
            }
        }.start()
    }

    /** "Assassin_sCreed2_Blackberry9000_EN_IGP_EU_222" -> "Assassin sCreed2" */
    private fun prettyName(module: String): String {
        val cut = module.split('_').takeWhile { p ->
            !p.lowercase().startsWith("blackberry") && !Regex("v?\\d{2,}.*|\\d+x\\d+|[A-Z]{2}").matches(p)
        }
        return cut.joinToString(" ").ifBlank { module }
    }

    /** New modules or a new OS need a fresh boot: restart this process. */
    private fun restart() {
        startActivity(Intent(this, BlackBerryActivity::class.java))
        finish()
        Process.killProcess(Process.myPid())
    }

    // ---- running ----------------------------------------------------------

    private fun bootMachine() {
        if (machine != null) return
        val m = BbMachine(osDir.path)
        m.onFrame = { screen?.postInvalidate() }
        val err = m.start()
        if (err != null) { showImport("The OS could not start: $err"); return }
        machine = m
    }

    private fun play(label: String, module: String) {
        val m = machine ?: return
        m.launch(module)
        showDevice(label)
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun showDevice(label: String) {
        val m = machine ?: return
        running = true
        caps.clear()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                intArrayOf(Color.rgb(0x2A, 0x2C, 0x30), Color.rgb(0x0E, 0x0F, 0x11)))
            setPadding(dp(10), dp(10), dp(10), dp(6))
        }
        val bar = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        statusLine = TextView(this).apply { textSize = 11f; setTextColor(Color.rgb(0x9A, 0xA4, 0xB0)); text = label }
        bar.addView(statusLine, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        bar.addView(Button(this).apply {
            text = "Games"; isAllCaps = false; textSize = 12f
            setOnClickListener { showLibrary() }
        })
        root.addView(bar)

        screen = BbScreenView(this).apply { machine = m }
        val bezel = FrameLayout(this).apply {
            setPadding(dp(6), dp(6), dp(6), dp(6))
            background = GradientDrawable().apply { cornerRadius = dpf(10f); setColor(Color.BLACK) }
            addView(screen)
        }
        root.addView(bezel)

        // Send · Menu · trackball · Escape · End
        val nav = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        nav.addView(navKey("📞", Color.rgb(0x2E, 0x8B, 0x57), BbNative.KEY_SEND), LinearLayout.LayoutParams(0, dp(54), 1f))
        nav.addView(navKey("☰", Color.rgb(0x3A, 0x3D, 0x44), BbNative.KEY_MENU), LinearLayout.LayoutParams(0, dp(54), 1f))
        val ball = TrackballView(this, dpf(14f)).apply {
            onRoll = { dx, dy -> m.trackball(dx, dy) }
            onClick = { d -> m.click(d) }
        }
        nav.addView(ball, LinearLayout.LayoutParams(0, dp(92), 1.3f))
        nav.addView(navKey("⤺", Color.rgb(0x3A, 0x3D, 0x44), BbNative.KEY_ESCAPE), LinearLayout.LayoutParams(0, dp(54), 1f))
        nav.addView(navKey("⏻", Color.rgb(0xA8, 0x32, 0x32), BbNative.KEY_END), LinearLayout.LayoutParams(0, dp(54), 1f))
        root.addView(nav, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(8); bottomMargin = dp(6) })

        // the QWERTY keyboard, with the Bold's number and symbol keys on Alt
        val rows = listOf(
            listOf("q#", "w1", "e2", "r3", "t(", "y)", "u_", "i-", "o\"", "p@"),
            listOf("a*", "s4", "d5", "f6", "g/", "h:", "j;", "k'", "l?", "⌫"),
            listOf("ALT", "z7", "x8", "c9", "v?", "b!", "n,", "m.", "$", "↵"),
            listOf("⇧", "0", "SPACE", ".", "⇧"),
        )
        for (r in rows) {
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            for (k in r) {
                val w = when (k) { "SPACE" -> 4.4f; "⇧", "ALT", "↵", "⌫" -> 1.15f; else -> 1f }
                row.addView(keyCap(k), LinearLayout.LayoutParams(0, dp(46), w))
            }
            root.addView(row)
        }
        setContentView(root)
        ui.removeCallbacks(statusTick)
        ui.post(statusTick)
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun navKey(label: String, color: Int, code: Int) = TextView(this).apply {
        text = label
        gravity = Gravity.CENTER
        textSize = 20f
        setTextColor(Color.WHITE)
        background = GradientDrawable().apply { cornerRadius = dpf(14f); setColor(color) }
        setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { v.alpha = 0.6f; machine?.key(code, BbNative.DOWN) }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { v.alpha = 1f; machine?.key(code, BbNative.UP) }
            }
            true
        }
    }.also { (it.layoutParams as? ViewGroup.MarginLayoutParams)?.setMargins(dp(3), 0, dp(3), 0) }

    @SuppressLint("ClickableViewAccessibility")
    private fun keyCap(spec: String): KeyCap {
        val special = spec in setOf("ALT", "⌫", "↵", "⇧", "SPACE")
        val label = if (special || spec.length == 1) spec else spec.substring(0, 1).uppercase()
        val altCh = if (!special && spec.length == 2) spec.substring(1) else ""
        val cap = KeyCap(this, label, altCh, special)
        caps[spec] = cap
        cap.setOnTouchListener { _, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { cap.down = true; press(spec, BbNative.DOWN) }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { cap.down = false; press(spec, BbNative.UP) }
            }
            true
        }
        return cap
    }

    /** One on-screen key; Shift and Alt latch for the next key. */
    private fun press(spec: String, action: Int) {
        val m = machine ?: return
        when (spec) {
            "⇧" -> { if (action == BbNative.UP) { shift = !shift; updateLatches() }; return }
            "ALT" -> { if (action == BbNative.UP) { alt = !alt; updateLatches() }; return }
        }
        val ch = when (spec) {
            "⌫" -> BbNative.KEY_BACKSPACE
            "↵" -> BbNative.KEY_ENTER
            "SPACE" -> ' '.code
            else -> {
                val base = spec[0]
                when {
                    alt && spec.length == 2 -> spec[1].code
                    shift -> base.uppercaseChar().code
                    else -> base.code
                }
            }
        }
        m.key(ch, action)
        if (action == BbNative.UP && (shift || alt)) { shift = false; alt = false; updateLatches() }
    }

    private fun updateLatches() {
        caps["⇧"]?.latched = shift
        caps["ALT"]?.latched = alt
        caps.values.filter { it.label == "⇧" }.forEach { it.latched = shift }
    }

    // ---- hardware keys and gamepads --------------------------------------

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val m = machine
        if (!running || m == null) return super.dispatchKeyEvent(event)
        val code = event.keyCode
        if (code == KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event)
        val down = event.action == KeyEvent.ACTION_DOWN
        val up = event.action == KeyEvent.ACTION_UP
        if (!down && !up) return super.dispatchKeyEvent(event)
        val action = if (down) (if (event.repeatCount > 0) BbNative.REPEAT else BbNative.DOWN) else BbNative.UP
        when (code) {
            KeyEvent.KEYCODE_DPAD_LEFT -> { if (down) m.trackball(-1, 0); return true }
            KeyEvent.KEYCODE_DPAD_RIGHT -> { if (down) m.trackball(1, 0); return true }
            KeyEvent.KEYCODE_DPAD_UP -> { if (down) m.trackball(0, -1); return true }
            KeyEvent.KEYCODE_DPAD_DOWN -> { if (down) m.trackball(0, 1); return true }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_BUTTON_A -> { if (event.repeatCount == 0) m.click(down); return true }
            KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_ESCAPE -> { m.key(BbNative.KEY_ESCAPE, action); return true }
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> { m.key(BbNative.KEY_MENU, action); return true }
            KeyEvent.KEYCODE_BUTTON_X -> { m.key(' '.code, action); return true }
            KeyEvent.KEYCODE_ENTER -> { m.key(BbNative.KEY_ENTER, action); return true }
            KeyEvent.KEYCODE_DEL -> { m.key(BbNative.KEY_BACKSPACE, action); return true }
        }
        val ch = event.getUnicodeChar(event.metaState)
        if (ch > 0) { m.key(ch, action); return true }
        return super.dispatchKeyEvent(event)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        val want = intent.getStringExtra("game_id") ?: return
        val m = findModule(want)
        if (m != null && machine != null) play(m.first, m.second)
        else Toast.makeText(this, "No game called \"$want\"", Toast.LENGTH_LONG).show()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (running) showLibrary() else super.onBackPressed()
    }

    override fun onPause() {
        super.onPause()
        machine?.pause()
    }

    override fun onResume() {
        super.onResume()
        machine?.resume()
    }

    override fun onDestroy() {
        ui.removeCallbacksAndMessages(null)
        machine?.pause()
        super.onDestroy()
        // one OS per process: start clean next time
        if (isFinishing) Process.killProcess(Process.myPid())
    }

    companion object {
        private const val REQ_OS = 11
        private const val REQ_APPS = 12
        const val POCKET_LAUNCH = "com.hcode91.pocket.LAUNCH_GAME"
    }
}
