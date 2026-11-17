package com.glammer.mge

import android.content.res.AssetManager
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
    external fun pause()
    external fun resume()
    external fun destroy()
}
