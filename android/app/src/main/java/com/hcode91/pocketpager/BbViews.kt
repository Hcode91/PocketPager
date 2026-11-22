package com.hcode91.pocketpager

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs

/** The handheld's 480x320 screen, scaled to the view's width. */
class BbScreenView(context: Context) : View(context) {
    var machine: BbMachine? = null
    private val paint = Paint().apply { isFilterBitmap = true }
    private val frame = Paint().apply { color = Color.BLACK }
    private val dst = Rect()

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        setMeasuredDimension(w, w * BbNative.SCREEN_H / BbNative.SCREEN_W)
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), frame)
        val m = machine ?: return
        dst.set(0, 0, width, height)
        synchronized(m.bitmap) { canvas.drawBitmap(m.bitmap, null, dst, paint) }
    }
}

/**
 * The trackball: drag to roll it (one step per [stepPx] of movement), tap to
 * press it.
 */
class TrackballView(context: Context, private val stepPx: Float) : View(context) {
    var onRoll: ((Int, Int) -> Unit)? = null
    var onClick: ((Boolean) -> Unit)? = null
    private val ball = Paint(Paint.ANTI_ALIAS_FLAG)
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(0x55, 0x59, 0x60); style = Paint.Style.STROKE; strokeWidth = 4f }
    private var lastX = 0f
    private var lastY = 0f
    private var accX = 0f
    private var accY = 0f
    private var moved = 0f
    private var pressed = false

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        val r = minOf(w, h) / 2f
        ball.shader = RadialGradient(w / 2f - r / 3, h / 2f - r / 3, r * 1.2f,
            Color.rgb(0xE8, 0xEC, 0xF2), Color.rgb(0x6A, 0x70, 0x7A), Shader.TileMode.CLAMP)
    }

    override fun onDraw(canvas: Canvas) {
        val r = minOf(width, height) / 2f - 6
        canvas.drawCircle(width / 2f, height / 2f, r, ball)
        canvas.drawCircle(width / 2f, height / 2f, r, ring)
        if (pressed) canvas.drawCircle(width / 2f, height / 2f, r * 0.35f, ring)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastX = e.x; lastY = e.y; accX = 0f; accY = 0f; moved = 0f
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = e.x - lastX
                val dy = e.y - lastY
                lastX = e.x; lastY = e.y
                accX += dx; accY += dy
                moved += abs(dx) + abs(dy)
                val sx = (accX / stepPx).toInt()
                val sy = (accY / stepPx).toInt()
                if (sx != 0 || sy != 0) {
                    accX -= sx * stepPx; accY -= sy * stepPx
                    onRoll?.invoke(sx, sy)
                }
            }
            MotionEvent.ACTION_UP -> {
                if (moved < stepPx) {
                    pressed = true; invalidate()
                    onClick?.invoke(true)
                    postDelayed({ onClick?.invoke(false); pressed = false; invalidate() }, 60)
                }
            }
        }
        return true
    }
}

/** A rounded key cap. */
class KeyCap(context: Context, val label: String, val alt: String, private val dark: Boolean) : View(context) {
    private val bg = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textAlign = Paint.Align.CENTER }
    private val altTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(0x9C, 0xC8, 0xF0); textAlign = Paint.Align.RIGHT }
    private val box = RectF()
    var down = false
        set(v) { field = v; invalidate() }
    var latched = false
        set(v) { field = v; invalidate() }

    override fun onDraw(canvas: Canvas) {
        val r = height * 0.22f
        bg.color = when {
            down -> Color.rgb(0x7A, 0x80, 0x8A)
            latched -> Color.rgb(0x3D, 0x6F, 0x9E)
            dark -> Color.rgb(0x22, 0x24, 0x28)
            else -> Color.rgb(0x38, 0x3B, 0x41)
        }
        box.set(2f, 2f, width - 2f, height - 2f)
        canvas.drawRoundRect(box, r, r, bg)
        txt.textSize = height * (if (label.length > 2) 0.26f else 0.42f)
        canvas.drawText(label, width / 2f, height / 2f + txt.textSize * 0.36f, txt)
        if (alt.isNotEmpty()) {
            altTxt.textSize = height * 0.24f
            canvas.drawText(alt, width - 6f, altTxt.textSize + 2f, altTxt)
        }
    }
}
