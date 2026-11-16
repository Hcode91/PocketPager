package com.hcode91.pocketpager

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.view.View

/** Draws the emulated LCD scaled up with sharp pixels and a bezel. */
class LcdView(context: Context) : View(context) {
    var emulator: Emulator? = null
        set(value) {
            field = value
            requestLayout()
            invalidate()
        }

    private val paint = Paint().apply { isFilterBitmap = false; isAntiAlias = false }
    private val bezel = Paint().apply { color = Color.rgb(0x2B, 0x2E, 0x33) }
    private val dst = Rect()

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val emu = emulator
        val aspect = if (emu != null) emu.height.toFloat() / emu.width else 1f
        val h = (w * aspect).toInt() + 32
        setMeasuredDimension(w, h)
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bezel)
        val emu = emulator ?: return
        val pad = 16
        val availW = width - pad * 2
        val availH = height - pad * 2
        val scale = minOf(availW / emu.width, availH / emu.height).coerceAtLeast(1)
        val w = emu.width * scale
        val h = emu.height * scale
        val x = (width - w) / 2
        val y = (height - h) / 2
        dst.set(x, y, x + w, y + h)
        synchronized(emu.bitmap) {
            canvas.drawBitmap(emu.bitmap, null, dst, paint)
        }
    }
}
