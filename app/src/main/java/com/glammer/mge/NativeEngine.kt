package com.glammer.mge

import android.content.res.AssetManager
import android.view.MotionEvent
import android.view.Surface

/** Thin JNI facade over the C++ engine. All rendering happens on a native render thread. */
object NativeEngine {
    init { System.loadLibrary("mge") }

    external fun create(assets: AssetManager, internalDataPath: String): Boolean
    external fun surfaceCreated(surface: Surface)
    external fun surfaceChanged(width: Int, height: Int)
    /** Safe-area insets in surface pixels (system bars + display cutout). */
    external fun setInsets(left: Int, top: Int, right: Int, bottom: Int)
    external fun surfaceDestroyed()
    /** Screen density (px per dp) so gesture thresholds feel the same on every display. */
    external fun setDensity(density: Float)
    /**
     * Raw pointer data. action: 0 down, 1 move, 2 up, 3 cancel. For down/up only the pointer at
     * actionIndex changed. xy holds count (x, y) pairs in surface pixels; time is uptimeMillis.
     */
    external fun touch(action: Int, actionIndex: Int, count: Int, ids: IntArray, xy: FloatArray, timeMs: Long)
    /** Latest engine stats (fps, frame time, draws, bodies, memory) as a few short lines. */
    external fun stats(): String
    external fun pause()
    external fun resume()
    external fun destroy()

    private const val MAX_POINTERS = 10
    private val touchIds = IntArray(MAX_POINTERS)
    private val touchXy = FloatArray(MAX_POINTERS * 2)

    /** Forwards a MotionEvent to the engine thread without allocating. Call on the UI thread. */
    fun sendTouch(e: MotionEvent) {
        val action = when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> 0
            MotionEvent.ACTION_MOVE -> 1
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> 2
            MotionEvent.ACTION_CANCEL -> 3
            else -> return
        }
        val n = minOf(e.pointerCount, MAX_POINTERS)
        for (i in 0 until n) {
            touchIds[i] = e.getPointerId(i)
            touchXy[2 * i] = e.getX(i)
            touchXy[2 * i + 1] = e.getY(i)
        }
        touch(action, e.actionIndex, n, touchIds, touchXy, e.eventTime)
    }
}
